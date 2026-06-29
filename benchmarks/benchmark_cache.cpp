// Performance / multithreading / memory benchmark for LruTtlCacheThreadSafe.
//
// Produces four CSV files in the output directory (default: "bench_results"):
//   throughput.csv  -> ops/sec vs thread count, per read/write mix
//   latency.csv     -> per-operation latency percentiles (nanoseconds)
//   memory.csv      -> resident-set growth vs entry count (bytes/entry)
//   hitrate.csv     -> cache hit ratio vs capacity (Zipf-skewed workload)
//
// Visualize with: python3 scripts/visualize.py <output_dir>
//
// Build (from repo root):
//   cmake -S . -B build && cmake --build build -j
//   ./build/cache_benchmark            # writes ./bench_results/*.csv

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <lru_ttl_cache_thread_safe.hpp>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>

#if defined(__GLIBC__)
#include <malloc.h>
#endif
#include <vector>

namespace fs = std::filesystem;

namespace
{

using BenchClock = std::chrono::steady_clock;
using Cache      = LruTtlCacheThreadSafe<std::int64_t, std::int64_t>;

// Long TTL + disabled background sweep keeps perf measurements deterministic.
constexpr auto kLongTtl = std::chrono::hours(1);
Cache::Duration noSweep()
{
    return Cache::Duration::zero();
}

// Current resident set size of this process, in bytes (Linux /proc).
std::size_t getRssBytes()
{
    std::ifstream statm("/proc/self/statm");
    long total_pages    = 0;
    long resident_pages = 0;
    if (statm >> total_pages >> resident_pages)
    {
        long const page_size = ::sysconf(_SC_PAGESIZE);
        return static_cast<std::size_t>(resident_pages) * static_cast<std::size_t>(page_size);
    }
    return 0;
}

// Return freed heap pages to the OS so the next RSS baseline is accurate.
void releaseFreedMemory()
{
#if defined(__GLIBC__)
    ::malloc_trim(0);
#endif
}

std::uint64_t percentile(std::vector<std::uint64_t> const &sorted, double p)
{
    if (sorted.empty())
    {
        return 0;
    }
    std::size_t const idx = static_cast<std::size_t>(p * static_cast<double>(sorted.size() - 1));
    return sorted[idx];
}

// ---------------------------------------------------------------------------
// 1) Throughput scaling: many threads doing a mixed get/put workload.
// ---------------------------------------------------------------------------
void runThroughput(fs::path const &out)
{
    constexpr std::size_t capacity        = 100'000;
    std::int64_t const keyspace           = static_cast<std::int64_t>(capacity) * 2;
    constexpr long ops_per_thread         = 1'000'000;
    std::vector<double> const read_ratios = {0.50, 0.95};

    std::vector<int> thread_counts;
    unsigned const hw = std::max(1u, std::thread::hardware_concurrency());
    for (int t = 1; t <= static_cast<int>(hw) * 2; t *= 2)
    {
        thread_counts.push_back(t);
    }

    std::ofstream csv(out);
    csv << "threads,read_ratio,ops_per_sec\n";

    for (double const read_ratio : read_ratios)
    {
        for (int const num_threads : thread_counts)
        {
            Cache cache(capacity, kLongTtl, noSweep());
            for (std::int64_t i = 0; i < static_cast<std::int64_t>(capacity); ++i)
            {
                cache.put(i, i);
            }

            std::atomic<bool> start{false};
            std::atomic<int> ready{0};
            std::vector<std::thread> threads;
            threads.reserve(num_threads);

            auto worker = [&](int seed) {
                std::mt19937_64 rng(static_cast<std::uint64_t>(seed));
                std::uniform_int_distribution<std::int64_t> key_dist(0, keyspace - 1);
                std::uniform_real_distribution<double> op_dist(0.0, 1.0);
                ready.fetch_add(1, std::memory_order_release);
                while (!start.load(std::memory_order_acquire))
                {
                }
                for (long i = 0; i < ops_per_thread; ++i)
                {
                    std::int64_t const key = key_dist(rng);
                    if (op_dist(rng) < read_ratio)
                    {
                        auto v = cache.get(key);
                        (void)v;
                    }
                    else
                    {
                        cache.put(key, key);
                    }
                }
            };

            for (int t = 0; t < num_threads; ++t)
            {
                threads.emplace_back(worker, t + 1);
            }
            while (ready.load(std::memory_order_acquire) < num_threads)
            {
            }

            auto const t0 = BenchClock::now();
            start.store(true, std::memory_order_release);
            for (auto &th : threads)
            {
                th.join();
            }
            auto const t1 = BenchClock::now();

            double const secs        = std::chrono::duration<double>(t1 - t0).count();
            double const total_ops   = static_cast<double>(ops_per_thread) * num_threads;
            double const ops_per_sec = total_ops / secs;
            csv << num_threads << ',' << read_ratio << ',' << ops_per_sec << '\n';
            std::printf("throughput threads=%2d read=%.2f -> %10.0f ops/s\n",
                        num_threads,
                        read_ratio,
                        ops_per_sec);
        }
    }
}

// ---------------------------------------------------------------------------
// 2) Latency percentiles for single-threaded get / put.
// ---------------------------------------------------------------------------
void runLatency(fs::path const &out)
{
    constexpr std::size_t capacity = 100'000;
    constexpr long samples         = 500'000;

    Cache cache(capacity, kLongTtl, noSweep());
    for (std::int64_t i = 0; i < static_cast<std::int64_t>(capacity); ++i)
    {
        cache.put(i, i);
    }

    std::mt19937_64 rng(42);
    std::uniform_int_distribution<std::int64_t> key_dist(
        0, static_cast<std::int64_t>(capacity) * 2 - 1);

    std::vector<std::uint64_t> get_ns;
    std::vector<std::uint64_t> put_ns;
    get_ns.reserve(samples);
    put_ns.reserve(samples);

    for (long i = 0; i < samples; ++i)
    {
        std::int64_t const key = key_dist(rng);
        auto const a           = BenchClock::now();
        auto v                 = cache.get(key);
        auto const b           = BenchClock::now();
        (void)v;
        get_ns.push_back(static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count()));
    }
    for (long i = 0; i < samples; ++i)
    {
        std::int64_t const key = key_dist(rng);
        auto const a           = BenchClock::now();
        cache.put(key, key);
        auto const b = BenchClock::now();
        put_ns.push_back(static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count()));
    }

    std::sort(get_ns.begin(), get_ns.end());
    std::sort(put_ns.begin(), put_ns.end());

    std::vector<std::pair<std::string, double>> const points = {
        {"p50", 0.50}, {"p90", 0.90}, {"p95", 0.95}, {"p99", 0.99}, {"p999", 0.999}, {"max", 1.0}};

    std::ofstream csv(out);
    csv << "operation,percentile,nanoseconds\n";
    for (auto const &pt : points)
    {
        csv << "get," << pt.first << ',' << percentile(get_ns, pt.second) << '\n';
    }
    for (auto const &pt : points)
    {
        csv << "put," << pt.first << ',' << percentile(put_ns, pt.second) << '\n';
    }
    std::printf("latency get p50=%lu p99=%lu ns | put p50=%lu p99=%lu ns\n",
                static_cast<unsigned long>(percentile(get_ns, 0.50)),
                static_cast<unsigned long>(percentile(get_ns, 0.99)),
                static_cast<unsigned long>(percentile(put_ns, 0.50)),
                static_cast<unsigned long>(percentile(put_ns, 0.99)));
}

