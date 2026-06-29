# Thread-Safe TTL LRU Cache

A high-performance, thread-safe LRU (Least Recently Used) cache implementation with TTL (Time-To-Live) expiration support in C++17.

## Features

- **Thread-Safe**: All operations are serialized by a single mutex, safe for concurrent access
- **O(1) Hot Path**: `put`, `get`, and `erase` are O(1) amortized with no per-call allocations
- **Efficient Expiry**: Expired entries are purged in O(k) (number of expired items), not O(n) over the whole cache
- **TTL Expiration**: Automatic expiration of entries based on time-to-live
- **LRU Eviction**: Evicts least recently used entries when capacity is reached
- **Background Cleanup**: Optional worker thread periodically sweeps expired entries
- **Any Key/Value Type**: Templated on `<K, V, Hash, KeyEqual>` — use any hashable key, any value
- **Observability**: Built-in hit/miss/eviction/expiration counters via `stats()`
- **Header-Only**: Easy to integrate, just include the header file
- **Modern C++**: Uses C++17 features (std::optional, std::chrono, etc.)

## Requirements

- C++17 or later
- CMake 3.14 or later (for building tests)
- Google Test (automatically downloaded via CMake FetchContent)

## Quick Start

### Basic Usage

```cpp
#include <lru_ttl_cache_thread_safe.hpp>
#include <chrono>

using namespace std::chrono_literals;

// Create a cache with capacity 100, TTL of 5 seconds
LruTtlCacheThreadSafe<int, std::string> cache(100, 5s);

// Insert values
cache.put(1, "value1");
cache.put(2, "value2");

// Retrieve values
auto value = cache.get(1);
if (value.has_value()) {
    std::cout << *value << std::endl;  // Prints "value1"
}

// Check if expired
std::this_thread::sleep_for(6s);
auto expired = cache.get(1);
if (!expired.has_value()) {
    std::cout << "Entry expired!" << std::endl;
}
```

### API Reference

#### Template Parameters

```cpp
template <typename K,
          typename V,
          typename Hash     = std::hash<K>,
          typename KeyEqual = std::equal_to<K>>
class LruTtlCacheThreadSafe;
```

- `K`: Key type (any type hashable by `Hash` and comparable by `KeyEqual`)
- `V`: Value type (copyable or movable; does not need to be default-constructible)
- `Hash` / `KeyEqual`: Provide custom functors to support user-defined key types

#### Constructor

```cpp
LruTtlCacheThreadSafe(std::size_t capacity,
                      Duration ttl,
                      Duration sweep_interval = std::chrono::seconds(1))
```

- `capacity`: Maximum number of entries in the cache (must be > 0)
- `ttl`: Time-to-live duration for entries (must be > 0)
- `sweep_interval`: Interval between background cleanup sweeps (default: 1 second). Pass `Duration::zero()` to disable the background worker and reclaim expired entries lazily on access.

#### Methods

- `void put(K const& key, V const& value)`: Insert or update a key-value pair
- `void put(K const& key, V&& value)`: Insert or update using move semantics
- `std::optional<V> get(K const& key)`: Retrieve a value, refreshing its LRU position (returns `std::nullopt` if not found or expired)
- `std::optional<V> peek(K const& key)`: Retrieve a value *without* updating its LRU position
- `bool contains(K const& key)`: Check for a live entry without promoting it
- `bool erase(K const& key)`: Remove a key-value pair (returns `true` if found and removed)
- `void clear()`: Clear all entries from the cache
- `std::size_t size() const`: Get the current number of entries
- `bool empty() const`: Check if the cache is empty
- `std::size_t capacity() const`: Get the maximum capacity
- `Duration ttl() const`: Get the TTL duration
- `Stats stats() const`: Snapshot of hit/miss/eviction/expiration counters
- `void resetStats()`: Reset all counters to zero

## Building and Testing

### Build the Project

```bash
mkdir build
cd build
cmake ..
cmake --build .
```

### Run Tests

```bash
cd build
ctest
# or run directly
./test_lru_ttl_cache
```

### Build Without Tests

```bash
cmake -DBUILD_TESTS=OFF ..
cmake --build .
```

## Benchmarks & Visualization

The project ships a self-contained benchmark that measures **throughput scaling,
latency percentiles, memory footprint, and hit rate**, plus a Python script that
renders charts from the results.

### Run the benchmark

```bash
cmake -S . -B build && cmake --build build -j
./build/cache_benchmark            # writes ./bench_results/*.csv
```

This produces four CSV files in `bench_results/`:

