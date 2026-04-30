// benchmark_tigon_timing.cpp
#include <fcntl.h>
#include <numa.h>
#include <rackobj.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <x86intrin.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <random>
#include <set>
#include <thread>
#include <unordered_set>
#include <vector>

#include "absl/log/check.h"
#include "absl/log/globals.h"
#include "absl/log/initialize.h"
#include "absl/log/log.h"
#include "access_pattern.h"
#include "common.h"

using namespace rackobj::benchmark;

static uint32_t exec_time;
static uint32_t preheat_time;
static uint32_t num_exec_numa = 0;
static uint32_t num_logical_node = 0;

#define LOCAL_BUF_SLOT_NUM 1
#define ACCESS_SIZE 4096
#define FLUSH_THRESHOLD 5000
#define REPORT_INTERVAL 1
#define COOLDOWN_TIME 5

using std::string;
using std::to_string;
using std::unique_ptr;
using std::unordered_set;
using std::vector;

// ---------- Timing control object shared across threads ----------
struct TimingControl {
    // phase flags
    std::atomic<bool> warmup_done{false};
    std::atomic<bool> measure{false};
    std::atomic<bool> cooldown{false};
    std::atomic<bool> stop{false};

    // measure window timestamps in nanoseconds since epoch
    // store as -1 if not set
    std::atomic<int64_t> measure_start_ns{-1};
    std::atomic<int64_t> measure_end_ns{-1};
};

// helper to get current time in ns since epoch
static inline int64_t now_ns() {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(high_resolution_clock::now().time_since_epoch()).count();
}

struct alignas(128) PaddedCounter {
    std::atomic<uint64_t> ops;

    static constexpr size_t kCacheLine = 128;
    static constexpr size_t kPad =
        (kCacheLine > sizeof(std::atomic<uint64_t>)) ? (kCacheLine - sizeof(std::atomic<uint64_t>)) : 1;
    char padding[kPad];

    PaddedCounter() noexcept : ops(0) {}

    PaddedCounter(const PaddedCounter&) = delete;
    PaddedCounter& operator=(const PaddedCounter&) = delete;

    PaddedCounter(PaddedCounter&&) noexcept : ops(0) {}

    PaddedCounter& operator=(PaddedCounter&&) noexcept {
        ops.store(0, std::memory_order_relaxed);
        return *this;
    }
};

struct stats_t {
    std::atomic<uint64_t> read_cnt;
    std::atomic<uint64_t> owned_hit;
    std::atomic<uint64_t> owned_miss;
    std::atomic<uint64_t> owned_access;
    std::atomic<uint64_t> remote_miss;
    std::atomic<uint64_t> remote_hit;
    std::atomic<uint64_t> remote_access;
};

template <typename T>
static void write_samples(vector<T>& samples, const string& filename) {
    if (samples.empty()) return;
    std::ofstream f(filename);
    if (!f.is_open()) {
        LOG(FATAL) << "Failed to open file: " << filename;
    }
    for (const auto& i : samples) {
        f << std::to_string(i) << '\n';
    }
}

static void WaitForSubprocesses(unordered_set<pid_t>& pids) {
    while (!pids.empty()) {
        int stat = 0;
        pid_t pid = wait(&stat);
        pids.erase(pid);
    }
}