// ---------------------------------------------------------------------------
// 3) Memory footprint: resident-set growth per stored entry.
// ---------------------------------------------------------------------------
void runMemory(fs::path const &out)
{
    std::vector<std::size_t> const sizes = {10'000, 50'000, 100'000, 250'000, 500'000};

    std::ofstream csv(out);
    csv << "entries,rss_bytes,bytes_per_entry\n";

    for (std::size_t const n : sizes)
    {
        releaseFreedMemory();
        std::size_t const before = getRssBytes();
        auto cache               = std::make_unique<Cache>(n, kLongTtl, noSweep());
        for (std::int64_t i = 0; i < static_cast<std::int64_t>(n); ++i)
        {
            cache->put(i, i);
        }
        std::size_t const after      = getRssBytes();
        std::size_t const delta      = after > before ? after - before : 0;
        double const bytes_per_entry = static_cast<double>(delta) / static_cast<double>(n);
        csv << n << ',' << delta << ',' << bytes_per_entry << '\n';
        std::printf(
            "memory entries=%6zu rss_delta=%9zu bytes/entry=%.1f\n", n, delta, bytes_per_entry);
    }
}

// Precomputed-CDF Zipf sampler over [0, n).
class Zipf
{
public:
    Zipf(std::size_t n, double skew) : cdf_(n)
    {
        double sum = 0.0;
        for (std::size_t i = 0; i < n; ++i)
        {
            sum += 1.0 / std::pow(static_cast<double>(i + 1), skew);
            cdf_[i] = sum;
        }
        for (double &c : cdf_)
        {
            c /= sum;
        }
    }

    std::size_t sample(std::mt19937_64 &rng) const
    {
        std::uniform_real_distribution<double> dist(0.0, 1.0);
        double const u = dist(rng);
        auto const it  = std::lower_bound(cdf_.begin(), cdf_.end(), u);
        return static_cast<std::size_t>(it - cdf_.begin());
    }

private:
    std::vector<double> cdf_;
};

// ---------------------------------------------------------------------------
// 4) Hit rate vs capacity under a skewed (Zipf) access pattern.
// ---------------------------------------------------------------------------
void runHitRate(fs::path const &out)
{
    constexpr std::size_t keyspace = 100'000;
    constexpr double skew          = 1.0;
    constexpr long ops             = 1'000'000;
    Zipf zipf(keyspace, skew);

    std::vector<double> const fractions = {0.01, 0.05, 0.10, 0.25, 0.50};

    std::ofstream csv(out);
    csv << "capacity_fraction,capacity,hit_rate\n";

    for (double const frac : fractions)
    {
        std::size_t const cap = std::max<std::size_t>(
            1, static_cast<std::size_t>(static_cast<double>(keyspace) * frac));
        Cache cache(cap, kLongTtl, noSweep());
        std::mt19937_64 rng(7);

        auto access = [&](long count) {
            for (long i = 0; i < count; ++i)
            {
                auto const key = static_cast<std::int64_t>(zipf.sample(rng));
                if (!cache.get(key))
                {
                    cache.put(key, key);
                }
            }
        };

        access(ops / 2);  // warm up
        cache.resetStats();
        access(ops);  // measured window

        auto const st         = cache.stats();
        double const total    = static_cast<double>(st.hits + st.misses);
        double const hit_rate = total > 0.0 ? static_cast<double>(st.hits) / total : 0.0;
        csv << frac << ',' << cap << ',' << hit_rate << '\n';
        std::printf("hitrate cap=%.0f%% (%6zu) -> %.3f\n", frac * 100.0, cap, hit_rate);
    }
}

}  // namespace

int main(int argc, char **argv)
{
    fs::path const out_dir = argc > 1 ? fs::path(argv[1]) : fs::path("bench_results");
    fs::create_directories(out_dir);
    std::printf("Writing benchmark CSVs to: %s\n", out_dir.c_str());

    runThroughput(out_dir / "throughput.csv");
    runLatency(out_dir / "latency.csv");
    runMemory(out_dir / "memory.csv");
    runHitRate(out_dir / "hitrate.csv");

    std::printf("\nDone. Visualize with:\n  python3 scripts/visualize.py %s\n", out_dir.c_str());
    return 0;
}
