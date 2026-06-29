#ifndef LRU_TTL_CACHE_CACHE_NODE_HPP
#define LRU_TTL_CACHE_CACHE_NODE_HPP

#include <utility>

namespace lru_ttl_cache
{
namespace detail
{

/**
 * @brief Intrusive cache node owned by the map and threaded through two
 *        intrusive doubly-linked lists (LRU order and expiry order).
 *
 * The node stores the value plus a pointer back to its owning map key so the
 * cache can erase the map entry given only a node pointer. Because the map is
 * node-based, both the node address and the key address remain stable.
 *
 * @tparam K         Key type.
 * @tparam V         Value type (need not be default-constructible).
 * @tparam TimePoint Clock time-point type used for expiration.
 */
template <typename K, typename V, typename TimePoint>
struct CacheNode
{
    K const *key{nullptr};  ///< Points at the owning map key (stable address).
    V value;
    TimePoint expires_at{};

    // Intrusive links: LRU list.
    CacheNode *lru_prev{nullptr};
    CacheNode *lru_next{nullptr};

    // Intrusive links: expiry list.
    CacheNode *exp_prev{nullptr};
    CacheNode *exp_next{nullptr};

    template <typename VV>
    CacheNode(VV &&v, TimePoint exp) : value(std::forward<VV>(v)), expires_at(exp)
    {
    }
};

}  // namespace detail
}  // namespace lru_ttl_cache

#endif  // LRU_TTL_CACHE_CACHE_NODE_HPP
