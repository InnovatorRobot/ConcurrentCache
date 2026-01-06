#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <list>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>

/**
 * @brief Thread-safe LRU cache with TTL (Time-To-Live) expiration
 *
 * This cache maintains a fixed capacity and automatically evicts entries
 * based on both LRU (Least Recently Used) policy and TTL expiration.
 * A background worker thread periodically sweeps expired entries.
 *
 * @tparam K Key type (must be hashable and equality-comparable)
 * @tparam V Value type (must be copyable or movable)
 */
template <typename K, typename V>
class LruTtlCacheThreadSafe
{
 public:
    using Clock     = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    using Duration  = Clock::duration;

    /**
     * @brief Internal node structure storing value, expiration time, and LRU
     * position
     */
    struct Node
    {
        V value;
        TimePoint expires_at;
        typename std::list<K>::iterator it;  // position in LRU list
    };

    /**
     * @brief Construct a new thread-safe LRU TTL cache
     *
     * @param capacity Maximum number of entries in the cache
     * @param ttl Time-to-live duration for entries
     * @param sweep_interval Interval between background cleanup sweeps
     */
    LruTtlCacheThreadSafe(std::size_t capacity,
                          Duration ttl,
                          Duration sweep_interval = std::chrono::seconds(1)) :
        capacity_(capacity),
        ttl_(ttl),
        sweep_interval_(sweep_interval),
        stop_{false}
    {
        if (capacity == 0)
        {
            throw std::invalid_argument("Capacity must be greater than 0");
        }
        worker_ = std::thread([this] { this->workerLoop(); });
    }

    // no copy for simplicity
    LruTtlCacheThreadSafe(LruTtlCacheThreadSafe const&)            = delete;
    LruTtlCacheThreadSafe& operator=(LruTtlCacheThreadSafe const&) = delete;

    // Move constructor
    LruTtlCacheThreadSafe(LruTtlCacheThreadSafe&& other) noexcept :
        capacity_(other.capacity_),
        ttl_(other.ttl_),
        sweep_interval_(other.sweep_interval_),
        lru_(std::move(other.lru_)),
        map_(std::move(other.map_)),
        stop_(other.stop_.load())
    {
        // Worker thread cannot be moved, so we need to start a new one
        if (!stop_.load())
        {
            worker_ = std::thread([this] { this->workerLoop(); });
        }
    }

    // Move assignment
    LruTtlCacheThreadSafe& operator=(LruTtlCacheThreadSafe&& other) noexcept
    {
        if (this != &other)
        {
            // Stop current worker
            stop_.store(true, std::memory_order_release);
            {
                std::lock_guard<std::mutex> lk(cv_mutex_);
                cv_.notify_all();
            }
            if (worker_.joinable())
                worker_.join();

            // Move data
            capacity_       = other.capacity_;
            ttl_            = other.ttl_;
            sweep_interval_ = other.sweep_interval_;
            lru_            = std::move(other.lru_);
            map_            = std::move(other.map_);
            stop_           = other.stop_.load();

            // Start new worker if needed
            if (!stop_.load())
            {
                worker_ = std::thread([this] { this->workerLoop(); });
            }
        }
        return *this;
    }

    /**
     * @brief Destructor - stops background worker thread
     */
    ~LruTtlCacheThreadSafe()
    {
        // signal worker to stop
        stop_.store(true, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lk(cv_mutex_);
            cv_.notify_all();
        }
        if (worker_.joinable())
            worker_.join();
    }

    // --------- put ---------

    /**
     * @brief Insert or update a key-value pair in the cache
     *
     * @param key The key
     * @param value The value
     */
    void put(K const& key, V const& value)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto now = Clock::now();

        auto m_it = map_.find(key);
        if (m_it != map_.end())
        {
            // update existing
            auto& node      = m_it->second;
            node.value      = value;
            node.expires_at = now + ttl_;
            // move key to front
            lru_.splice(lru_.begin(), lru_, node.it);
            node.it = lru_.begin();
            return;
        }

        // ensure capacity (may also evict expired from tail)
        evictIfNeededLocked(now);

