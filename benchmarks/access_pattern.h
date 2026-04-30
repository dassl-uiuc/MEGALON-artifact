#pragma once

#include <numa.h>
#include <sys/types.h>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <memory>
#include <random>
#include <sstream>
#include <vector>

// #include "absl/log/log.h"
#include "zipf_implementation.h"

namespace rackobj::benchmark {

#ifdef FILE_INTERFACE
static constexpr off_t PAGE_OFFSET = 4096;
#else
static constexpr off_t PAGE_OFFSET = 1;
#endif

// YCSB operation types
enum class Operation : uint8_t { READ = 0, UPDATE = 1, INSERT = 2, READ_MODIFY_WRITE = 3 };

class AccessPattern {
public:
    explicit AccessPattern(size_t page_count) : page_count_(static_cast<off_t>(page_count)) {}

    virtual ~AccessPattern() {}

    virtual off_t GenerateNextOffset() = 0;

    /** 0: read; else: write */
    virtual uint8_t GenerateRW() { return 0; }

    virtual Operation GenerateOperation() { return Operation::READ; }

    off_t GetPageCount() const { return page_count_; }

    virtual std::vector<off_t> WarmupSequence() { return {}; }

    off_t page_count_;
};

class SequentialAccessPattern : public AccessPattern {
public:
    SequentialAccessPattern(size_t page_count) : AccessPattern(page_count), index_(0) {}

    off_t GenerateNextOffset() { return ((index_++) % GetPageCount()) * PAGE_OFFSET; }

private:
    off_t index_;
};

class RandomAccessPattern : public AccessPattern {
public:
    RandomAccessPattern(size_t page_count, float write_ratio = 0.0)
        : AccessPattern(page_count), rd_(), generator_(rd_()), distribution_(0, GetPageCount() - 1) {
        float threshold_f = write_ratio * 100.0;
        threshold_ = (long)threshold_f;
        // DLOG(INFO) << "pattern init with page_count: " << page_count << ", threshold: " << threshold_;
    }

    off_t GenerateNextOffset() { return distribution_(generator_) * PAGE_OFFSET; }

    uint8_t GenerateRW() {
        long var;
        do {
            var = distribution_(generator_) % 100;
        } while (var == threshold_);

        return var > threshold_ ? 0 : 1;
    }

protected:
    std::random_device rd_;
    std::mt19937_64 generator_;
    std::uniform_int_distribution<off_t> distribution_;
    long threshold_;
};

class RangeRandomAccessPattern : public AccessPattern {
public:
    RangeRandomAccessPattern(size_t min, size_t max, float write_ratio = 0.0)
        : AccessPattern(max - min + 1), rd_(), generator_(rd_()), distribution_(min, max) {
        float threshold_f = write_ratio * 100.0;
        threshold_ = (long)threshold_f;
    }

    off_t GenerateNextOffset() { return distribution_(generator_); }
    uint8_t GenerateRW() { return utils::ThreadLocalRandomDouble() < threshold_ ? 1 : 0; }

protected:
    std::random_device rd_;
    std::mt19937_64 generator_;
    std::uniform_int_distribution<off_t> distribution_;
    long threshold_;
};

class ZipfianAccessPattern : public AccessPattern {
public:
    ZipfianAccessPattern(size_t page_count, double theta = 0.99, float write_ratio = 0.0)
        : AccessPattern(page_count), generator_(page_count, theta, write_ratio) {
        assert(theta >= 0.0 && theta < 1.0);
    }

    std::vector<off_t> WarmupSequence() override {
        std::vector<off_t> ret(static_cast<size_t>(GetPageCount()));
        std::iota(ret.begin(), ret.end(), 0);
        std::for_each(ret.begin(), ret.end(), [](off_t &i) { i *= PAGE_OFFSET; });
        return ret;
    }

    off_t GenerateNextOffset() override { return static_cast<off_t>(generator_.Next() * PAGE_OFFSET); }

