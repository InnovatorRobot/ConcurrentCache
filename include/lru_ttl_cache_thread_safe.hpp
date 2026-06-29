#ifndef LRU_TTL_CACHE_THREAD_SAFE_HPP
#define LRU_TTL_CACHE_THREAD_SAFE_HPP

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>

#include "lru_ttl_cache/cache_node.hpp"
#include "lru_ttl_cache/cache_stats.hpp"
#include "lru_ttl_cache/intrusive_list.hpp"

/**
 * @brief Thread-safe LRU cache with TTL (Time-To-Live) expiration.
 *
 * The cache holds at most @p capacity entries. Entries are evicted either when
 * the capacity is exceeded (least-recently-used victim) or when their TTL
 * elapses. An optional background worker thread periodically purges expired
 * entries so memory is reclaimed even without user traffic.
 *
 * Design / performance
 * --------------------
 * Every entry lives exactly once inside an `std::unordered_map<K, Node>` (the
 * map is node-based, so element addresses stay stable across rehashes). Each
 * node is threaded through two intrusive doubly-linked lists (see
 * lru_ttl_cache::detail::IntrusiveList):
 *   - the LRU list (front = most-recently-used, back = least-recently-used);
 *   - the expiry list, kept in ascending `expires_at` order.
 *
 * Because the TTL is constant for the cache and `get()` does not refresh it,
 * newly inserted/updated entries always have the largest expiration time and
 * are appended to the expiry tail. Purging therefore only walks the expiry
 * *head* and stops at the first live entry — O(k) in the number of expired
 * entries rather than O(n) over the whole cache. All hot-path operations
 * (`put`, `get`, `erase`) are O(1) amortized with no per-call allocations
 * beyond the single map node.
 *
 * Thread safety
 * -------------
 * Every public operation is serialized by a single mutex. Pointers stored in
 * the intrusive lists remain valid for the lifetime of their map node.
 *
 * @tparam K        Key type (hashable via @p Hash, comparable via @p KeyEqual).
 * @tparam V        Value type (copyable or movable).
 * @tparam Hash     Hash functor for keys (defaults to std::hash<K>).
 * @tparam KeyEqual Equality functor for keys (defaults to std::equal_to<K>).
 */
template <typename K,
          typename V,
          typename Hash     = std::hash<K>,
          typename KeyEqual = std::equal_to<K>>
class LruTtlCacheThreadSafe
{
public:
    using Clock     = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    using Duration  = Clock::duration;

    /// Snapshot of runtime counters (see lru_ttl_cache::CacheStats).
    using Stats = lru_ttl_cache::CacheStats;

    /**
     * @brief Construct a thread-safe LRU+TTL cache.
     *
     * @param capacity       Maximum number of live entries (must be > 0).
     * @param ttl            Time-to-live applied to each entry (must be > 0).
     * @param sweep_interval Background purge cadence. Pass `Duration::zero()`
     *                       to disable the background worker entirely (expired
     *                       entries are then reclaimed lazily on access).
     *
     * @throws std::invalid_argument if @p capacity is 0 or @p ttl is <= 0.
     */
    LruTtlCacheThreadSafe(std::size_t capacity,
                          Duration ttl,
                          Duration sweep_interval = std::chrono::seconds(1)) :
        capacity_{capacity}, ttl_{ttl}, sweep_interval_{sweep_interval}
    {
        if (capacity == 0)
        {
            throw std::invalid_argument("Capacity must be greater than 0");
        }
        if (ttl <= Duration::zero())
        {
            throw std::invalid_argument("TTL must be greater than 0");
        }

        map_.reserve(capacity_);

        if (sweep_interval_ > Duration::zero())
        {
            worker_ = std::thread([this] { this->workerLoop(); });
        }
    }

    // Owning a background thread makes copying and moving unsafe; disable both.
    LruTtlCacheThreadSafe(LruTtlCacheThreadSafe const &)            = delete;
    LruTtlCacheThreadSafe &operator=(LruTtlCacheThreadSafe const &) = delete;
    LruTtlCacheThreadSafe(LruTtlCacheThreadSafe &&)                 = delete;
    LruTtlCacheThreadSafe &operator=(LruTtlCacheThreadSafe &&)      = delete;

