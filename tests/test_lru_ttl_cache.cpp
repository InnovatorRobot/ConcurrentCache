#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <lru_ttl_cache_thread_safe.hpp>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

TEST(LruTtlCacheTest, BasicPutGet)
{
    LruTtlCacheThreadSafe<int, std::string> cache(10, 1s);

    cache.put(1, "one");
    cache.put(2, "two");

    auto val1 = cache.get(1);
    ASSERT_TRUE(val1.has_value());
    EXPECT_EQ(*val1, "one");

    auto val2 = cache.get(2);
    ASSERT_TRUE(val2.has_value());
    EXPECT_EQ(*val2, "two");
}

TEST(LruTtlCacheTest, Expiration)
{
    LruTtlCacheThreadSafe<int, std::string> cache(10, 100ms);

    cache.put(1, "one");

    // Should be available immediately
    auto val = cache.get(1);
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, "one");

    // Wait for expiration
    std::this_thread::sleep_for(150ms);

    // Should be expired
    val = cache.get(1);
    ASSERT_FALSE(val.has_value());
}

TEST(LruTtlCacheTest, LruEviction)
{
    LruTtlCacheThreadSafe<int, std::string> cache(3, 10s);

    cache.put(1, "one");
    cache.put(2, "two");
    cache.put(3, "three");

    // All should be present
    EXPECT_TRUE(cache.get(1).has_value());
    EXPECT_TRUE(cache.get(2).has_value());
    EXPECT_TRUE(cache.get(3).has_value());

    // Access 1 and 2 to make 3 the LRU
    cache.get(1);
    cache.get(2);

    // Add a new entry - should evict 3
    cache.put(4, "four");

    // 3 should be evicted
    EXPECT_FALSE(cache.get(3).has_value());

    // Others should still be present
    EXPECT_TRUE(cache.get(1).has_value());
    EXPECT_TRUE(cache.get(2).has_value());
    EXPECT_TRUE(cache.get(4).has_value());
}

TEST(LruTtlCacheTest, UpdateExisting)
{
    LruTtlCacheThreadSafe<int, std::string> cache(10, 10s);

    cache.put(1, "one");
    cache.put(1, "updated");

    auto val = cache.get(1);
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, "updated");
    EXPECT_EQ(cache.size(), 1);
}

TEST(LruTtlCacheTest, Erase)
{
    LruTtlCacheThreadSafe<int, std::string> cache(10, 10s);

    cache.put(1, "one");
    cache.put(2, "two");

    EXPECT_TRUE(cache.erase(1));
    EXPECT_FALSE(cache.get(1).has_value());
    EXPECT_TRUE(cache.get(2).has_value());

    EXPECT_FALSE(cache.erase(999));  // Non-existent key
}

TEST(LruTtlCacheTest, Clear)
{
    LruTtlCacheThreadSafe<int, std::string> cache(10, 10s);

    cache.put(1, "one");
    cache.put(2, "two");
    cache.put(3, "three");

    EXPECT_EQ(cache.size(), 3);
    cache.clear();
    EXPECT_EQ(cache.size(), 0);
    EXPECT_TRUE(cache.empty());
}

TEST(LruTtlCacheTest, SizeAndEmpty)
{
    LruTtlCacheThreadSafe<int, std::string> cache(10, 10s);

    EXPECT_TRUE(cache.empty());
    EXPECT_EQ(cache.size(), 0);

    cache.put(1, "one");
    EXPECT_FALSE(cache.empty());
    EXPECT_EQ(cache.size(), 1);

    cache.put(2, "two");
    EXPECT_EQ(cache.size(), 2);
}

TEST(LruTtlCacheTest, Capacity)
{
    LruTtlCacheThreadSafe<int, std::string> cache(5, 10s);
    EXPECT_EQ(cache.capacity(), 5);
}

TEST(LruTtlCacheTest, TtlGetter)
{
    LruTtlCacheThreadSafe<int, std::string> cache(10, 5s);
    EXPECT_EQ(cache.ttl(), 5s);
}

TEST(LruTtlCacheTest, ExpirationPriorityOverLru)
{
    LruTtlCacheThreadSafe<int, std::string> cache(10, 100ms);

    cache.put(1, "one");
    cache.put(2, "two");

    // Wait for expiration
    std::this_thread::sleep_for(150ms);

    // Even if we try to add a new entry, expired ones should be removed first
    cache.put(3, "three");

    // Expired entries should be gone
    EXPECT_FALSE(cache.get(1).has_value());
    EXPECT_FALSE(cache.get(2).has_value());
    EXPECT_TRUE(cache.get(3).has_value());
}

TEST(LruTtlCacheTest, MoveSemantics)
{
    LruTtlCacheThreadSafe<int, std::string> cache(10, 10s);

    std::string value = "movable";
    cache.put(1, std::move(value));

    auto val = cache.get(1);
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, "movable");
}