    uint8_t GenerateRW() override { return generator_.get_op() == 'U' ? 1 : 0; }

private:
    ScrambledZipfianGenerator generator_;
};

class PartitionedAccessPattern : public ZipfianAccessPattern {
public:
    PartitionedAccessPattern(size_t page_count, int total_node_count, float write_ratio, double logical_node_index)
        : ZipfianAccessPattern(page_count / total_node_count, 0.99, write_ratio),
          total_node_count(total_node_count),
          logical_node_index(logical_node_index),
          page_count(page_count) {}
    off_t GenerateNextOffset() override {
        off_t offset = ZipfianAccessPattern::GenerateNextOffset();
        off_t node_offset = logical_node_index * (page_count / total_node_count);
        return (offset + node_offset) * PAGE_OFFSET;
    }
    uint8_t GenerateRW() override { return ZipfianAccessPattern::GenerateRW(); }

private:
    size_t page_count;
    int total_node_count;
    double logical_node_index;
};

class PartialPartitionedAccessPattern : public AccessPattern {
public:
    PartialPartitionedAccessPattern(size_t page_count, int total_nodes, int logical_node_index,
                                    double partition_percentage,   // e.g., 0.8 = 80% partitioned
                                    double partition_access_prob,  // e.g., 0.7 = 70% access partition
                                    double theta = 0.99, float write_ratio = 0.0)
        : AccessPattern(page_count),
          total_nodes_(total_nodes),
          logical_node_index_(logical_node_index),
          partition_percentage_(partition_percentage),
          partition_access_prob_(partition_access_prob),
          prob_dist_(0.0, 1.0),
          rd_(),
          generator_(rd_()) {
        // Calculate key space division
        size_t partitioned_keys = static_cast<size_t>(page_count * partition_percentage);
        size_t keys_per_partition = partitioned_keys / total_nodes;
        size_t shared_keys = page_count - partitioned_keys;
        // std::cout << "page_count: " << page_count << ", partition_percentage: " << partition_percentage
        //           << ", total_nodes: " << total_nodes << ", keys_per_partition: " << keys_per_partition
        //           << ", shared_keys: " << shared_keys << std::endl;

        // Store partition boundaries
        partition_start_ = logical_node_index * keys_per_partition;
        partition_size_ = keys_per_partition;
        shared_start_ = partitioned_keys;
        shared_size_ = shared_keys;

        // Create zipfian generator for this node's partition
        partition_zipfian_ = std::make_unique<ScrambledZipfianGenerator>(partition_size_, theta, write_ratio);

        // Create zipfian generator for shared space
        if (shared_size_ > 0) {
            shared_zipfian_ = std::make_unique<ScrambledZipfianGenerator>(shared_size_, theta, write_ratio);
        } else {
            shared_zipfian_ = nullptr;
        }

        // std::cout << "PartitionedAccessPattern initialized with: "
        //           << "partition_start: " << partition_start_ << ", partition_size: " << partition_size_
        //           << ", shared_start: " << shared_start_ << ", shared_size: " << shared_size_ << std::endl;
        // exit(0);
    }

    off_t GenerateNextOffset() override {
        // Determine: partition access or shared access?
        bool access_partition = (prob_dist_(generator_) < partition_access_prob_);

        if (access_partition || partition_percentage_ == 1.0) {
            // Access this node's partition
            uint64_t offset_in_partition = partition_zipfian_->Next();
            return static_cast<off_t>((partition_start_ + offset_in_partition) * PAGE_OFFSET);
        } else {
            // Access shared space
            if (shared_zipfian_ == nullptr) {
                throw std::runtime_error("Shared space is not allowed to be empty");
            }
            uint64_t offset_in_shared = shared_zipfian_->Next();
            return static_cast<off_t>((shared_start_ + offset_in_shared) * PAGE_OFFSET);
        }
    }

    uint8_t GenerateRW() override {
        // Randomly choose which generator to query for operation type
        if (prob_dist_(generator_) < partition_access_prob_) {
            return partition_zipfian_->get_op() == 'U' ? 1 : 0;
        } else {
            return shared_zipfian_->get_op() == 'U' ? 1 : 0;
        }
    }

    std::vector<off_t> WarmupSequence() override {
        // Warmup entire key space (both partitioned and shared)
        std::vector<off_t> ret(static_cast<size_t>(GetPageCount()));
        std::iota(ret.begin(), ret.end(), 0);
        std::for_each(ret.begin(), ret.end(), [](off_t &i) { i *= PAGE_OFFSET; });
        return ret;
    }

private:
    int total_nodes_;
    int logical_node_index_;
    double partition_percentage_;
    double partition_access_prob_;

    size_t partition_start_;
    size_t partition_size_;
    size_t shared_start_;
    size_t shared_size_;

    std::unique_ptr<ScrambledZipfianGenerator> partition_zipfian_;
    std::unique_ptr<ScrambledZipfianGenerator> shared_zipfian_;