    /**
     * @brief Stop the background worker and release all entries.
     */
    ~LruTtlCacheThreadSafe()
    {
        stop_.store(true, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lk{cv_mutex_};
            cv_.notify_all();
        }
        if (worker_.joinable())
        {
            worker_.join();
        }
    }

    // --------------------------------------------------------------- put ---

    /**
     * @brief Insert or update an entry (copying the value).
     */
    void put(K const &key, V const &value) { putImpl(key, value); }

    /**
     * @brief Insert or update an entry (moving the value).
     */
    void put(K const &key, V &&value) { putImpl(key, std::move(value)); }

    // --------------------------------------------------------------- get ---

    /**
     * @brief Look up a key, refreshing its LRU position on a hit.
     *
     * @return The stored value if present and not expired, else std::nullopt.
     */
    std::optional<V> get(K const &key)
    {
        std::lock_guard<std::mutex> lock{mutex_};
        auto it = map_.find(key);
        if (it == map_.end())
        {
            ++misses_;
            return std::nullopt;
        }

        Node &node = it->second;
        if (node.expires_at <= Clock::now())
        {
            ++expirations_;
            ++misses_;
            detachNodeLocked(it);
            return std::nullopt;
        }

        lru_.moveToFront(&node);
        ++hits_;
        return node.value;
    }

    /**
     * @brief Look up a key without affecting its LRU position.
     *
     * Useful for read-only inspection that must not promote the entry.
     *
     * @return The stored value if present and not expired, else std::nullopt.
     */
    std::optional<V> peek(K const &key)
    {
        std::lock_guard<std::mutex> lock{mutex_};
        auto it = map_.find(key);
        if (it == map_.end())
        {
            ++misses_;
            return std::nullopt;
        }

        Node &node = it->second;
        if (node.expires_at <= Clock::now())
        {
            ++expirations_;
            ++misses_;
            detachNodeLocked(it);
            return std::nullopt;
        }

        ++hits_;
        return node.value;
    }

    /**
     * @brief Test whether a live (non-expired) entry exists for @p key.
     *
     * Does not update the LRU position. Expired entries are purged on contact.
     */
    bool contains(K const &key)
    {
        std::lock_guard<std::mutex> lock{mutex_};
        auto it = map_.find(key);
        if (it == map_.end())
        {
            return false;
        }
        if (it->second.expires_at <= Clock::now())
        {
            ++expirations_;
            detachNodeLocked(it);
            return false;
        }
        return true;
    }

    // ------------------------------------------------------- erase / misc ---

    /**
     * @brief Remove an entry by key.
     *
     * @return true if a (live or expired) entry was removed, false otherwise.
     */
    bool erase(K const &key)
    {
        std::lock_guard<std::mutex> lock{mutex_};
        auto it = map_.find(key);
        if (it == map_.end())
        {
            return false;
        }
        detachNodeLocked(it);
        return true;
    }

    /**
     * @brief Remove all entries.
     */
    void clear()
    {
        std::lock_guard<std::mutex> lock{mutex_};
        map_.clear();
        lru_.clear();
        exp_.clear();
    }

    /**
     * @brief Current number of stored entries (may include not-yet-purged
     *        expired entries until the next access or sweep).
     */
    std::size_t size() const
    {
        std::lock_guard<std::mutex> lock{mutex_};
        return map_.size();
    }

    /**
     * @brief Whether the cache currently holds no entries.
     */
    bool empty() const
    {
        std::lock_guard<std::mutex> lock{mutex_};
        return map_.empty();
    }

    /**
     * @brief Maximum number of live entries.
     */
    std::size_t capacity() const noexcept { return capacity_; }

    /**
     * @brief Configured time-to-live.
     */
    Duration ttl() const noexcept { return ttl_; }