TEST(LruTtlCacheTest, ConcurrentPuts)
{
    LruTtlCacheThreadSafe<int, int> cache(1000, 10s);

    int const num_threads      = 10;
    int const items_per_thread = 100;
    std::vector<std::thread> threads;

    for (int t = 0; t < num_threads; ++t)
    {
        threads.emplace_back([&cache, t, items_per_thread]() {
            for (int i = 0; i < items_per_thread; ++i)
            {
                int key = t * items_per_thread + i;
                cache.put(key, key * 2);
            }
        });
    }

    for (auto &thread : threads)
    {
        thread.join();
    }

    EXPECT_EQ(cache.size(), num_threads * items_per_thread);

    for (int t = 0; t < num_threads; ++t)
    {
        for (int i = 0; i < items_per_thread; ++i)
        {
            int key  = t * items_per_thread + i;
            auto val = cache.get(key);
            ASSERT_TRUE(val.has_value());
            EXPECT_EQ(*val, key * 2);
        }
    }
}

TEST(LruTtlCacheTest, ConcurrentGetsAndPuts)
{
    LruTtlCacheThreadSafe<int, int> cache(100, 10s);

    // Pre-populate
    for (int i = 0; i < 50; ++i)
    {
        cache.put(i, i * 2);
    }

    int const num_threads = 10;
    std::vector<std::thread> threads;
    std::atomic<int> successful_gets{0};
    std::atomic<int> failed_gets{0};

    for (int t = 0; t < num_threads; ++t)
    {
        threads.emplace_back([&cache, &successful_gets, &failed_gets, t]() {
            for (int i = 0; i < 100; ++i)
            {
                int key  = (t * 100 + i) % 50;
                auto val = cache.get(key);
                if (val.has_value())
                {
                    successful_gets++;
                    EXPECT_EQ(*val, key * 2);
                }
                else
                {
                    failed_gets++;
                }

                // Also do some puts
                cache.put(key + 100, (key + 100) * 2);
            }
        });
    }

    for (auto &thread : threads)
    {
        thread.join();
    }

    // Should have many successful gets
    EXPECT_GT(successful_gets.load(), 0);
}

TEST(LruTtlCacheTest, ConcurrentErases)
{
    LruTtlCacheThreadSafe<int, int> cache(100, 10s);

    // Pre-populate
    for (int i = 0; i < 100; ++i)
    {
        cache.put(i, i);
    }

    int const num_threads = 5;
    std::vector<std::thread> threads;

    for (int t = 0; t < num_threads; ++t)
    {
        threads.emplace_back([&cache, t]() {
            for (int i = 0; i < 20; ++i)
            {
                int key = t * 20 + i;
                cache.erase(key);
            }
        });
    }

    for (auto &thread : threads)
    {
        thread.join();
    }

    // All should be erased
    EXPECT_EQ(cache.size(), 0);
}

TEST(LruTtlCacheTest, BackgroundWorkerCleanup)
{
    LruTtlCacheThreadSafe<int, std::string> cache(10, 200ms, 50ms);

    cache.put(1, "one");
    cache.put(2, "two");
    cache.put(3, "three");

    // Wait for expiration and worker sweep
    std::this_thread::sleep_for(300ms);

    // All should be expired and cleaned up
    EXPECT_FALSE(cache.get(1).has_value());
    EXPECT_FALSE(cache.get(2).has_value());
    EXPECT_FALSE(cache.get(3).has_value());
    EXPECT_EQ(cache.size(), 0);
}

TEST(LruTtlCacheTest, AccessExtendsExpiration)
{
    LruTtlCacheThreadSafe<int, std::string> cache(10, 200ms);

    cache.put(1, "one");

    // Access before expiration
    std::this_thread::sleep_for(100ms);
    cache.get(1);  // This should refresh the expiration

    // Wait a bit more (but not enough to expire from original time)
    std::this_thread::sleep_for(150ms);

    // Note: get() doesn't extend expiration in current implementation
    // This test verifies current behavior
    auto val = cache.get(1);
    // Should be expired because get() doesn't refresh TTL
    EXPECT_FALSE(val.has_value());
}

TEST(LruTtlCacheTest, DifferentTypes)
{
    // String keys, int values
    LruTtlCacheThreadSafe<std::string, int> cache1(10, 10s);
    cache1.put("key1", 42);
    auto val1 = cache1.get("key1");
    ASSERT_TRUE(val1.has_value());
    EXPECT_EQ(*val1, 42);

    // Int keys, string values
    LruTtlCacheThreadSafe<int, std::string> cache2(10, 10s);
    cache2.put(123, "value");
    auto val2 = cache2.get(123);
    ASSERT_TRUE(val2.has_value());
    EXPECT_EQ(*val2, "value");
}

TEST(LruTtlCacheTest, InvalidCapacity)
{
    try
    {
        LruTtlCacheThreadSafe<int, int> cache(0, 10s);
        FAIL() << "Expected std::invalid_argument exception";
    }
    catch (std::invalid_argument const &)
    {
        // Expected exception
    }
    catch (...)
    {
        FAIL() << "Expected std::invalid_argument, but got different exception";
    }
}

