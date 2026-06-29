#ifndef LRU_TTL_CACHE_CACHE_STATS_HPP
#define LRU_TTL_CACHE_CACHE_STATS_HPP

#include <cstdint>

namespace lru_ttl_cache
{

/**
 * @brief Runtime counters useful for production observability.
 *
 * A plain aggregate snapshot returned by LruTtlCacheThreadSafe::stats().
 */
struct CacheStats
{
    std::uint64_t hits{0};         ///< Successful, non-expired lookups.
    std::uint64_t misses{0};       ///< Lookups that found nothing usable.
    std::uint64_t evictions{0};    ///< Entries removed due to capacity.
    std::uint64_t expirations{0};  ///< Entries removed due to TTL.
};

}  // namespace lru_ttl_cache
#endif  // LRU_TTL_CACHE_CACHE_STATS_HPP