    std::random_device rd_;
    std::mt19937_64 generator_;
    std::uniform_real_distribution<> prob_dist_;
};
class PartialPartitionedRangeRandomAccessPattern : public AccessPattern {
public:
    PartialPartitionedRangeRandomAccessPattern(size_t page_count, int total_nodes, int logical_node_index,
                                               double partition_percentage,   // e.g., 0.8 = 80% partitioned
                                               double partition_access_prob,  // e.g., 0.7 = 70% access partition
                                               double theta = 0.99, float write_ratio = 0.0)
        : AccessPattern(page_count),
          total_nodes_(total_nodes),
          logical_node_index_(logical_node_index),
          partition_percentage_(partition_percentage),
          partition_access_prob_(partition_access_prob),
          prob_dist_(0.0, 1.0),
          rd_(),
          generator_(rd_()) {
        // Calculate key space division
        size_t partitioned_keys = static_cast<size_t>(page_count * partition_percentage);
        size_t keys_per_partition = partitioned_keys / total_nodes;
        size_t shared_keys = page_count - partitioned_keys;
        std::cout << "page_count: " << page_count << ", partition_percentage: " << partition_percentage
                  << ", total_nodes: " << total_nodes << ", keys_per_partition: " << keys_per_partition
                  << ", shared_keys: " << shared_keys << std::endl;

        // Store partition boundaries
        partition_start_ = logical_node_index * keys_per_partition;
        partition_size_ = keys_per_partition;
        shared_start_ = partitioned_keys;
        shared_size_ = shared_keys;

        // Create zipfian generator for this node's partition
        // partition_zipfian_ = std::make_unique<ScrambledZipfianGenerator>(partition_size_, theta, write_ratio);
        partition_zipfian_ = std::make_unique<RangeRandomAccessPattern>(
            partition_start_, (partition_start_ + partition_size_ - 1), write_ratio);

        // Create zipfian generator for shared space
        if (shared_size_ > 0) {
            shared_zipfian_ = std::make_unique<ScrambledZipfianGenerator>(shared_size_, theta, write_ratio);
        } else {
            shared_zipfian_ = nullptr;
        }

        std::cout << "PartitionedAccessPattern initialized with: "
                  << "partition_start: " << partition_start_ << ", partition_size: " << partition_size_
                  << ", shared_start: " << shared_start_ << ", shared_size: " << shared_size_ << std::endl;
        // exit(0);
    }

    off_t GenerateNextOffset() override {
        // Determine: partition access or shared access?
        bool access_partition = (prob_dist_(generator_) < partition_access_prob_);

        if (access_partition || partition_percentage_ == 1.0) {
            // Access this node's partition
            uint64_t offset_in_partition = partition_zipfian_->GenerateNextOffset();
            return static_cast<off_t>((partition_start_ + offset_in_partition) * PAGE_OFFSET);
        } else {
            // Access shared space
            if (shared_zipfian_ == nullptr) {
                throw std::runtime_error("Shared space is not allowed to be empty");
            }
            uint64_t offset_in_shared = shared_zipfian_->Next();
            return static_cast<off_t>((shared_start_ + offset_in_shared) * PAGE_OFFSET);
        }
    }

    uint8_t GenerateRW() override {
        // Randomly choose which generator to query for operation type
        if (prob_dist_(generator_) < partition_access_prob_) {
            return partition_zipfian_->GenerateRW();
        } else {
            return shared_zipfian_->get_op() == 'U' ? 1 : 0;
        }
    }

    std::vector<off_t> WarmupSequence() override {
        // Warmup entire key space (both partitioned and shared)
        std::vector<off_t> ret(static_cast<size_t>(GetPageCount()));
        std::iota(ret.begin(), ret.end(), 0);
        std::for_each(ret.begin(), ret.end(), [](off_t &i) { i *= PAGE_OFFSET; });
        return ret;
    }

private:
    int total_nodes_;
    int logical_node_index_;
    double partition_percentage_;
    double partition_access_prob_;

    size_t partition_start_;
    size_t partition_size_;
    size_t shared_start_;
    size_t shared_size_;

    std::unique_ptr<RangeRandomAccessPattern> partition_zipfian_;
    std::unique_ptr<ScrambledZipfianGenerator> shared_zipfian_;

