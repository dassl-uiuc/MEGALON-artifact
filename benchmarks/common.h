#pragma once

#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <condition_variable>
#include <iomanip>
#include <mutex>
#include <sstream>

// [NOTE] this breaks the abstraction of the benchmark code - we should not be using constants.h here
#include "../src/common/constants.h"
//
#include "absl/log/check.h"
#include "access_pattern.h"

#define CPU_FREQ_GHZ 2.1

pthread_barrier_t* InitializeBarrier(size_t subprocess_count) {
    shm_unlink("/barrier");
    int shm_fd = shm_open("/barrier", O_CREAT | O_EXCL | O_RDWR, 0666);
    CHECK(shm_fd != -1) << "Failed to open shared memory";
    int rc = ftruncate(shm_fd, sizeof(pthread_barrier_t));
    pthread_barrier_t* barrier =
        (pthread_barrier_t*)mmap(nullptr, sizeof(pthread_barrier_t), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    CHECK((void*)barrier != MAP_FAILED) << "failed to mmap shared memory";
    close(shm_fd);

    pthread_barrierattr_t barrier_attr;
    pthread_barrierattr_init(&barrier_attr);
    pthread_barrierattr_setpshared(&barrier_attr, PTHREAD_PROCESS_SHARED);
    pthread_barrier_init(barrier, &barrier_attr, subprocess_count);
    pthread_barrierattr_destroy(&barrier_attr);

    return barrier;
}

void ReinitializeBarrier(pthread_barrier_t* barrier, size_t subprocess_count) {
    int rc = pthread_barrier_destroy(barrier);
    CHECK(rc != -1) << "pthread_barrier_destroy";

    pthread_barrierattr_t barrier_attr;
    pthread_barrierattr_init(&barrier_attr);
    pthread_barrierattr_setpshared(&barrier_attr, PTHREAD_PROCESS_SHARED);
    rc = pthread_barrier_init(barrier, &barrier_attr, subprocess_count);
    CHECK(rc != -1) << "pthread_barrier_init";
    pthread_barrierattr_destroy(&barrier_attr);
}

std::unique_ptr<rackobj::benchmark::AccessPattern> InitializeAccessPattern(
    const std::string& pattern, const size_t key_space, const int numa_node = 0, const float arg1 = 0.0,
    const double arg2 = 1.0, const double arg3 = 1.0, const double arg4 = 0.99) {
    using namespace rackobj::benchmark;
    if (pattern == "random") {
        return std::make_unique<RandomAccessPattern>(key_space);
    } else if (pattern == "sequential") {
        return std::make_unique<SequentialAccessPattern>(key_space);
    } else if (pattern == "hotspot-local") {
        return std::make_unique<HotspotAccessPattern>(key_space, arg4, arg1);
    } else if (pattern == "hotspot") {
        return std::make_unique<HotspotAccessPattern>(key_space, 7040);
    } else if (pattern == "hotspot2") {
        return std::make_unique<HotspotAccessPattern>(key_space, 14080);
    } else if (pattern == "hotspot5") {
        return std::make_unique<HotspotAccessPattern>(key_space, 35200);
    } else if (pattern == "hotcache") {
        return std::make_unique<HotRandomAccessPattern>(key_space, arg1);
    } else if (pattern == "hotcachepartial") {
        return std::make_unique<HotRandomPartialSharedAccessPattern>(key_space / 2, numa_node, arg4);
    } else if (pattern == "zipfian") {
        return std::make_unique<ZipfianAccessPattern>(key_space, arg4, arg1);
    } else if (pattern == "partitioned") {
        return std::make_unique<PartitionedAccessPattern>(key_space, numa_node, arg2, arg1);
    } else if (pattern == "partial-partitioned") {
        // arg1 = write_ratio
        // arg2 = partition_percentage (e.g., 0.8 = 80% partitioned)
        // arg3 = partition_access_prob (e.g., 0.9 = 90% access partition)
        // arg4 = theta (zipfian skew parameter)
        // numa_node = logical_node_index
        // std::cout << "key_space: " << key_space << ", numa_node: " << numa_node << ", arg2: " << arg2
        //           << ", arg3: " << arg3 << ", arg4: " << arg4 << ", arg1: " << arg1 << std::endl;
        return std::make_unique<PartialPartitionedAccessPattern>(key_space,
                                                                 LOGICAL_NODE_NUM,  // total_nodes
                                                                 numa_node,         // logical_node_index
                                                                 arg2,              // partition_percentage
                                                                 arg3,              // partition_access_prob
                                                                 arg4,              // theta
                                                                 arg1               // write_ratio
        );
    }

    return nullptr;
}

template <typename T>
class SharedMemoryArray {
public:
    SharedMemoryArray(const char* name, size_t len) : name_(name) {
        int shm_fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0666);
        CHECK(shm_fd != -1) << "Failed to open shared memory";

        size_t array_length = (sizeof(T) * len);
        segment_length_ = array_length + sizeof(std::atomic_uint64_t) + sizeof(size_t);
        int rc = ftruncate(shm_fd, segment_length_);
        void* mmap_res = mmap(nullptr, segment_length_, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
        CHECK(mmap_res != MAP_FAILED) << "failed to mmap shared memory";

        ref_count_ = reinterpret_cast<std::atomic_uint64_t*>(mmap_res);
        length_ = reinterpret_cast<size_t*>(ref_count_ + 1);
        data_ = reinterpret_cast<T*>(length_ + 1);

        *length_ = len;
        ref_count_->fetch_add(1);

        close(shm_fd);
    }

    SharedMemoryArray(const char* name) : name_(name) {
        int shm_fd = shm_open(name, O_RDWR, 0666);
        CHECK(shm_fd != -1) << "Failed to open shared memory";

        struct stat stbuf;
        fstat(shm_fd, &stbuf);
        if (stbuf.st_size == 0) {
            LOG(FATAL) << "shared memory at " << name << " has size 0";
        }

        segment_length_ = static_cast<size_t>(stbuf.st_size);
        void* mmap_res = mmap(nullptr, segment_length_, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
        CHECK((void*)mmap_res != MAP_FAILED) << "failed to mmap shared memory";

        ref_count_ = reinterpret_cast<std::atomic_uint64_t*>(mmap_res);
        length_ = reinterpret_cast<size_t*>(ref_count_ + 1);
        data_ = reinterpret_cast<T*>(length_ + 1);

        ref_count_->fetch_add(1);
        close(shm_fd);
    }

    ~SharedMemoryArray() {
        if (ref_count_->fetch_sub(1) == 1) {
            shm_unlink(name_.c_str());
        }

        munmap((void*)ref_count_, segment_length_);
    }

    SharedMemoryArray(const SharedMemoryArray&) = delete;
    SharedMemoryArray(SharedMemoryArray&&) = default;

    SharedMemoryArray& operator=(const SharedMemoryArray&) = delete;
    SharedMemoryArray& operator=(SharedMemoryArray&&) = default;

    T& operator[](size_t index) {
        CHECK(index < *length_) << "Index " << index << " out of bounds of length " << *length_;
        return data_[index];
    }

    size_t size() { return *length_; }
    T* data() { return data_; }
    T* begin() { return data_; }
    T* end() { return data_ + (*length_); }

private:
    std::atomic_uint64_t* ref_count_;
    size_t* length_;
    T* data_;
    size_t segment_length_;
    std::string name_;
};

class SharedMemoryBarrier {
public:
    SharedMemoryBarrier(const char* name, size_t count) : name_(name) {
        int shm_fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0666);
        CHECK(shm_fd != -1) << "Failed to open shared memory";

        segment_length_ = sizeof(pthread_barrier_t) + sizeof(std::atomic_uint64_t);
        int rc = ftruncate(shm_fd, segment_length_);
        void* mmap_res = mmap(nullptr, segment_length_, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
        CHECK(mmap_res != MAP_FAILED) << "failed to mmap shared memory";

        ref_count_ = reinterpret_cast<std::atomic_uint64_t*>(mmap_res);
        barrier_ = reinterpret_cast<pthread_barrier_t*>(ref_count_ + 1);

        pthread_barrierattr_t barrier_attr;
        pthread_barrierattr_init(&barrier_attr);
        pthread_barrierattr_setpshared(&barrier_attr, PTHREAD_PROCESS_SHARED);
        pthread_barrier_init(barrier_, &barrier_attr, count);
        pthread_barrierattr_destroy(&barrier_attr);

        ref_count_->fetch_add(1);
        close(shm_fd);
    }

    SharedMemoryBarrier(const char* name) : name_(name) {
        int shm_fd = shm_open(name, O_RDWR, 0666);
        CHECK(shm_fd != -1) << "Failed to open shared memory";

        struct stat stbuf;
        fstat(shm_fd, &stbuf);
        segment_length_ = static_cast<size_t>(stbuf.st_size);
        void* mmap_res = mmap(nullptr, segment_length_, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
        CHECK((void*)mmap_res != MAP_FAILED) << "failed to mmap shared memory";

        ref_count_ = reinterpret_cast<std::atomic_uint64_t*>(mmap_res);
        barrier_ = reinterpret_cast<pthread_barrier_t*>(ref_count_ + 1);

        ref_count_->fetch_add(1);
        close(shm_fd);
    }

    ~SharedMemoryBarrier() {
        if (ref_count_->fetch_sub(1) == 1) {
            pthread_barrier_destroy(barrier_);
            shm_unlink(name_.c_str());
        }

        munmap((void*)ref_count_, segment_length_);
    }

    SharedMemoryBarrier(const SharedMemoryBarrier&) = delete;
    SharedMemoryBarrier(SharedMemoryBarrier&&) = default;

    SharedMemoryBarrier& operator=(const SharedMemoryBarrier&) = delete;
    SharedMemoryBarrier& operator=(SharedMemoryBarrier&&) = default;

    void Wait() { pthread_barrier_wait(barrier_); }

private:
    std::atomic_uint64_t* ref_count_;
    pthread_barrier_t* barrier_;
    size_t segment_length_;
    std::string name_;
};

class ThreadBarrier {
public:
    explicit ThreadBarrier(size_t count) : threshold(count), count(count), generation(0) {}

    void Wait() {
        std::unique_lock<std::mutex> lock(mutex);
        auto gen = generation;
        if (--count == 0) {
            generation++;
            count = threshold;
            condition.notify_all();
        } else {
            condition.wait(lock, [this, gen] { return gen != generation; });
        }
    }

private:
    std::mutex mutex;
    std::condition_variable condition;
    size_t threshold;
    size_t count;
    size_t generation;
};

int64_t cycles_to_nanoseconds(uint64_t cycles, double cpu_frequency_ghz) {
    return static_cast<int64_t>(cycles / cpu_frequency_ghz);
}

std::string double_to_string(double value) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << value;
    return oss.str();
}