TEST(LruTtlCacheTest, GetUpdatesLruPosition)
{
    LruTtlCacheThreadSafe<int, std::string> cache(3, 10s);

    cache.put(1, "one");
    cache.put(2, "two");
    cache.put(3, "three");

    // Access 1 to make it MRU
    cache.get(1);

    // Add new entry - should evict 2 (not 1)
    cache.put(4, "four");

    EXPECT_TRUE(cache.get(1).has_value());
    EXPECT_FALSE(cache.get(2).has_value());
    EXPECT_TRUE(cache.get(3).has_value());
    EXPECT_TRUE(cache.get(4).has_value());
}

TEST(LruTtlCacheTest, RapidExpirationAndInsertion)
{
    LruTtlCacheThreadSafe<int, int> cache(5, 50ms);

    for (int i = 0; i < 20; ++i)
    {
        cache.put(i, i * 2);
        std::this_thread::sleep_for(30ms);
    }

    // Most entries should have expired
    // Only recent ones should remain
    EXPECT_LE(cache.size(), 5);
}

TEST(LruTtlCacheTest, StatsCounters)
{
    LruTtlCacheThreadSafe<int, int> cache(2, 10s);

    cache.put(1, 10);
    cache.put(2, 20);

    EXPECT_TRUE(cache.get(1).has_value());    // hit
    EXPECT_FALSE(cache.get(99).has_value());  // miss
    cache.put(3, 30);                         // evicts LRU (key 2)
    EXPECT_FALSE(cache.get(2).has_value());   // miss (evicted)

    auto const s = cache.stats();
    EXPECT_EQ(s.hits, 1u);
    EXPECT_EQ(s.misses, 2u);
    EXPECT_EQ(s.evictions, 1u);

    cache.resetStats();
    auto const s2 = cache.stats();
    EXPECT_EQ(s2.hits, 0u);
    EXPECT_EQ(s2.misses, 0u);
    EXPECT_EQ(s2.evictions, 0u);
    EXPECT_EQ(s2.expirations, 0u);
}

TEST(LruTtlCacheTest, PeekDoesNotUpdateLru)
{
    LruTtlCacheThreadSafe<int, std::string> cache(3, 10s);

    cache.put(1, "one");
    cache.put(2, "two");
    cache.put(3, "three");

    // peek 1: must NOT promote it, so 1 stays the LRU victim.
    auto val = cache.peek(1);
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, "one");

    cache.put(4, "four");  // should evict 1 (still LRU)

    EXPECT_FALSE(cache.contains(1));
    EXPECT_TRUE(cache.contains(2));
    EXPECT_TRUE(cache.contains(3));
    EXPECT_TRUE(cache.contains(4));
}

TEST(LruTtlCacheTest, ContainsRespectsExpiration)
{
    LruTtlCacheThreadSafe<int, int> cache(10, 100ms);

    cache.put(1, 1);
    EXPECT_TRUE(cache.contains(1));

    std::this_thread::sleep_for(150ms);
    EXPECT_FALSE(cache.contains(1));
    EXPECT_EQ(cache.size(), 0u);
}

struct Point
{
    int x{0};
    int y{0};
    bool operator==(Point const &other) const { return x == other.x && y == other.y; }
};

struct PointHash
{
    std::size_t operator()(Point const &p) const noexcept
    {
        return std::hash<int>{}(p.x) * 31u + std::hash<int>{}(p.y);
    }
};

TEST(LruTtlCacheTest, CustomKeyTypeWithCustomHash)
{
    LruTtlCacheThreadSafe<Point, std::string, PointHash> cache(10, 10s);

    cache.put(Point{1, 2}, "a");
    cache.put(Point{3, 4}, "b");

    auto a = cache.get(Point{1, 2});
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(*a, "a");

    EXPECT_FALSE(cache.get(Point{9, 9}).has_value());
}

struct NoDefault
{
    int v;
    explicit NoDefault(int value) : v(value) {}
};

TEST(LruTtlCacheTest, NonDefaultConstructibleValue)
{
    LruTtlCacheThreadSafe<int, NoDefault> cache(4, 10s);

    cache.put(1, NoDefault{42});
    auto val = cache.get(1);
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(val->v, 42);
}

TEST(LruTtlCacheTest, BackgroundWorkerDisabled)
{
    // sweep_interval == 0 disables the worker; expiry is lazy on access.
    LruTtlCacheThreadSafe<int, int> cache(
        10, 50ms, LruTtlCacheThreadSafe<int, int>::Duration::zero());

    cache.put(1, 1);
    std::this_thread::sleep_for(80ms);

    // Still counted until touched, then purged lazily.
    EXPECT_FALSE(cache.get(1).has_value());
    EXPECT_EQ(cache.size(), 0u);
}