// Worker workload function adapted to use TimingControl, PaddedCounter etc.
static void workload(AccessPattern* ap, const std::string& latency_filename, const std::string& write_latency_filename,
                     const std::string& tput_filename, struct stats_t& stats, TimingControl* control,
                     PaddedCounter* local_ctr, size_t thread_idx) {
    // local buffers/samples
    std::vector<int64_t> r_o_h, r_o_m, r_r_h, r_r_m;
    std::vector<int64_t> w_o_h, w_o_m, w_r_h, w_r_m;
    r_o_h.reserve(500000);
    r_o_m.reserve(500000);
    r_r_h.reserve(500000);
    r_r_m.reserve(500000);
    w_o_h.reserve(500000);
    w_o_m.reserve(500000);
    w_r_h.reserve(500000);
    w_r_m.reserve(500000);

    char* buf = (char*)malloc((size_t)ACCESS_SIZE * (size_t)LOCAL_BUF_SLOT_NUM);
    if (!buf) LOG(FATAL) << "Failed to allocate local buffer";

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> distrib(0, LOCAL_BUF_SLOT_NUM - 1);

    uint64_t local_ops = 0;
    bool warmup_done_local = false;
    bool measure_local = false;
    bool cooldown_local = false;

    int fd = open("/KV_STORE", O_RDWR);
    CHECK(fd != -1) << "Failed to open file";

    while (true) {
        // update phase flags
        if (!warmup_done_local) {
            if (control->warmup_done.load(std::memory_order_acquire)) warmup_done_local = true;
        }
        if (!measure_local) {
            if (control->measure.load(std::memory_order_acquire)) measure_local = true;
        } else if (!cooldown_local) {
            if (control->cooldown.load(std::memory_order_acquire)) cooldown_local = true;
        } else if (control->stop.load(std::memory_order_acquire))
            break;

        buf[0] = 0;
        off_t offset = ap->GenerateNextOffset();
        bool is_read = (ap->GenerateRW() == 0);
        size_t buf_offset = ACCESS_SIZE * distrib(gen);

        uint32_t tsc_aux;
        uint64_t t1 = __rdtscp(&tsc_aux);

        int rc;
        if (is_read) {
            rc = pread(fd, reinterpret_cast<uint8_t*>(buf) + buf_offset, ACCESS_SIZE, offset);
        } else {
            rc = pwrite(fd, reinterpret_cast<uint8_t*>(buf) + buf_offset, ACCESS_SIZE, offset);
        }

        uint64_t t2 = __rdtscp(&tsc_aux);
        PCHECK(rc != -1);

        local_ops++;
        if (local_ops >= FLUSH_THRESHOLD) {
            local_ctr->ops.fetch_add(local_ops, std::memory_order_relaxed);
            local_ops = 0;
        }

        // WARMUP: don't record
        if (!warmup_done_local) continue;

        // MEASURE: record latencies only if measuring and not in cooldown
        if (measure_local && !cooldown_local) {
            int64_t ns = cycles_to_nanoseconds(t2 - t1, CPU_FREQ_GHZ);
            if (is_read) {
                switch (rc) {
                    case 0:
                        r_o_h.push_back(ns);
                        break;
                    case 1:
                        r_o_m.push_back(ns);
                        break;
                    case 2:
                        r_r_h.push_back(ns);
                        break;
                    case 3:
                        r_r_m.push_back(ns);
                        break;
                    default:
                        break;
                }
            } else {
                switch (rc) {
                    case 0:
                        w_o_h.push_back(ns);
                        break;
                    case 1:
                        w_o_m.push_back(ns);
                        break;
                    case 2:
                        w_r_h.push_back(ns);
                        break;
                    case 3:
                        w_r_m.push_back(ns);
                        break;
                    default:
                        break;
                }
            }
        }

        // COOLDOWN: drain ops without recording
        if (cooldown_local) continue;
    }

    // flush any remaining local_ops to global counter
    if (local_ops > 0) local_ctr->ops.fetch_add(local_ops, std::memory_order_relaxed);

    // write per-thread latency files
    write_samples(r_o_h, latency_filename + "-r_o_h");
    write_samples(r_o_m, latency_filename + "-r_o_m");
    write_samples(r_r_h, latency_filename + "-r_r_h");
    write_samples(r_r_m, latency_filename + "-r_r_m");
    write_samples(w_o_h, write_latency_filename + "-w_o_h");
    write_samples(w_o_m, write_latency_filename + "-w_o_m");
    write_samples(w_r_h, write_latency_filename + "-w_r_h");
    write_samples(w_r_m, write_latency_filename + "-w_r_m");

    // compute measurement duration using controller timestamps
    int64_t ms_start = control->measure_start_ns.load(std::memory_order_acquire);
    int64_t ms_end = control->measure_end_ns.load(std::memory_order_acquire);
    double duration_s = 0.0;
    if (ms_start != -1 && ms_end != -1 && ms_end > ms_start) {
        duration_s = static_cast<double>(ms_end - ms_start) / 1e9;
    } else {
        // fallback: zero or unknown
        duration_s = 0.0;
    }

    uint64_t local_count = r_o_h.size() + r_o_m.size() + r_r_h.size() + r_r_m.size() + w_o_h.size() + w_o_m.size() +
                           w_r_h.size() + w_r_m.size();

    std::ofstream f(tput_filename);
    if (!f.is_open()) {
        LOG(ERROR) << "Failed to open throughput file: " << tput_filename;
    } else {
        f << local_count << "\n" << duration_s << "\n";
    }

    // update aggregated stats
    stats.read_cnt.fetch_add(r_o_h.size(), std::memory_order_seq_cst);
    stats.read_cnt.fetch_add(r_o_m.size(), std::memory_order_seq_cst);
    stats.read_cnt.fetch_add(r_r_h.size(), std::memory_order_seq_cst);
    stats.read_cnt.fetch_add(r_r_m.size(), std::memory_order_seq_cst);

    stats.owned_access.fetch_add(rackobj::GetTigonLocalAccess());
    stats.owned_hit.fetch_add(rackobj::GetTigonLocalAccessHit());
    stats.owned_miss.fetch_add(rackobj::GetTigonLocalAccessMiss());
    stats.remote_access.fetch_add(rackobj::GetTigonRemoteAccess());
    stats.remote_hit.fetch_add(rackobj::GetTigonRemoteAccessHit());
    stats.remote_miss.fetch_add(rackobj::GetTigonRemoteAccessMiss());

    free(buf);
}