| File             | What it measures                                              |
| ---------------- | ------------------------------------------------------------ |
| `throughput.csv` | Ops/sec vs thread count, at 50% and 95% read mixes           |
| `latency.csv`    | `get` / `put` latency percentiles (p50–p99.9, max), in ns    |
| `memory.csv`     | Resident-set growth and bytes-per-entry vs entry count       |
| `hitrate.csv`    | Hit rate vs capacity under a Zipf-skewed (s=1.0) workload    |

Building the benchmark is controlled by `-DBUILD_BENCHMARKS=ON` (default on).

### Generate charts

```bash
pip install matplotlib
python3 scripts/visualize.py bench_results   # writes ./bench_results/*.png
```

This renders `throughput.png` (throughput + scalability vs ideal linear),
`latency.png`, `memory.png`, and `hitrate.png`.

### Memory & data-race analysis (sanitizers)

```bash
# ThreadSanitizer — detect data races in concurrent tests
cmake -S . -B build-tsan -DENABLE_TSAN=ON && cmake --build build-tsan -j
./build-tsan/test_lru_ttl_cache

# Address/UB Sanitizer — detect memory errors and leaks
cmake -S . -B build-asan -DENABLE_ASAN=ON && cmake --build build-asan -j
./build-asan/test_lru_ttl_cache
```

For a heap-usage-over-time profile, run the benchmark under Valgrind Massif:

```bash
valgrind --tool=massif ./build/cache_benchmark && ms_print massif.out.*
```

### Indicative results

Measured with `<int64_t, int64_t>` on a multi-core Linux machine (your numbers
will vary):

- **Latency**: `get` p50 ≈ 120 ns, p99 ≈ 335 ns; `put` p50 ≈ 148 ns
- **Memory**: ≈ 88 bytes per entry (node + map bucket overhead)
- **Throughput**: peaks single-threaded (~9–12M ops/s); the shared mutex means
  added threads contend rather than scale — see *Limitations* below

## Design Details

### Thread Safety

All public methods are protected by a mutex, ensuring thread-safe access. The cache can be safely used from multiple threads concurrently.

### Expiration Strategy

1. **On Access**: When `get()` is called, the entry is checked for expiration and removed if expired
2. **On Insert**: When `put()` is called, expired entries are evicted before inserting new ones
3. **Background Worker**: A dedicated thread periodically sweeps expired entries based on `sweep_interval`

### Eviction Policy

When the cache reaches capacity:

1. Expired entries are removed first (from oldest to newest)
2. If still at capacity, the least recently used (LRU) entry is evicted

### Performance Considerations

- **O(1) Operations**: Each entry is stored once in a node-based `std::unordered_map` and threaded through two intrusive doubly-linked lists (one for LRU order, one for expiry order). `put`/`get`/`erase` touch only a handful of pointers — O(1) amortized.
- **O(k) Purge**: Because the TTL is constant and `get()` does not refresh it, the expiry list stays sorted; purging only walks the soonest-to-expire end and stops at the first live entry.
- **Lock Granularity**: A single mutex protects all operations, which ensures correctness but may limit throughput under high contention
- **Background Sweep**: The optional background worker keeps the cache clean; disable it with `sweep_interval = Duration::zero()` if you prefer purely lazy reclamation
- **Move Semantics**: Use `put(key, std::move(value))` for better performance with large objects

## Example Use Cases

- **Session Management**: Store user sessions with automatic expiration
- **API Response Caching**: Cache API responses with TTL
- **Rate Limiting**: Track request counts with automatic cleanup
- **Configuration Caching**: Cache configuration values that need periodic refresh

## Thread Safety Guarantees

- Multiple threads can call `put()`, `get()`, `erase()`, `clear()`, `size()`, and `empty()` concurrently
- All operations are atomic with respect to each other
- The background worker thread safely cleans up expired entries

## Limitations

- **Single Mutex**: All operations share a single mutex, which may limit throughput under extreme contention. For very high concurrency, shard several caches by key hash.
- **No TTL Refresh**: Calling `get()` does not extend the TTL of an entry (this is by design and keeps expiry purging O(k))
- **Non-Copyable / Non-Movable**: The cache owns a background thread, so it is neither copyable nor movable. Wrap it in `std::unique_ptr` or `std::shared_ptr` if you need to relocate ownership.

## License

This project is provided as-is for educational and practical use.

## Contributing

Contributions are welcome! Please feel free to submit issues or pull requests.

## Author

Created as a thread-safe, production-ready LRU cache with TTL support.
