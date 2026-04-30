// benchmark_timing_control.cpp
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

#define FLUSH_THRESHOLD 5000
#define REPORT_INTERVAL 1
#define COOLDOWN_TIME 5
#define LOCAL_BUF_SLOT_NUM 1
#define ACCESS_SIZE 1024  // access size (max 4096)

using std::string;
using std::to_string;
using std::unique_ptr;
using std::unordered_set;
using std::vector;

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

// WaitForSubprocesses retained (unchanged)
static void WaitForSubprocesses(unordered_set<pid_t>& pids) {
    while (!pids.empty()) {
        int stat = 0;
        pid_t pid = wait(&stat);
        pids.erase(pid);
    }
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

// Worker workload function.
// Each worker keeps local latency vectors to avoid global synchronization.
// The worker reads the TimingControl flags to decide what to record/do.
// local_ctr: pointer to the thread's PaddedCounter (unique cacheline)
static void workload(AccessPattern* ap, const std::string& latency_filename, const std::string& write_latency_filename,
                     const std::string& tput_filename, std::atomic<uint64_t>* read_retry_cnt,
                     std::atomic<uint64_t>* read_cnt, std::atomic<uint64_t>* read_retry_invoc, TimingControl* control,
                     PaddedCounter* local_ctr, size_t thread_idx) {
    // local buffers/samples
    std::vector<int64_t> samples_read;
    std::vector<int64_t> samples_write;
    samples_read.reserve(5000000);
    samples_write.reserve(5000000);

    char* buf = (char*)malloc((size_t)ACCESS_SIZE * (size_t)LOCAL_BUF_SLOT_NUM);
    if (!buf) {
        LOG(FATAL) << "Failed to allocate local buffer";
    }

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> distrib(0, LOCAL_BUF_SLOT_NUM - 1);

    uint64_t local_ops = 0;
    bool warmup_done_local = false;
    bool measure_local = false;
    bool cooldown_local = false;

    // main loop: continue until controller sets stop = true
    while (true) {
        if (!warmup_done_local) {
            if (control->warmup_done.load(std::memory_order_acquire)) warmup_done_local = true;
        }
        if (!measure_local) {
            if (control->measure.load(std::memory_order_acquire)) measure_local = true;
        } else if (!cooldown_local) {
            if (control->cooldown.load(std::memory_order_acquire)) cooldown_local = true;
        } else if (control->stop.load(std::memory_order_acquire))
            break;

        // Generate access pattern
        off_t offset = ap->GenerateNextOffset();
        bool is_read = (ap->GenerateRW() == 0);
        size_t buf_offset = ACCESS_SIZE * distrib(gen);

        uint32_t tsc_aux;
        uint64_t t1 = __rdtscp(&tsc_aux);

        int rc;
        if (is_read) {
            rc = rackobj::Get(reinterpret_cast<uint8_t*>(buf) + buf_offset, ACCESS_SIZE, offset);
        } else {
            rc = rackobj::Put(reinterpret_cast<uint8_t*>(buf) + buf_offset, ACCESS_SIZE, offset);
        }

        uint64_t t2 = __rdtscp(&tsc_aux);
        PCHECK(rc != -1);

        local_ops++;
        if (local_ops >= FLUSH_THRESHOLD) {
            local_ctr->ops.fetch_add(local_ops, std::memory_order_relaxed);
            local_ops = 0;
        }

        // WARMUP: do not record latencies
        if (!warmup_done_local) continue;

        // MEASURE: record latencies only if measuring and not in cooldown
        if (measure_local && !cooldown_local) {
            int64_t ns = cycles_to_nanoseconds(t2 - t1, CPU_FREQ_GHZ);
            if (is_read)
                samples_read.push_back(ns);
            else
                samples_write.push_back(ns);
        }

        // COOLDOWN: drain ops w/o recording
        if (cooldown_local) continue;
    }

    // Write latency samples to the per-thread files
    write_samples(samples_read, latency_filename);
    write_samples(samples_write, write_latency_filename);

    // compute measurement duration using controller timestamps
    int64_t ms_start = control->measure_start_ns.load(std::memory_order_acquire);
    int64_t ms_end = control->measure_end_ns.load(std::memory_order_acquire);
    double duration_s = 0.0;
    if (ms_start != -1 && ms_end != -1 && ms_end > ms_start) {
        duration_s = static_cast<double>(ms_end - ms_start) / 1e9;
    } else {
        duration_s = 0.0;
    }

    // compute local counts by summing sizes (reads+writes) for this worker
    uint64_t local_count = samples_read.size() + samples_write.size();

    std::ofstream f(tput_filename);
    if (!f.is_open()) {
        LOG(ERROR) << "Failed to open throughput file: " << tput_filename;
    } else {
        f << local_count << "\n" << duration_s << "\n";
    }

#ifndef NONRACKOBJ
    read_cnt->fetch_add(samples_read.size(), std::memory_order_seq_cst);
    read_retry_cnt->fetch_add(rackobj::GetReadRetryCount());
    read_retry_invoc->fetch_add(rackobj::GetReadRetryInvoc());
#endif

    free(buf);
}

int main(int argc, char** argv) {
    absl::SetStderrThreshold(absl::LogSeverity::kInfo);
    absl::InitializeLog();

    if (argc != 10) {
        std::cerr << "Usage: " << argv[0]
                  << " <access pattern> <result dir> <key space> <thread cnt> <write_ratio> <zipfian_theta> "
                     "<partition_percentage> <partition_access_prob> <preheat time> <exec time>"
                  << std::endl;
        std::cerr << "       <shared ratio>: optional: default 1.0, for HotRandomPartialSharedAccessPattern"
                  << std::endl;
        std::cerr << "arg count: " << argc << std::endl;
        exit(1);
    }

    numa_set_strict(1);  // setting strict memory policy

    string pattern(argv[1]);
    string result_dir(argv[2]);
    size_t thread_cnt = std::stoull(argv[3]);
    float write_ratio = std::stof(argv[4]);
    auto write_ratio_str = argv[4];
    double zipfian_theta = std::stod(argv[5]);
    double partition_percentage = rackobj::GetPartitionRatio();
    double partition_access_prob = std::stod(argv[7]);
    preheat_time = std::stoi(argv[8]);
    exec_time = std::stoi(argv[9]);

    LOG(INFO) << "pattern: " << pattern;
    LOG(INFO) << "result_dir: " << result_dir;
    LOG(INFO) << "thread_cnt: " << thread_cnt;
    LOG(INFO) << "write_ratio: " << write_ratio;
    LOG(INFO) << "zipfian_theta: " << zipfian_theta;
    LOG(INFO) << "partition_percentage: " << partition_percentage;
    LOG(INFO) << "partition_access_prob: " << partition_access_prob;
    LOG(INFO) << "preheat_time: " << preheat_time;
    LOG(INFO) << "exec_time: " << exec_time;

    size_t key_space = rackobj::GetConfigSize_t("key_space");
    num_exec_numa = rackobj::GetConfigSize_t("num_exec_numa");

    string dir = result_dir + pattern + "-" + write_ratio_str;
    if (pattern == "hotcachepartial") dir += "/" + std::to_string(zipfian_theta);
    if (pattern == "zipfian" || pattern == "partial-partitioned") {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(6) << zipfian_theta;
        std::string sr_str = oss.str();
        sr_str.erase(sr_str.find_last_not_of('0') + 1, std::string::npos);
        if (!sr_str.empty() && sr_str.back() == '.') sr_str.pop_back();
        dir += "/" + sr_str;  // shared ratio is theta for zipf
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

    // Initialize access pattern and run the original warmup pre-loading.
    unique_ptr<AccessPattern> ap_shared =
        InitializeAccessPattern(pattern, key_space, rackobj::GetConfigSize_t("logical_nid"), write_ratio,
                                partition_percentage, partition_access_prob, zipfian_theta);
    if (ap_shared == nullptr) {
        std::cerr << "Unknown pattern: " << pattern << std::endl;
        exit(1);
    }

    static constexpr const size_t warmup_threads = 18;
    std::vector<std::jthread> warm_threads(warmup_threads);
    ThreadBarrier warmup_barrier(warmup_threads);

    std::vector<off_t> warmup_sequence = ap_shared->WarmupSequence();
    const size_t operation_count = warmup_sequence.size();
    LOG(INFO) << "warming up with " << operation_count << " ops";

    for (size_t thread_idx = 0; thread_idx < warm_threads.size(); ++thread_idx) {
        warm_threads[thread_idx] =
            std::jthread([thread_idx, operation_count, warmup_sequence, &warmup_barrier, write_ratio] {
                rackobj::Register(thread_idx);
                uint32_t numa_node_local = UINT32_MAX;
                int rc_getcpu = getcpu(nullptr, &numa_node_local);
                CHECK(rc_getcpu != -1) << "getcpu() failed";
                CHECK(numa_node_local == (thread_idx % num_exec_numa) + 1)
                    << "not running on target node: " << numa_node_local;

                warmup_barrier.Wait();

                uint8_t* buf = nullptr;
                std::unique_ptr<uint8_t[]> buf_holder;
                if (write_ratio > 0.0) {
                    buf_holder = std::make_unique<uint8_t[]>(ACCESS_SIZE);
                    buf = buf_holder.get();
                }
                ssize_t rc;

                for (size_t i = thread_idx; i < operation_count; i += warmup_threads) {
                    rc = rackobj::Put(buf, ACCESS_SIZE, warmup_sequence[i]);
                    PCHECK(rc != -1) << "warmup put failed";
                }

                warmup_barrier.Wait();
                rackobj::UnRegister();
            });
    }

    for (auto& t : warm_threads) {
        if (t.joinable()) t.join();
    }

    LOG(INFO) << "done loading...";
    sleep(5);

    // ---------------------------------------------------------------------
    // Setup timing control + per-thread counters + reporter + controller
    // ---------------------------------------------------------------------
    TimingControl control;
    std::vector<PaddedCounter> thread_counters;
    thread_counters.resize(thread_cnt);

    std::atomic<uint64_t> read_cnt{0};
    std::atomic<uint64_t> read_retry_cnt{0};
    std::atomic<uint64_t> read_retry_invoc{0};

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
                // relaxed loads are fine here because we only need approximate totals,
                // and each counter is padded to its own cache line (no false sharing).
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

    // ---------------------------------------------------------------------
    // Spawn worker threads (the "real" threads that execute the workload)
    // ---------------------------------------------------------------------
    std::vector<std::jthread> workers(thread_cnt);
    ThreadBarrier barrier(thread_cnt);  // barrier used to start workers together

    for (size_t i = 0; i < thread_cnt; ++i) {
        workers[i] = std::jthread([&, i] {
            rackobj::Register(i);  // keeps your existing Register behavior (affinity etc.)

            uint32_t numa_node_local = UINT32_MAX;
            int rc_getcpu = getcpu(nullptr, &numa_node_local);
            CHECK(rc_getcpu != -1) << "getcpu() failed";
            CHECK(numa_node_local == (i % num_exec_numa) + 1) << "not running on target node: " << numa_node_local;

            unique_ptr<AccessPattern> ap_local =
                InitializeAccessPattern(pattern, key_space, rackobj::GetConfigSize_t("logical_nid"), write_ratio,
                                        partition_percentage, partition_access_prob, zipfian_theta);

            // wait for all workers (not for controller) to be ready
            barrier.Wait();

            string latency_filename = dir2 + "/latencies-" + to_string(i);
            string write_latency_filename = dir2 + "/write-latencies-" + to_string(i);
            string tput_filename = dir2 + "/throughput-" + to_string(i);

            // pass pointer to this thread's counter
            workload(ap_local.get(), latency_filename, write_latency_filename, tput_filename, &read_retry_cnt,
                     &read_cnt, &read_retry_invoc, &control, &thread_counters[i], i);

            barrier.Wait();  // optional: wait for all to finish
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

    // final global stats
    uint64_t ttl_read_retry_cnt = read_retry_cnt.load(std::memory_order_acquire);
    uint64_t ttl_read_cnt = read_cnt.load(std::memory_order_acquire);
    uint64_t ttl_read_retry_invoc = read_retry_invoc.load(std::memory_order_acquire);

    if (ttl_read_cnt > 0) {
        LOG(INFO) << "retry ratio: "
                  << static_cast<double>(ttl_read_retry_invoc) / static_cast<double>(ttl_read_cnt) * 100 << "% ("
                  << ttl_read_retry_invoc << "|" << ttl_read_cnt << ")";
        LOG(INFO) << "avg # of retry attempts in a read call that had reties: "
                  << static_cast<double>(ttl_read_retry_cnt) / static_cast<double>(ttl_read_retry_invoc) << " ("
                  << ttl_read_retry_cnt << "|" << ttl_read_retry_invoc << ")";
    } else {
        LOG(INFO) << "No reads recorded.";
    }

    return 0;
}