#include <fcntl.h>
#include <numa.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <x86intrin.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <numeric>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>
#ifndef NONRACKOBJ
#include <rackobj.h>
#endif

constexpr size_t BLOCK_SIZE = 4096;
constexpr int NUM_EXEC_NODES = 3;

struct Operation {
    off_t offset;
    char type;
};

std::mutex latency_mutex;
std::vector<double> all_latencies;
std::vector<double> all_reads;
std::vector<double> all_writes;

// Function that a thread will execute
void worker(const std::string& target_filename, const std::vector<Operation>& ops, int tid) {
    {
        struct bitmask* cpus = numa_allocate_cpumask();
        int node = (tid % NUM_EXEC_NODES) + 1;
        if (node < numa_max_node()) {
            // If we actually have that NUMA node
            int rc = numa_node_to_cpus(node, cpus);
            if (rc == 0) {
                cpu_set_t cpuset;
                CPU_ZERO(&cpuset);
                for (int j = 0; j < cpus->size; ++j) {
                    if (numa_bitmask_isbitset(cpus, j)) {
                        CPU_SET(j, &cpuset);
                    }
                }
                pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
            }
        }
        numa_free_cpumask(cpus);
    }
    char buffer[BLOCK_SIZE];
    memset(buffer, tid + '0', BLOCK_SIZE);  // Different data per thread

    int fd = open(target_filename.c_str(), O_RDWR | O_CREAT, 0666);
    if (fd < 0) {
        perror("open failed inside thread");
        return;
    }

    std::vector<double> local_latencies;
    std::vector<double> local_reads;
    std::vector<double> local_writes;
    local_latencies.reserve(ops.size());
    local_reads.reserve(ops.size());
    local_writes.reserve(ops.size());

    for (const auto& op : ops) {
        auto t1 = std::chrono::high_resolution_clock::now();
        if (op.type == 'R') {
            ssize_t bytesRead = pread(fd, buffer, BLOCK_SIZE, op.offset);
            if (bytesRead < 0) perror("pread");
        } else if (op.type == 'W') {
            ssize_t bytesWritten = pwrite(fd, buffer, BLOCK_SIZE, op.offset);
            if (bytesWritten < 0) perror("pwrite");
        }
        auto t2 = std::chrono::high_resolution_clock::now();
        double us = std::chrono::duration<double, std::micro>(t2 - t1).count();
        local_latencies.push_back(us);
        if (op.type == 'R') {
            local_reads.push_back(us);
        } else if (op.type == 'W') {
            local_writes.push_back(us);
        }
    }

    close(fd);

    // Merge local latencies to global
    std::lock_guard<std::mutex> lock(latency_mutex);
    all_latencies.insert(all_latencies.end(), local_latencies.begin(), local_latencies.end());
    all_reads.insert(all_reads.end(), local_reads.begin(), local_reads.end());
    all_writes.insert(all_writes.end(), local_writes.begin(), local_writes.end());
}

int main(int argc, char* argv[]) {
    if (argc < 4) {
        std::cerr << "Usage: " << argv[0] << " <input_file> <target_file> <num_threads>\n";
        return 1;
    }

    std::string input_filename = argv[1];
    std::string target_filename = argv[2];
    int num_threads = std::stoi(argv[3]);

    std::ifstream infile(input_filename);
    if (!infile.is_open()) {
        std::cerr << "Failed to open input file\n";
        return 1;
    }

    std::vector<Operation> operations;
    std::string line;
    while (std::getline(infile, line)) {
        std::istringstream iss(line);
        std::string key_str, op_str;
        if (std::getline(iss, key_str, ',') && std::getline(iss, op_str)) {
            off_t offset = std::stoll(key_str) * BLOCK_SIZE;
            char op_type = op_str[0];
            operations.push_back({offset, op_type});
        }
        if (operations.size() == 1000000) {
            break;  // Limit to 20,000 operations for testing
        }
    }

    // Partition operations round-robin style
    std::vector<std::vector<Operation>> thread_ops(num_threads);
    for (size_t i = 0; i < operations.size(); ++i) {
        thread_ops[i % num_threads].push_back(operations[i]);
    }

    std::vector<std::thread> threads;
    all_latencies.clear();
    all_reads.clear();
    all_writes.clear();

    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back(worker, target_filename, thread_ops[i], i);
    }
    for (auto& t : threads) {
        t.join();
    }

    threads.clear();

    all_latencies.clear();
    all_reads.clear();
    all_writes.clear();

    auto start_time = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back(worker, target_filename, thread_ops[i], i);
    }
    for (auto& t : threads) {
        t.join();
    }
    auto end_time = std::chrono::high_resolution_clock::now();

    double elapsed_sec = std::chrono::duration<double>(end_time - start_time).count();
    double sum_lat = 0.0;
    for (double lat : all_latencies) sum_lat += lat;
    double avg_lat = all_latencies.empty() ? 0.0 : sum_lat / all_latencies.size();
    double throughput = operations.size() / ((sum_lat / num_threads) * 1e-6);
    ;
    double read_lat =
        all_reads.empty() ? 0.0 : std::accumulate(all_reads.begin(), all_reads.end(), 0.0) / all_reads.size();
    double write_lat =
        all_writes.empty() ? 0.0 : std::accumulate(all_writes.begin(), all_writes.end(), 0.0) / all_writes.size();

    std::cout << std::fixed << std::setprecision(2);
    std::cout << "BLOCK_RW | Threads: " << num_threads << " | Total Ops: " << operations.size()
              << " | Throughput: " << throughput << " ops/sec"
              << " | Avg Latency: " << avg_lat << " us/op\n";
    std::cout << "Avg Read Latency: " << read_lat << " us\n";
    std::cout << "Avg Write Latency: " << write_lat << " us\n";
    // std::cout << "Total Reads: " << all_reads.size() << "\n";
    // std::cout << "Total Writes: " << all_writes.size() << "\n";
    return 0;
}