    std::random_device rd_;
    std::mt19937_64 generator_;
    std::uniform_real_distribution<> prob_dist_;
};

class Zipfian_file_acc : public AccessPattern {
public:
    Zipfian_file_acc(size_t page_count, std::string filename_ = "/mydata/ycsb/c")
        : AccessPattern(page_count), filename(filename_) {
        read_file();
    }
    std::vector<off_t> WarmupSequence() override {
        std::vector<off_t> ret(static_cast<size_t>(GetPageCount()));
        std::iota(ret.begin(), ret.end(), 0);
        std::for_each(ret.begin(), ret.end(), [](off_t &i) { i *= PAGE_OFFSET; });

        return ret;
    }

    off_t GenerateNextOffset() override { return next() * PAGE_OFFSET; }

private:
    std::vector<std::pair<uint64_t, char>> ops;
    size_t next() {
        if (current_index_ >= ops.size()) {
            read_file();
            current_index_ = 0;
        }
        return ops[current_index_++].first;
    }
    void read_file() {
        std::ifstream file(filename);
        if (file.is_open()) {
            std::string line;
            while (std::getline(file, line)) {
                std::istringstream iss(line);
                uint64_t key;
                int node;
                char op;
                if (iss >> key >> node >> op) {
                    ops.emplace_back(key, op);
                }
            }
            file.close();
        } else {
            std::cerr << "Unable to open file: " << filename << "\n";
        }
        file.close();
    }
    std::vector<off_t> page_ids_;
    uint64_t current_index_ = 1;
    std::string filename;
};

class HotRandomAccessPattern : public RandomAccessPattern {
public:
    HotRandomAccessPattern(size_t page_count, float write_ratio = 0.0) : RandomAccessPattern(page_count, write_ratio) {}

    std::vector<off_t> WarmupSequence() override {
        std::vector<off_t> ret(static_cast<size_t>(GetPageCount()));
        std::iota(ret.begin(), ret.end(), 0);
        std::for_each(ret.begin(), ret.end(), [](off_t &i) { i *= PAGE_OFFSET; });

        std::default_random_engine rng(rd_());
        std::shuffle(ret.begin(), ret.end(), rng);
        return ret;
    }
};

/**
 * Assume 3 numa nodes, thread ids are placed on the numa nodes in round robin order
 * pages are preloaded in nodes' local thread evenly in round robin order
 *
 * Illustration: assume: in total 9 pages, node 2 is local
 * |  1  |  |  2  |  |  3  | <- node (numa node 0 is cxl node)
 * |0|3|6|  |1|4|7|  |2|5|8| <- cached page id
 * |*| | |  |*|*|*|  |*| | | <- uniform random access among * with shared_ratio=0.33
 * |*|*| |  |*|*|*|  |*|*| | <- uniform random access among * with shared_ratio=0.66
 * |*|*|*|  |*|*|*|  |*|*|*| <- uniform random access among * with shared_ratio=1.0
 * random access page # = (1 + 2*ratio)*(num_pages/3)
 */
class HotRandomPartialSharedAccessPattern : public RandomAccessPattern {
public:
    HotRandomPartialSharedAccessPattern(size_t page_count, int numa_node, double shared_ratio = 1.0)
        : RandomAccessPattern(page_count) {
        double d_page_cnt = static_cast<double>(page_count);
        double local_page_count = d_page_cnt / 3.0;
        double shared_page_count_per_node = local_page_count * shared_ratio;

        local_page_count_ = static_cast<uint64_t>(local_page_count);
        shared_page_count_per_node_ = static_cast<uint64_t>(shared_page_count_per_node);
        numa_node_ = numa_node;

        // if (numa_node_ <= 0) std::cerr << "numa node must be positive integer since node 0 is for cxl mem" <<
        // std::endl;

        // LOG(INFO) << "init HotRandomPartialSharedAccessPattern w shared ratio: " << shared_ratio
        //           << ", random page range: " << local_page_count_ + 2 * shared_page_count_per_node_ << ", on numa
        //           node "
        //           << numa_node;
        // reinit w correct page number
        distribution_ = std::uniform_int_distribution<off_t>(
            0, static_cast<size_t>(local_page_count_ + 2 * shared_page_count_per_node_ - 1));
    }

    std::vector<off_t> WarmupSequence() override {
        std::vector<off_t> ret(static_cast<size_t>(GetPageCount()));
        std::iota(ret.begin(), ret.end(), 0);
        std::for_each(ret.begin(), ret.end(), [](off_t &i) { i *= PAGE_OFFSET; });

        // std::default_random_engine rng(rd_());
        // std::shuffle(ret.begin(), ret.end(), rng);
        return ret;
    }