    /**
     * @brief Snapshot of the runtime counters.
     */
    Stats stats() const
    {
        std::lock_guard<std::mutex> lock{mutex_};
        return Stats{hits_, misses_, evictions_, expirations_};
    }

    /**
     * @brief Reset all runtime counters to zero.
     */
    void resetStats()
    {
        std::lock_guard<std::mutex> lock{mutex_};
        hits_ = misses_ = evictions_ = expirations_ = 0;
    }

private:
    using Node    = lru_ttl_cache::detail::CacheNode<K, V, TimePoint>;
    using Map     = std::unordered_map<K, Node, Hash, KeyEqual>;
    using LruList = lru_ttl_cache::detail::IntrusiveList<Node, &Node::lru_prev, &Node::lru_next>;
    using ExpList = lru_ttl_cache::detail::IntrusiveList<Node, &Node::exp_prev, &Node::exp_next>;

    // ---------------------------------------------------------- put impl ---
    template <typename VV>
    void putImpl(K const &key, VV &&value)
    {
        std::lock_guard<std::mutex> lock{mutex_};
        TimePoint const now = Clock::now();

        auto it = map_.find(key);
        if (it != map_.end())
        {
            Node &node      = it->second;
            node.value      = std::forward<VV>(value);
            node.expires_at = now + ttl_;
            lru_.moveToFront(&node);
            exp_.moveToBack(&node);
            return;
        }

        // Reclaim space (expired first, then true LRU) before inserting.
        evictIfNeededLocked(now);

        auto res   = map_.try_emplace(key, std::forward<VV>(value), now + ttl_);
        Node &node = res.first->second;
        node.key   = &res.first->first;
        lru_.pushFront(&node);
        exp_.pushBack(&node);
    }

    // ----------------------------------------------------- background work ---
    void workerLoop()
    {
        std::unique_lock<std::mutex> lk{cv_mutex_};
        while (!stop_.load(std::memory_order_acquire))
        {
            cv_.wait_for(
                lk, sweep_interval_, [this] { return stop_.load(std::memory_order_acquire); });
            if (stop_.load(std::memory_order_acquire))
            {
                break;
            }

            TimePoint const now = Clock::now();
            std::lock_guard<std::mutex> data_lock{mutex_};
            purgeExpiredLocked(now);
        }
    }

    // ------------------------------------------ helpers (require mutex_) ---
    void evictIfNeededLocked(TimePoint now)
    {
        purgeExpiredLocked(now);

        if (map_.size() >= capacity_ && lru_.tail() != nullptr)
        {
            Node *victim = lru_.tail();
            ++evictions_;
            detachNodeLocked(map_.find(*victim->key));
        }
    }

    void purgeExpiredLocked(TimePoint now)
    {
        // The expiry list is sorted ascending; stop at the first live entry.
        while (exp_.head() != nullptr && exp_.head()->expires_at <= now)
        {
            Node *victim = exp_.head();
            ++expirations_;
            detachNodeLocked(map_.find(*victim->key));
        }
    }

    void detachNodeLocked(typename Map::iterator it)
    {
        Node *node = &it->second;
        lru_.unlink(node);
        exp_.unlink(node);
        map_.erase(it);
    }

private:
    std::size_t const capacity_;
    Duration const ttl_;
    Duration const sweep_interval_;

    Map map_;
    LruList lru_;  // front = most-recently-used, back = least-recently-used
    ExpList exp_;  // front = soonest to expire, back = latest to expire

    // Statistics (guarded by mutex_).
    std::uint64_t hits_{0};
    std::uint64_t misses_{0};
    std::uint64_t evictions_{0};
    std::uint64_t expirations_{0};

    mutable std::mutex mutex_;  // Protects map_ and the intrusive lists.

    // Background worker control.
    std::thread worker_;
    std::atomic<bool> stop_{false};
    std::condition_variable cv_;
    std::mutex cv_mutex_;
};
#endif  // LRU_TTL_CACHE_THREAD_SAFE_HPP
