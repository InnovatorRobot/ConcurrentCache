# Thread-Safe TTL LRU Cache

A high-performance, thread-safe LRU (Least Recently Used) cache implementation with TTL (Time-To-Live) expiration support in C++17.

## Features

- **Thread-Safe**: All operations are protected by mutexes, safe for concurrent access
- **TTL Expiration**: Automatic expiration of entries based on time-to-live
- **LRU Eviction**: Evicts least recently used entries when capacity is reached
- **Background Cleanup**: Dedicated worker thread periodically sweeps expired entries
- **Header-Only**: Easy to integrate, just include the header file
- **Modern C++**: Uses C++17 features (std::optional, std::chrono, etc.)
- **Move Semantics**: Supports efficient move operations for better performance

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

#### Constructor

```cpp
LruTtlCacheThreadSafe(std::size_t capacity,
                      Duration ttl,
                      Duration sweep_interval = std::chrono::seconds(1))
```

- `capacity`: Maximum number of entries in the cache
- `ttl`: Time-to-live duration for entries
- `sweep_interval`: Interval between background cleanup sweeps (default: 1 second)

#### Methods

- `void put(K const& key, V const& value)`: Insert or update a key-value pair
- `void put(K const& key, V&& value)`: Insert or update using move semantics
- `std::optional<V> get(K const& key)`: Retrieve a value by key (returns `std::nullopt` if not found or expired)
- `bool erase(K const& key)`: Remove a key-value pair (returns `true` if found and removed)
- `void clear()`: Clear all entries from the cache
- `std::size_t size() const`: Get the current number of entries
- `bool empty() const`: Check if the cache is empty
- `std::size_t capacity() const`: Get the maximum capacity
- `Duration ttl() const`: Get the TTL duration

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

- **Lock Granularity**: A single mutex protects all operations, which ensures correctness but may limit throughput under high contention
- **Background Sweep**: The background worker helps keep the cache clean without blocking user operations
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

- **Single Mutex**: All operations share a single mutex, which may limit throughput under extreme contention
- **No TTL Refresh**: Calling `get()` does not extend the TTL of an entry (this is by design)
- **No Copy Semantics**: The cache is not copyable (only movable) to avoid complexity

## License

This project is provided as-is for educational and practical use.

## Contributing

Contributions are welcome! Please feel free to submit issues or pull requests.

## Author

Created as a thread-safe, production-ready LRU cache with TTL support.