    off_t GenerateNextOffset() override {
        off_t page_slot_idx;
        uint32_t numa_node = UINT32_MAX, low_boundary;
        // int rc = getcpu(NULL, &numa_node);
        // CHECK(rc != -1) << "getcpu() failed";
        // if (numa_node <= 0) std::cerr << "numa node must be positive integer since node 0 is for cxl mem" <<
        // std::endl;
        numa_node = numa_node_;

        auto rand_num = distribution_(generator_);
        if (rand_num < local_page_count_) {
            /* Case 1) Local page mapping */
            low_boundary = 0;
        } else if (rand_num < (local_page_count_ + shared_page_count_per_node_)) {
            /* Case 2) Min remote nid page mapping */
            numa_node = MinOrMaxNumaNodeExcludingMyself(numa_node, false);
            low_boundary = local_page_count_;
        } else {
            /* Case 3) Max remote nid page mapping */
            numa_node = MinOrMaxNumaNodeExcludingMyself(numa_node, true);
            low_boundary = local_page_count_ + shared_page_count_per_node_;
        }
        page_slot_idx = (rand_num - low_boundary) * 3 + (numa_node - 1);

        // CHECK(page_slot_idx < 131072) << "page idx larger than max, page_slot_idx: " << page_slot_idx
        //                               << ", rand_num: " << rand_num << ", low_boundary: " << low_boundary
        //                               << ", numa_node: " << numa_node;
        return page_slot_idx * PAGE_OFFSET;
    }

private:
    uint32_t MinOrMaxNumaNodeExcludingMyself(uint32_t my_nid, bool max) {
        std::vector<int> numbers = {1, 2, 3};
        numbers.erase(std::remove(numbers.begin(), numbers.end(), my_nid), numbers.end());
        if (max) return *std::max_element(numbers.begin(), numbers.end());
        return *std::min_element(numbers.begin(), numbers.end());
    }

    int numa_node_;
    uint64_t local_page_count_;
    uint64_t shared_page_count_per_node_;
};

class HotspotAccessPattern : public AccessPattern {
public:
    HotspotAccessPattern(size_t page_count, float hot_ratio = 0.1, float write_ratio = 0.0)
        : AccessPattern(page_count),
          rd_(),
          generator_(rd_()),
          hot_ratio_(hot_ratio),
          prob_distr_(0.0, 1.0),
          write_ratio_(write_ratio) {
        size_t hot_page_count = static_cast<size_t>(static_cast<double>(page_count) * static_cast<double>(hot_ratio));
        hotspot_distribution_ = std::uniform_int_distribution<off_t>(0, hot_page_count - 1);
        coldspot_distribution_ = std::uniform_int_distribution<off_t>(hot_page_count, GetPageCount() - 1);
        // LOG(INFO) << "Access pattern HotspotAccessPattern: page_count " << page_count << "| hot_page_count "
        //           << hot_page_count << "| write_ratio " << write_ratio;
    }

    std::vector<off_t> WarmupSequence() override {
        std::vector<off_t> ret(static_cast<size_t>(GetPageCount()));
        std::iota(ret.begin(), ret.end(), 0);
        std::for_each(ret.begin(), ret.end(), [](off_t &i) { i *= PAGE_OFFSET; });

        return ret;
    }

    off_t GenerateNextOffset() override {
        if (prob_distr_(generator_) < (1.0 - hot_ratio_)) {
            // if (prob_distr_(generator_) < 0.99) {
            return hotspot_distribution_(generator_) * PAGE_OFFSET;
        } else {
            return coldspot_distribution_(generator_) * PAGE_OFFSET;
        }
    }

    uint8_t GenerateRW() override { return prob_distr_(generator_) > write_ratio_ ? 0 : 1; }

private:
    float hot_ratio_;
    float write_ratio_;
    std::random_device rd_;
    std::mt19937_64 generator_;
    std::uniform_int_distribution<off_t> hotspot_distribution_;
    std::uniform_int_distribution<off_t> coldspot_distribution_;
    std::uniform_real_distribution<> prob_distr_;
};

// YCSB Workload D Pattern - Read Latest with dynamic inserts
// 95% read (biased toward recently inserted), 5% insert
class YCSBWorkloadDPattern : public AccessPattern {
public:
    YCSBWorkloadDPattern(size_t initial_count, CounterGenerator *shared_counter, float insert_ratio = 0.05)
        : AccessPattern(initial_count),
          shared_counter_(shared_counter),
          insert_ratio_(insert_ratio),
          last_op_(Operation::READ),
          last_insert_key_(0) {
        read_key_gen_ = std::make_unique<SkewedLatestGenerator>(*shared_counter_);
    }

