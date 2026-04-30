// YCSB Benchmark for RackOBJ
// Supports YCSB workloads A, B, C, D, F
//
// Workload A: 50% read, 50% update (Zipfian)
// Workload B: 95% read, 5% update (Zipfian)
// Workload C: 100% read (Zipfian)
// Workload D: 95% read, 5% insert (Latest - biased toward recent inserts)
// Workload F: 50% read, 50% read-modify-write (Zipfian)

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
#include <cmath>
#include <csignal>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <random>
#include <thread>
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

#define FLUSH_THRESHOLD 5000
#define REPORT_INTERVAL 1
#define COOLDOWN_TIME 5
#define LOCAL_BUF_SLOT_NUM 1
#define ACCESS_SIZE 1024

using std::string;
using std::to_string;
using std::unique_ptr;
using std::vector;

// YCSB Workload types
enum class YCSBWorkload { A, B, C, D, F };

// Parse workload from string
YCSBWorkload ParseWorkload(const string& workload_str) {
    if (workload_str == "a" || workload_str == "A") return YCSBWorkload::A;
    if (workload_str == "b" || workload_str == "B") return YCSBWorkload::B;
    if (workload_str == "c" || workload_str == "C") return YCSBWorkload::C;
    if (workload_str == "d" || workload_str == "D") return YCSBWorkload::D;
    if (workload_str == "f" || workload_str == "F") return YCSBWorkload::F;
    LOG(FATAL) << "Unknown workload: " << workload_str << ". Valid options: a, b, c, d, f";
    return YCSBWorkload::A;  // unreachable
}

string WorkloadToString(YCSBWorkload workload) {
    switch (workload) {
        case YCSBWorkload::A:
            return "A";
        case YCSBWorkload::B:
            return "B";
        case YCSBWorkload::C:
            return "C";
        case YCSBWorkload::D:
            return "D";
        case YCSBWorkload::F:
            return "F";
    }
    return "Unknown";
}

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

// Timing control object shared across threads
struct TimingControl {
    std::atomic<bool> warmup_done{false};
    std::atomic<bool> measure{false};
    std::atomic<bool> cooldown{false};
    std::atomic<bool> stop{false};
    std::atomic<int64_t> measure_start_ns{-1};
    std::atomic<int64_t> measure_end_ns{-1};
};

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