int main(int argc, char** argv) {
    absl::SetStderrThreshold(absl::LogSeverity::kInfo);
    absl::InitializeLog();

    if (argc != 7 && argc != 8) {
        std::cerr << "Usage: " << argv[0]
                  << " <access pattern> <result dir> <thread cnt> <write_ratio> <shared ratio> <preheat time> "
                     "<benchmark time>"
                  << std::endl;
        std::cerr << "       <shared ratio>: optional: default 1.0, for HotRandomPartialSharedAccessPattern"
                  << std::endl;
        std::cerr << "arg count: " << argc << std::endl;
        exit(1);
    }

    numa_set_strict(1);  // setting strict memory policy
    CHECK(rackobj::GetConfigSize_t("slot_size") == 4096)
        << "slot size is not 4096: " << rackobj::GetConfigSize_t("slot_size");

    string pattern(argv[1]);
    string result_dir(argv[2]);
    size_t key_space = rackobj::GetConfigSize_t("key_space");
    size_t thread_cnt = std::stoull(argv[3]);
    float write_ratio = std::stof(argv[4]);
    double shared_ratio = std::stod(argv[5]);
    preheat_time = std::stoi(argv[6]);
    exec_time = std::stoi(argv[7]);

    num_exec_numa = rackobj::GetConfigSize_t("num_exec_numa");
    num_logical_node = rackobj::GetConfigSize_t("num_logical_node");

    string dir = result_dir;
    if (pattern == "hotcachepartial") {
        dir += "tigon/" + pattern + "-" + argv[4] + std::to_string(shared_ratio);
    } else if (pattern == "zipfian") {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(6) << shared_ratio;
        std::string sr_str = oss.str();
        sr_str.erase(sr_str.find_last_not_of('0') + 1, std::string::npos);
        if (!sr_str.empty() && sr_str.back() == '.') sr_str.pop_back();
        dir += "tigon/" + pattern + "-" + sr_str + "-" + argv[4];
    } else {
        dir += "tigon/" + pattern + "-" + argv[4];
    }
    struct stat st = {0};
    if (stat(dir.c_str(), &st) == -1) {
        std::string path = dir;
        size_t pos = 0;
        while ((pos = path.find('/', pos + 1)) != std::string::npos) {
            std::string subdir = path.substr(0, pos);
            if (mkdir(subdir.c_str(), 0755) < 0 && errno != EEXIST) {
                LOG(FATAL) << "Failed to create directory: " << subdir << " " << strerror(errno);
                exit(1);
            }
        }
        if (mkdir(path.c_str(), 0755) < 0 && errno != EEXIST) {
            LOG(FATAL) << "Failed to create directory: " << path << " " << strerror(errno);
            exit(1);
        }
    }

    LOG(INFO) << "RESULT DIR " << dir;

    uint32_t numa_node = UINT32_MAX;
    int rc = getcpu(nullptr, &numa_node);
    CHECK(rc != -1) << "getcpu() failed";
    LOG(INFO) << "Running on numa node " << numa_node << " , write ratio: " << write_ratio;

    LOG(INFO) << "Running " << thread_cnt << " concurrent threads";
    string dir2 = dir + "/" + to_string(thread_cnt);
    if (stat(dir2.c_str(), &st) == -1) {
        if (mkdir(dir2.c_str(), 0755) < 0) {
            LOG(FATAL) << "Failed to create directory: " << dir2 << " " << strerror(errno);
            exit(1);
        }
    }

    // Preload / warmup (kept same as before)
    {
        unique_ptr<AccessPattern> ap =
            InitializeAccessPattern(pattern, key_space, numa_node, write_ratio, shared_ratio);
        if (ap == nullptr) {
            std::cerr << "Unknown pattern: " << pattern << std::endl;
            exit(1);
        }

        CHECK(ap->GetPageOffset() == 4096) << "PAGE_OFFSET in access_pattern.h should be 4096";

        static const size_t num_threads = 6 * num_logical_node;
        std::vector<std::jthread> threads(num_threads);
        ThreadBarrier warmup_barrier(num_threads);

        std::vector<off_t> warmup_sequence = ap->WarmupSequence();
        const size_t operation_count = warmup_sequence.size();
        LOG(INFO) << "loading system with " << operation_count << " keys, " << num_threads << " threads";

        for (size_t thread_idx = 0; thread_idx < threads.size(); ++thread_idx) {
            threads[thread_idx] = std::jthread([thread_idx, operation_count, warmup_sequence, &warmup_barrier] {
                rackobj::Register(thread_idx);

                uint32_t thread_numa_node = (thread_idx % num_exec_numa) + 1;
                uint32_t numa_node_local = UINT32_MAX;
                int rc_getcpu = getcpu(nullptr, &numa_node_local);
                CHECK(rc_getcpu != -1) << "getcpu() failed";
                CHECK(numa_node_local == thread_numa_node)
                    << "not running on target node (" << numa_node_local << " != " << thread_numa_node << ")";

                warmup_barrier.Wait();

                char buf[ACCESS_SIZE];
                buf[0] = 1;
                ssize_t rc;

                // Calculate how many threads are on the same NUMA node
                size_t threads_per_numa = num_threads / num_logical_node;
                size_t local_thread_idx = thread_idx / num_logical_node;

                int fd = open("/KV_STORE", O_RDWR);
                for (size_t i = local_thread_idx; i < operation_count; i += threads_per_numa) {
                    rc = pwrite(fd, reinterpret_cast<uint8_t*>(buf), ACCESS_SIZE, warmup_sequence[i]);
                    PCHECK(rc != -1);
                }

                warmup_barrier.Wait();
                rackobj::UnRegister();
            });
        }
    }

    LOG(INFO) << "done loading...";
    LOG(INFO) << "starting workload for (" << preheat_time << "+" << exec_time << ") seconds...";

    // Setup TimingControl and counters
    TimingControl control;
    std::vector<PaddedCounter> thread_counters;
    thread_counters.resize(thread_cnt);

    stats_t stats = {
        0, 0, 0, 0, 0, 0, 0,
    };

    // Controller thread: drives warmup -> measure -> cooldown -> stop
    std::jthread controller([&] {
        using namespace std::chrono;
        LOG(INFO) << "Controller: warmup for " << preheat_time << "s";
        std::this_thread::sleep_for(seconds(preheat_time));

        // Warmup done
        control.warmup_done.store(true, std::memory_order_release);
        LOG(INFO) << "Controller: starting measurement window for " << exec_time << "s";

        // start measurement
        control.measure_start_ns.store(now_ns(), std::memory_order_release);
        control.measure.store(true, std::memory_order_release);
        rackobj::ClearMovementCounter();

        // measure for exec_time
        std::this_thread::sleep_for(seconds(exec_time));

        // end measurement
        control.measure.store(false);
        control.measure_end_ns.store(now_ns(), std::memory_order_release);
        LOG(INFO) << "Controller: measurement ended, entering cooldown for " << COOLDOWN_TIME << "s";

        // start cooldown
        control.cooldown.store(true, std::memory_order_release);
        std::this_thread::sleep_for(seconds(COOLDOWN_TIME));

        // stop everything
        control.stop.store(true, std::memory_order_release);
        LOG(INFO) << "Controller: stop set";
    });

    // Reporter thread: periodically aggregate per-thread counters and print throughput
    std::jthread reporter([&] {
        using namespace std::chrono;
        uint64_t last_total = 0;
        std::vector<uint64_t> tputs;
        const uint64_t interval = static_cast<uint64_t>(REPORT_INTERVAL);
        while (!control.stop.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(seconds(interval));

            uint64_t total = 0;
            for (size_t i = 0; i < thread_counters.size(); ++i) {
                total += thread_counters[i].ops.load(std::memory_order_relaxed);
            }

            uint64_t delta = total - last_total;
            last_total = total;

            uint64_t tput = delta / interval;
            LOG(INFO) << "GLOBAL THROUGHPUT: " << tput << " ops/s";
            if (control.measure.load(std::memory_order_acquire) && !control.cooldown.load(std::memory_order_acquire)) {
                tputs.push_back(tput);
            }
        }

        // final snapshot
        uint64_t final_total = 0;
        for (size_t i = 0; i < thread_counters.size(); ++i) {
            final_total += thread_counters[i].ops.load(std::memory_order_relaxed);
        }
        LOG(INFO) << "GLOBAL TOTAL OPS: " << final_total;
        if (!tputs.empty()) {
            double sum = 0.0;
            for (auto v : tputs) sum += v;
            double avg = sum / tputs.size();
            double sq_sum = 0.0;
            for (auto v : tputs) sq_sum += (v - avg) * (v - avg);
            double stddev = std::sqrt(sq_sum / tputs.size());
            LOG(INFO) << "GLOBAL THROUGHPUT: avg=" << static_cast<uint64_t>(avg)
                      << " ops/s, stddev=" << static_cast<uint64_t>(stddev) << " ops/s, out of " << tputs.size()
                      << " data points";
        }
    });

    // Spawn worker threads
    std::vector<std::jthread> workers(thread_cnt);
    ThreadBarrier barrier(thread_cnt);

    for (size_t i = 0; i < thread_cnt; ++i) {
        workers[i] = std::jthread([&, i] {
            rackobj::Register(i);

            uint32_t numa_node_local = UINT32_MAX;
            int rc_getcpu = getcpu(nullptr, &numa_node_local);
            CHECK(rc_getcpu != -1) << "getcpu() failed";
            CHECK(numa_node_local == (i % num_exec_numa) + 1) << "not running on target node: " << numa_node_local;

            unique_ptr<AccessPattern> ap =
                InitializeAccessPattern(pattern, key_space, numa_node_local, write_ratio, shared_ratio);

            // wait for all workers (not for controller) to be ready
            barrier.Wait();

            string latency_filename = dir2 + "/latencies-" + to_string(i);
            string write_latency_filename = dir2 + "/write-latencies-" + to_string(i);
            string tput_filename = dir2 + "/throughput-" + to_string(i);

            workload(ap.get(), latency_filename, write_latency_filename, tput_filename, stats, &control,
                     &thread_counters[i], i);

            barrier.Wait();
            rackobj::UnRegister();
        });
    }

    // join workers
    for (auto& th : workers) {
        if (th.joinable()) th.join();
    }

    // join controller + reporter (they should exit after stop)
    if (controller.joinable()) controller.join();
    if (reporter.joinable()) reporter.join();

    // final aggregated stats
    uint64_t ttl_read_cnt = stats.read_cnt.load(std::memory_order_acquire);
    uint64_t ttl_owned_access = stats.owned_access.load(std::memory_order_acquire);
    uint64_t ttl_owned_hit = stats.owned_hit.load(std::memory_order_acquire);
    uint64_t ttl_owned_miss = stats.owned_miss.load(std::memory_order_acquire);
    uint64_t ttl_remote_access = stats.remote_access.load(std::memory_order_acquire);
    uint64_t ttl_remote_hit = stats.remote_hit.load(std::memory_order_acquire);
    uint64_t ttl_remote_miss = stats.remote_miss.load(std::memory_order_acquire);

    LOG(INFO) << "owned partition hits: " << ttl_owned_hit << "\towned partition misses: " << ttl_owned_miss
              << "\ttotal owned partition accesses: " << ttl_owned_access;
    LOG(INFO) << "remote access hits: " << ttl_remote_hit << "\tremote access miss: " << ttl_remote_miss
              << "\ttotal remote accesses: " << ttl_remote_access;
    if ((ttl_owned_access + ttl_remote_access) > 0) {
        LOG(INFO) << "percent owned accesses (total owned / (total owned + total remote)): "
                  << static_cast<double>(ttl_owned_access) / static_cast<double>(ttl_owned_access + ttl_remote_access);
        LOG(INFO) << "percent remote accesses (total remote / (total owned + total remote)): "
                  << static_cast<double>(ttl_remote_access) / static_cast<double>(ttl_owned_access + ttl_remote_access);
        LOG(INFO) << "percent accesses to shared keys (total remote + owned misses / (total owned + total remote)): "
                  << static_cast<double>(ttl_remote_access + ttl_owned_miss) /
                         static_cast<double>(ttl_owned_access + ttl_remote_access);
        LOG(INFO) << "percent accesses to non-shared keys (owned hits / (total owned + total remote)): "
                  << static_cast<double>(ttl_owned_hit) / static_cast<double>(ttl_owned_access + ttl_remote_access);
    }

    return 0;
}