    Operation GenerateOperation() override {
        last_op_ = (utils::ThreadLocalRandomDouble() < insert_ratio_) ? Operation::INSERT : Operation::READ;
        return last_op_;
    }

    off_t GenerateNextOffset() override {
        if (last_op_ == Operation::INSERT) {
            last_insert_key_ = shared_counter_->Next();
            return static_cast<off_t>(last_insert_key_) * PAGE_OFFSET;
        }
        return static_cast<off_t>(read_key_gen_->Next()) * PAGE_OFFSET;
    }

    uint8_t GenerateRW() override { return last_op_ == Operation::READ ? 0 : 1; }

    std::vector<off_t> WarmupSequence() override {
        // Return sequence for initial records only
        std::vector<off_t> ret(static_cast<size_t>(GetPageCount()));
        std::iota(ret.begin(), ret.end(), 0);
        std::for_each(ret.begin(), ret.end(), [](off_t &i) { i *= PAGE_OFFSET; });
        return ret;
    }

private:
    CounterGenerator *shared_counter_;  // Shared across all threads
    std::unique_ptr<SkewedLatestGenerator> read_key_gen_;
    float insert_ratio_;
    Operation last_op_;
    uint64_t last_insert_key_;
};

// YCSB Workload F Pattern - Read-Modify-Write
// 50% read, 50% read-modify-write (Zipfian distribution)
class YCSBWorkloadFPattern : public AccessPattern {
public:
    YCSBWorkloadFPattern(size_t page_count, double theta = 0.99, float rmw_ratio = 0.5)
        : AccessPattern(page_count), key_gen_(page_count, theta), rmw_ratio_(rmw_ratio), last_op_(Operation::READ) {}

    Operation GenerateOperation() override {
        last_op_ = (utils::ThreadLocalRandomDouble() < rmw_ratio_) ? Operation::READ_MODIFY_WRITE : Operation::READ;
        return last_op_;
    }

    off_t GenerateNextOffset() override {
        last_key_ = static_cast<off_t>(key_gen_.Next()) * PAGE_OFFSET;
        return last_key_;
    }

    uint8_t GenerateRW() override {
        // For backward compatibility: RMW counts as write
        return last_op_ == Operation::READ ? 0 : 1;
    }

    std::vector<off_t> WarmupSequence() override {
        std::vector<off_t> ret(static_cast<size_t>(GetPageCount()));
        std::iota(ret.begin(), ret.end(), 0);
        std::for_each(ret.begin(), ret.end(), [](off_t &i) { i *= PAGE_OFFSET; });
        return ret;
    }

private:
    ScrambledZipfianGenerator key_gen_;
    float rmw_ratio_;
    Operation last_op_;
    off_t last_key_;
};

// YCSB Workload A/B/C Pattern - Standard Zipfian with configurable read/update ratio
class YCSBWorkloadABCPattern : public AccessPattern {
public:
    YCSBWorkloadABCPattern(size_t page_count, double theta = 0.99, float update_ratio = 0.5)
        : AccessPattern(page_count),
          key_gen_(page_count, theta),
          update_ratio_(update_ratio),
          last_op_(Operation::READ) {}

    Operation GenerateOperation() override {
        last_op_ = (utils::ThreadLocalRandomDouble() < update_ratio_) ? Operation::UPDATE : Operation::READ;
        return last_op_;
    }

    off_t GenerateNextOffset() override { return static_cast<off_t>(key_gen_.Next()) * PAGE_OFFSET; }

    uint8_t GenerateRW() override { return last_op_ == Operation::READ ? 0 : 1; }

    std::vector<off_t> WarmupSequence() override {
        std::vector<off_t> ret(static_cast<size_t>(GetPageCount()));
        std::iota(ret.begin(), ret.end(), 0);
        std::for_each(ret.begin(), ret.end(), [](off_t &i) { i *= PAGE_OFFSET; });
        return ret;
    }

private:
    ScrambledZipfianGenerator key_gen_;
    float update_ratio_;
    Operation last_op_;
};

}  // namespace rackobj::benchmark