        // insert new
        lru_.push_front(key);
        Node node;
        node.value      = value;
        node.expires_at = now + ttl_;
        node.it         = lru_.begin();
        map_[key]       = std::move(node);
    }

    /**
     * @brief Insert or update a key-value pair using move semantics
     *
     * @param key The key
     * @param value The value (will be moved)
     */
    void put(K const& key, V&& value)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto now = Clock::now();

        auto m_it = map_.find(key);
        if (m_it != map_.end())
        {
            // update existing
            auto& node      = m_it->second;
            node.value      = std::move(value);
            node.expires_at = now + ttl_;
            // move key to front
            lru_.splice(lru_.begin(), lru_, node.it);
            node.it = lru_.begin();
            return;
        }

        // ensure capacity (may also evict expired from tail)
        evictIfNeededLocked(now);

        // insert new
        lru_.push_front(key);
        Node node;
        node.value      = std::move(value);
        node.expires_at = now + ttl_;
        node.it         = lru_.begin();
        map_[key]       = std::move(node);
    }

    // --------- get (optional-style) ---------

    /**
     * @brief Retrieve a value by key
     *
     * @param key The key to look up
     * @return std::optional<V> The value if found and not expired, std::nullopt
     * otherwise
     */
    std::optional<V> get(K const& key)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto m_it = map_.find(key);
        if (m_it == map_.end())
            return std::nullopt;

        auto& node = m_it->second;
        auto now   = Clock::now();

        // expired → drop & report miss
        if (node.expires_at <= now)
        {
            eraseNodeLocked(m_it);
            return std::nullopt;
        }

        // refresh LRU: move to front
        lru_.splice(lru_.begin(), lru_, node.it);
        node.it = lru_.begin();

        return node.value;
    }

    // --------- erase / misc ---------

    /**
     * @brief Remove a key-value pair from the cache
     *
     * @param key The key to remove
     * @return true if the key was found and removed, false otherwise
     */
    bool erase(K const& key)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto m_it = map_.find(key);
        if (m_it == map_.end())
            return false;
        eraseNodeLocked(m_it);
        return true;
    }

    /**
     * @brief Clear all entries from the cache
     */
    void clear()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        map_.clear();
        lru_.clear();
    }

    /**
     * @brief Get the current number of entries in the cache
     *
     * @return std::size_t Number of entries
     */
    std::size_t size() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return map_.size();
    }

    /**
     * @brief Check if the cache is empty
     *
     * @return true if empty, false otherwise
     */
    bool empty() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return map_.empty();
    }

    /**
     * @brief Get the maximum capacity of the cache
     *
     * @return std::size_t Maximum capacity
     */
    std::size_t capacity() const noexcept { return capacity_; }

    /**
     * @brief Get the TTL duration
     *
     * @return Duration TTL duration
     */
    Duration ttl() const noexcept { return ttl_; }

 private:
    using Map   = std::unordered_map<K, Node>;
    using List  = std::list<K>;
    using MapIt = typename Map::iterator;

    // --------- background worker ---------
    void workerLoop()
    {
        std::unique_lock<std::mutex> lk(cv_mutex_);
        while (!stop_.load(std::memory_order_acquire))
        {
            // wait for sweep_interval_ or stop signal
            cv_.wait_for(lk, sweep_interval_, [this] {
                return stop_.load(std::memory_order_acquire);
            });
            if (stop_.load(std::memory_order_acquire))
                break;

            // do a cleanup pass
            auto now = Clock::now();
            std::lock_guard<std::mutex> data_lock(mutex_);
            purgeExpiredLocked(now);
        }
    }

    // --------- internal helpers (must hold mutex_) ---------
    void evictIfNeededLocked(TimePoint now)
    {
        // first, drop expired ones from the back
        purgeExpiredLocked(now);

        // if still over capacity, evict true LRU (back)
        if (map_.size() >= capacity_ && !lru_.empty())
        {
            auto it_key = std::prev(lru_.end());
            auto m_it   = map_.find(*it_key);
            if (m_it != map_.end())
                eraseNodeLocked(m_it);
        }
    }

    void purgeExpiredLocked(TimePoint now)
    {
        // iterate LRU from back (oldest) to front for efficiency
        // Collect expired keys first to avoid iterator invalidation issues
        std::vector<K> to_erase;
        for (auto it = lru_.rbegin(); it != lru_.rend(); ++it)
        {
            auto m_it = map_.find(*it);
            if (m_it == map_.end())
            {
                // inconsistent state - will clean up below
                to_erase.push_back(*it);
                continue;
            }
            if (m_it->second.expires_at <= now)
            {
                to_erase.push_back(*it);
            }
        }
        // Erase all expired/inconsistent entries
        for (auto const& key : to_erase)
        {
            auto m_it = map_.find(key);
            if (m_it != map_.end())
            {
                eraseNodeLocked(m_it);
            }
            else
            {
                // Inconsistent state - remove from list only
                for (auto it = lru_.begin(); it != lru_.end(); ++it)
                {
                    if (*it == key)
                    {
                        lru_.erase(it);
                        break;
                    }
                }
            }
        }
    }

    void eraseNodeLocked(MapIt m_it)
    {
        lru_.erase(m_it->second.it);
        map_.erase(m_it);
    }

 private:
    std::size_t capacity_;
    Duration ttl_;
    Duration sweep_interval_;

    List lru_;  // front = MRU, back = LRU
    Map map_;

    mutable std::mutex mutex_;  // protects map_ + lru_

    // worker control
    std::thread worker_;
    std::atomic<bool> stop_;
    std::condition_variable cv_;
    std::mutex cv_mutex_;
};