// Worker workload function
static void workload(YCSBWorkload workload_type, AccessPattern* ap, const string& read_latency_file,
                     const string& write_latency_file, const string& insert_latency_file, const string& tput_file,
                     std::atomic<uint64_t>* read_retry_cnt, std::atomic<uint64_t>* read_cnt,
                     std::atomic<uint64_t>* read_retry_invoc, TimingControl* control, PaddedCounter* local_ctr,
                     size_t thread_idx) {
    // Local buffers for latency samples
    vector<int64_t> samples_read;
    vector<int64_t> samples_write;
    vector<int64_t> samples_insert;
    samples_read.reserve(5000000);
    samples_write.reserve(5000000);
    samples_insert.reserve(1000000);

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

    while (true) {
        if (workload_type == YCSBWorkload::D) {
            if (control->stop.load(std::memory_order_acquire)) break;
        }
        if (!warmup_done_local) {
            if (control->warmup_done.load(std::memory_order_acquire)) warmup_done_local = true;
        }
        if (!measure_local) {
            if (control->measure.load(std::memory_order_acquire)) measure_local = true;
        } else if (!cooldown_local) {
            if (control->cooldown.load(std::memory_order_acquire)) cooldown_local = true;
        } else if (control->stop.load(std::memory_order_acquire))
            break;

        // Generate operation and offset
        Operation op = ap->GenerateOperation();
        off_t offset = ap->GenerateNextOffset();
        size_t buf_offset = ACCESS_SIZE * distrib(gen);

        uint32_t tsc_aux;
        uint64_t t1, t2;
        int rc;

        // Variables to store latencies for recording after the operation
        int64_t read_latency_ns = 0;
        int64_t write_latency_ns = 0;
        bool did_read = false;
        bool did_write = false;
        bool did_insert = false;

        switch (op) {
            case Operation::READ:
                t1 = __rdtscp(&tsc_aux);
                rc = rackobj::Get(reinterpret_cast<uint8_t*>(buf) + buf_offset, ACCESS_SIZE, offset);
                t2 = __rdtscp(&tsc_aux);
                PCHECK(rc != -1);
                local_ops++;
                read_latency_ns = cycles_to_nanoseconds(t2 - t1, CPU_FREQ_GHZ);
                did_read = true;
                break;

            case Operation::UPDATE:
                t1 = __rdtscp(&tsc_aux);
                rc = rackobj::Put(reinterpret_cast<uint8_t*>(buf) + buf_offset, ACCESS_SIZE, offset);
                t2 = __rdtscp(&tsc_aux);
                PCHECK(rc != -1);
                local_ops++;
                write_latency_ns = cycles_to_nanoseconds(t2 - t1, CPU_FREQ_GHZ);
                did_write = true;
                break;

            case Operation::INSERT:
                t1 = __rdtscp(&tsc_aux);
                rc = rackobj::Put(reinterpret_cast<uint8_t*>(buf) + buf_offset, ACCESS_SIZE, offset);
                t2 = __rdtscp(&tsc_aux);
                PCHECK(rc != -1);
                local_ops++;
                write_latency_ns = cycles_to_nanoseconds(t2 - t1, CPU_FREQ_GHZ);
                did_insert = true;
                break;

            case Operation::READ_MODIFY_WRITE:
                // Read phase
                t1 = __rdtscp(&tsc_aux);
                rc = rackobj::Get(reinterpret_cast<uint8_t*>(buf) + buf_offset, ACCESS_SIZE, offset);
                t2 = __rdtscp(&tsc_aux);
                PCHECK(rc != -1);
                read_latency_ns = cycles_to_nanoseconds(t2 - t1, CPU_FREQ_GHZ);
                did_read = true;

                // Write phase (same key)
                t1 = __rdtscp(&tsc_aux);
                rc = rackobj::Put(reinterpret_cast<uint8_t*>(buf) + buf_offset, ACCESS_SIZE, offset);
                t2 = __rdtscp(&tsc_aux);
                PCHECK(rc != -1);
                write_latency_ns = cycles_to_nanoseconds(t2 - t1, CPU_FREQ_GHZ);
                did_write = true;

                local_ops += 2;  // Count as 2 operations
                break;
        }

        if (local_ops >= FLUSH_THRESHOLD) {
            local_ctr->ops.fetch_add(local_ops, std::memory_order_relaxed);
            local_ops = 0;
        }

        // WARMUP: do not record latencies
        if (!warmup_done_local) continue;

        // MEASURE: record latencies only if measuring and not in cooldown
        if (measure_local && !cooldown_local) {
            if (did_read) samples_read.push_back(read_latency_ns);
            if (did_write) samples_write.push_back(write_latency_ns);
            if (did_insert) samples_insert.push_back(write_latency_ns);
        }

        // COOLDOWN: drain ops w/o recording
        if (cooldown_local) continue;
    }

    // Write latency samples to files
    write_samples(samples_read, read_latency_file);
    write_samples(samples_write, write_latency_file);
    if (workload_type == YCSBWorkload::D) {
        write_samples(samples_insert, insert_latency_file);
    }

    // Compute measurement duration
    int64_t ms_start = control->measure_start_ns.load(std::memory_order_acquire);
    int64_t ms_end = control->measure_end_ns.load(std::memory_order_acquire);
    double duration_s = 0.0;
    if (ms_start != -1 && ms_end != -1 && ms_end > ms_start) {
        duration_s = static_cast<double>(ms_end - ms_start) / 1e9;
    }

    uint64_t local_count = samples_read.size() + samples_write.size() + samples_insert.size();

    std::ofstream f(tput_file);
    if (f.is_open()) {
        f << local_count << "\n" << duration_s << "\n";
    }

#ifndef NONRACKOBJ
    read_cnt->fetch_add(samples_read.size(), std::memory_order_seq_cst);
    read_retry_cnt->fetch_add(rackobj::GetReadRetryCount());
    read_retry_invoc->fetch_add(rackobj::GetReadRetryInvoc());
#endif

    free(buf);
}

void PrintUsage(const char* prog_name) {
    std::cerr << "Usage: " << prog_name
              << " <workload> <result_dir> <thread_cnt> <zipfian_theta> <preheat_time> <exec_time>\n"
              << "\n"
              << "Arguments:\n"
              << "  workload       : a, b, c, d, or f\n"
              << "  result_dir     : Directory to store results\n"
              << "  thread_cnt     : Number of worker threads\n"
              << "  zipfian_theta  : Zipfian skewness parameter (0.0-0.99, default 0.99)\n"
              << "  preheat_time   : Warmup duration in seconds\n"
              << "  exec_time      : Measurement duration in seconds\n"
              << "\n"
              << "Note: key_space is read from RACKOBJ_CONFIG file\n"
              << "\n"
              << "YCSB Workloads:\n"
              << "  A: 50% read, 50% update (Zipfian)\n"
              << "  B: 95% read, 5% update (Zipfian)\n"
              << "  C: 100% read (Zipfian)\n"
              << "  D: 95% read, 5% insert (Latest distribution)\n"
              << "  F: 50% read, 50% read-modify-write (Zipfian)\n"
              << std::endl;
}

int main(int argc, char** argv) {
    absl::SetStderrThreshold(absl::LogSeverity::kInfo);
    absl::InitializeLog();

    if (argc != 7) {
        PrintUsage(argv[0]);
        exit(1);
    }

    numa_set_strict(1);

    YCSBWorkload workload_type = ParseWorkload(argv[1]);
    string result_dir(argv[2]);
    size_t thread_cnt = std::stoull(argv[3]);
    double zipfian_theta = std::stod(argv[4]);
    preheat_time = std::stoi(argv[5]);
    exec_time = std::stoi(argv[6]);

    // Read key_space from config file
    size_t record_count = rackobj::GetConfigSize_t("key_space");
    num_exec_numa = rackobj::GetConfigSize_t("num_exec_numa");

    // Create result directory
    string dir = result_dir + "ycsb-" + WorkloadToString(workload_type);
    struct stat st = {0};
    if (stat(dir.c_str(), &st) == -1) {
        string path = dir;
        size_t pos = 0;
        while ((pos = path.find('/', pos + 1)) != string::npos) {
            string subdir = path.substr(0, pos);
            if (mkdir(subdir.c_str(), 0755) < 0 && errno != EEXIST) {
                LOG(FATAL) << "Failed to create directory: " << subdir << " " << strerror(errno);
            }
        }
        if (mkdir(path.c_str(), 0755) < 0 && errno != EEXIST) {
            LOG(FATAL) << "Failed to create directory: " << path << " " << strerror(errno);
        }
    }

    LOG(INFO) << "YCSB Workload " << WorkloadToString(workload_type);
    LOG(INFO) << "Thread count: " << thread_cnt;
    LOG(INFO) << "Record count: " << record_count;
    LOG(INFO) << "Zipfian theta: " << zipfian_theta;
    LOG(INFO) << "Preheat time: " << preheat_time << "s";
    LOG(INFO) << "Exec time: " << exec_time << "s";
    LOG(INFO) << "Result dir: " << dir;

    uint32_t numa_node = UINT32_MAX;
    int rc = getcpu(nullptr, &numa_node);
    CHECK(rc != -1) << "getcpu() failed";
    LOG(INFO) << "Running on numa node " << numa_node;

    string dir2 = dir + "/" + to_string(thread_cnt);
    if (stat(dir2.c_str(), &st) == -1) {
        if (mkdir(dir2.c_str(), 0755) < 0) {
            LOG(FATAL) << "Failed to create directory: " << dir2 << " " << strerror(errno);
        }
    }

    // Shared counter for workload D (dynamic inserts)
    unique_ptr<CounterGenerator> shared_counter;
    if (workload_type == YCSBWorkload::D) {
        shared_counter = std::make_unique<CounterGenerator>(record_count);
    }

    // Create warmup access pattern
    unique_ptr<AccessPattern> ap_warmup;
    switch (workload_type) {
        case YCSBWorkload::A:
            ap_warmup = std::make_unique<YCSBWorkloadABCPattern>(record_count, zipfian_theta, 0.5);
            break;
        case YCSBWorkload::B:
            ap_warmup = std::make_unique<YCSBWorkloadABCPattern>(record_count, zipfian_theta, 0.05);
            break;
        case YCSBWorkload::C:
            ap_warmup = std::make_unique<YCSBWorkloadABCPattern>(record_count, zipfian_theta, 0.0);
            break;
        case YCSBWorkload::D:
            ap_warmup = std::make_unique<YCSBWorkloadDPattern>(record_count, shared_counter.get(), 0.05);
            break;
        case YCSBWorkload::F:
            ap_warmup = std::make_unique<YCSBWorkloadFPattern>(record_count, zipfian_theta, 0.5);
            break;
    }

    // Warmup phase - load initial records
    static constexpr size_t warmup_threads = 18;
    vector<std::jthread> warm_threads(warmup_threads);
    ThreadBarrier warmup_barrier(warmup_threads);

    num_logical_node = rackobj::GetConfigSize_t("num_logical_node");
    vector<off_t> warmup_sequence = ap_warmup->WarmupSequence();
    const size_t operation_count = warmup_sequence.size();
    LOG(INFO) << "Warming up with " << operation_count << " records";

    for (size_t thread_idx = 0; thread_idx < warm_threads.size(); ++thread_idx) {
        warm_threads[thread_idx] = std::jthread([thread_idx, operation_count, &warmup_sequence, &warmup_barrier] {
            rackobj::Register(thread_idx);

            uint32_t numa_node_local = UINT32_MAX;
            int rc_getcpu = getcpu(nullptr, &numa_node_local);
            CHECK(rc_getcpu != -1) << "getcpu() failed";
            CHECK(numa_node_local == (thread_idx % num_exec_numa) + 1)
                << "not running on target node: " << numa_node_local;

            warmup_barrier.Wait();

            uint8_t buf[ACCESS_SIZE] = {0};
            ssize_t rc;

            // Calculate how many threads are on the same NUMA node
            size_t threads_per_numa = warmup_threads / num_logical_node;
            size_t local_thread_idx = thread_idx / num_logical_node;

            for (size_t i = local_thread_idx; i < operation_count; i += threads_per_numa) {
                rc = rackobj::Preload(buf, ACCESS_SIZE, warmup_sequence[i]);
                PCHECK(rc != -1) << "warmup put failed";
            }

            warmup_barrier.Wait();
            rackobj::UnRegister();
        });
    }

    for (auto& t : warm_threads) {
        if (t.joinable()) t.join();
    }

    LOG(INFO) << "Done loading initial records";

    // Setup timing control and counters
    TimingControl control;
    vector<PaddedCounter> thread_counters;
    thread_counters.resize(thread_cnt);

    std::atomic<uint64_t> read_cnt{0};
    std::atomic<uint64_t> read_retry_cnt{0};
    std::atomic<uint64_t> read_retry_invoc{0};

    // Controller thread
    std::jthread controller([&] {
        using namespace std::chrono;

        LOG(INFO) << "Controller: warmup for " << preheat_time << "s";
        std::this_thread::sleep_for(seconds(preheat_time));

        control.warmup_done.store(true, std::memory_order_release);
        LOG(INFO) << "Controller: starting measurement for " << exec_time << "s";

        control.measure_start_ns.store(now_ns(), std::memory_order_release);
        control.measure.store(true, std::memory_order_release);
        rackobj::ClearMovementCounter();

        std::this_thread::sleep_for(seconds(exec_time));

        control.measure.store(false);
        control.measure_end_ns.store(now_ns(), std::memory_order_release);
        LOG(INFO) << "Controller: measurement ended, cooldown for " << COOLDOWN_TIME << "s";

        control.cooldown.store(true, std::memory_order_release);
        std::this_thread::sleep_for(seconds(COOLDOWN_TIME));

        control.stop.store(true, std::memory_order_release);
        LOG(INFO) << "Controller: stop";
    });

    // Reporter thread
    std::jthread reporter([&] {
        using namespace std::chrono;
        uint64_t last_total = 0;
        vector<uint64_t> tputs;
        const uint64_t interval = REPORT_INTERVAL;

        while (!control.stop.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(seconds(interval));

            uint64_t total = 0;
            for (size_t i = 0; i < thread_counters.size(); ++i) {
                total += thread_counters[i].ops.load(std::memory_order_relaxed);
            }

            uint64_t delta = total - last_total;
            last_total = total;

            uint64_t tput = delta / interval;
            LOG(INFO) << "THROUGHPUT: " << tput << " ops/s";

            if (control.measure.load(std::memory_order_acquire) && !control.cooldown.load(std::memory_order_acquire)) {
                tputs.push_back(tput);
            }
        }

        uint64_t final_total = 0;
        for (size_t i = 0; i < thread_counters.size(); ++i) {
            final_total += thread_counters[i].ops.load(std::memory_order_relaxed);
        }
        LOG(INFO) << "TOTAL OPS: " << final_total;

        if (!tputs.empty()) {
            double sum = 0.0;
            for (auto v : tputs) sum += v;
            double avg = sum / tputs.size();
            double sq_sum = 0.0;
            for (auto v : tputs) sq_sum += (v - avg) * (v - avg);
            double stddev = std::sqrt(sq_sum / tputs.size());
            LOG(INFO) << "AVG THROUGHPUT: " << static_cast<uint64_t>(avg)
                      << " ops/s, stddev=" << static_cast<uint64_t>(stddev) << " (" << tputs.size() << " samples)";
        }
    });

    // Worker threads
    vector<std::jthread> workers(thread_cnt);
    ThreadBarrier barrier(thread_cnt);

    for (size_t i = 0; i < thread_cnt; ++i) {
        workers[i] = std::jthread([&, i] {
            rackobj::Register(i);

            uint32_t numa_node_local = UINT32_MAX;
            int rc_getcpu = getcpu(nullptr, &numa_node_local);
            CHECK(rc_getcpu != -1) << "getcpu() failed";
            CHECK(numa_node_local == (i % num_exec_numa) + 1) << "not running on target node: " << numa_node_local;

            // Create per-thread access pattern
            unique_ptr<AccessPattern> ap_local;
            switch (workload_type) {
                case YCSBWorkload::A:
                    ap_local = std::make_unique<YCSBWorkloadABCPattern>(record_count, zipfian_theta, 0.5);
                    break;
                case YCSBWorkload::B:
                    ap_local = std::make_unique<YCSBWorkloadABCPattern>(record_count, zipfian_theta, 0.05);
                    break;
                case YCSBWorkload::C:
                    ap_local = std::make_unique<YCSBWorkloadABCPattern>(record_count, zipfian_theta, 0.0);
                    break;
                case YCSBWorkload::D:
                    ap_local = std::make_unique<YCSBWorkloadDPattern>(record_count, shared_counter.get(), 0.05);
                    break;
                case YCSBWorkload::F:
                    ap_local = std::make_unique<YCSBWorkloadFPattern>(record_count, zipfian_theta, 0.5);
                    break;
            }

            barrier.Wait();

            string read_latency_file = dir2 + "/latencies-" + to_string(i);
            string write_latency_file = dir2 + "/write-latencies-" + to_string(i);
            string insert_latency_file = dir2 + "/insert-latencies-" + to_string(i);
            string tput_file = dir2 + "/throughput-" + to_string(i);

            workload(workload_type, ap_local.get(), read_latency_file, write_latency_file, insert_latency_file,
                     tput_file, &read_retry_cnt, &read_cnt, &read_retry_invoc, &control, &thread_counters[i], i);

            barrier.Wait();
            rackobj::UnRegister();
        });
    }

    // Join all threads
    for (auto& th : workers) {
        if (th.joinable()) th.join();
    }
    if (controller.joinable()) controller.join();
    if (reporter.joinable()) reporter.join();

    // Print final stats
    uint64_t ttl_read_retry_cnt = read_retry_cnt.load(std::memory_order_acquire);
    uint64_t ttl_read_cnt = read_cnt.load(std::memory_order_acquire);
    uint64_t ttl_read_retry_invoc = read_retry_invoc.load(std::memory_order_acquire);

    if (ttl_read_cnt > 0) {
        LOG(INFO) << "Retry ratio: "
                  << static_cast<double>(ttl_read_retry_invoc) / static_cast<double>(ttl_read_cnt) * 100 << "% ("
                  << ttl_read_retry_invoc << "|" << ttl_read_cnt << ")";
        if (ttl_read_retry_invoc > 0) {
            LOG(INFO) << "Avg retries per retry invoc: "
                      << static_cast<double>(ttl_read_retry_cnt) / static_cast<double>(ttl_read_retry_invoc);
        }
    }

    if (workload_type == YCSBWorkload::D && shared_counter) {
        LOG(INFO) << "Total inserts (keyspace growth): " << shared_counter->Current() - record_count;
    }

    return 0;
}
