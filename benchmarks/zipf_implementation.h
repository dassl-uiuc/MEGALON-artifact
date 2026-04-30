#pragma once

#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>  // Required for numeric_limits
#include <mutex>
#include <random>
#include <stdexcept>

namespace utils {
// inline double ThreadLocalRandomDouble() {
//     thread_local static std::mt19937_64 generator(std::random_device{}());
//     thread_local static std::uniform_real_distribution<double> distribution(0.0, 1.0);
//     return distribution(generator);
// }

inline double ThreadLocalRandomDouble(double min = 0.0, double max = 1.0) {
    static thread_local std::random_device rd;
    static thread_local std::minstd_rand rn(rd());
    static thread_local std::uniform_real_distribution<double> uniform(min, max);
    return uniform(rn);
}

const uint64_t kFNVOffsetBasis64 = 0xCBF29CE484222325ull;
const uint64_t kFNVPrime64 = 1099511628211ull;

inline uint64_t FNVHash64(uint64_t val) {
    uint64_t hash = kFNVOffsetBasis64;

    for (int i = 0; i < 8; i++) {
        uint64_t octet = val & 0x00ff;
        val = val >> 8;

        hash = hash ^ octet;
        hash = hash * kFNVPrime64;
    }
    return hash;
}

inline double Zeta(uint64_t num, double theta) {
    if (num == 0) return 0.0;
    double zeta = 0.0;
    for (uint64_t i = 1; i <= num; ++i) {
        zeta += 1.0 / std::pow(static_cast<double>(i), theta);
    }
    return zeta;
}
}  // namespace utils

// Minimal Generator interface placeholder
template <typename T>
class Generator {
public:
    virtual ~Generator() = default;
    virtual T Next() = 0;
};

class ZipfianGenerator : public Generator<uint64_t> {
public:
    static constexpr double kZipfianConst = 0.99;
    static constexpr uint64_t kMaxNumItems = 1ULL << 40;  // Adjusted practical limit

    ZipfianGenerator(const ZipfianGenerator&) = default;
    ZipfianGenerator& operator=(const ZipfianGenerator&) = default;

    ZipfianGenerator(uint64_t num_items, double theta = kZipfianConst, double write_ratio = 0.0)
        : ZipfianGenerator(0, num_items - 1, theta) {
        if (num_items == 0) {
            throw std::invalid_argument("num_items must be > 0");
        }
        write_ratio_ = write_ratio;
    }

    inline char get_op() {
        double random_number = utils::ThreadLocalRandomDouble();
        if (random_number < write_ratio_) {
            return 'U';
        }
        return 'R';
    }

    ZipfianGenerator(uint64_t min, uint64_t max, double zipfian_const = kZipfianConst)
        : ZipfianGenerator(min, max, zipfian_const, Zeta(max - min + 1, zipfian_const)) {}

    ZipfianGenerator(uint64_t min, uint64_t max, double zipfian_const, double zeta_n)
        : items_(max - min + 1),
          base_(min),
          theta_(zipfian_const),
          zeta_n_(zeta_n),
          alpha_(1.0 / (1.0 - theta_)),
          count_for_zeta_(items_),
          allow_count_decrease_(false) {  // allow_count_decrease_ defaults to false
        if (items_ < 2) {
            throw std::invalid_argument("Number of items must be at least 2");
        }
        if (items_ >= kMaxNumItems) {
            throw std::invalid_argument("Number of items exceeds maximum limit");
        }
        zeta_2_ = Zeta(2, theta_);
        eta_ = Eta();
    }

    uint64_t Next(uint64_t num);

    uint64_t Next() override { return Next(items_); }

private:
    double Eta() { return (1.0 - std::pow(2.0 / count_for_zeta_, 1.0 - theta_)) / (1.0 - zeta_2_ / zeta_n_); }

    static double Zeta(uint64_t last_num, uint64_t cur_num, double theta, double last_zeta) {
        double zeta = last_zeta;
        if (cur_num < last_num) {
            throw std::invalid_argument("cur_num must be >= last_num");
        }
        for (uint64_t i = last_num + 1; i <= cur_num; ++i) {
            zeta += 1.0 / std::pow(static_cast<double>(i), theta);
        }
        return zeta;
    }

    static double Zeta(uint64_t num, double theta) {
        if (num == 0) return 0.0;  // Handle edge case
        return Zeta(0, num, theta, 0.0);
    }

    uint64_t items_;
    uint64_t base_;
    double theta_;
    double zeta_n_;
    double eta_;
    double alpha_;
    double zeta_2_;
    uint64_t count_for_zeta_;
    std::mutex mutex_;
    bool allow_count_decrease_;
    double write_ratio_;
};

inline uint64_t ZipfianGenerator::Next(uint64_t num) {
    if (num < 2) {
        throw std::domain_error("Number of items must be >= 2");
    }
    if (num >= kMaxNumItems) {
        throw std::domain_error("Number of items exceeds maximum limit");
    }

    if (num != count_for_zeta_) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (num > count_for_zeta_) {
            zeta_n_ = Zeta(count_for_zeta_, num, theta_, zeta_n_);
            count_for_zeta_ = num;
            eta_ = Eta();
        } else if (num < count_for_zeta_ && allow_count_decrease_) {
            // Decreasing count requires recalculating zeta from scratch or storing
            // intermediate values, which is complex. YCSB-cpp leaves this as TODO.
            // We'll recompute from scratch if allowed.
            zeta_n_ = Zeta(num, theta_);
            count_for_zeta_ = num;
            eta_ = Eta();
        } else if (num < count_for_zeta_ && !allow_count_decrease_) {
            throw std::logic_error("Dynamic decrease in item count not allowed/implemented");
        }
        // If num == count_for_zeta_, no update needed.
    }

    // Use count_for_zeta_ for calculations as zeta_n_ and eta_ are based on it.
    double u = utils::ThreadLocalRandomDouble();
    double uz = u * zeta_n_;

    uint64_t result;
    if (uz < 1.0) {
        result = base_;
    } else if (uz < 1.0 + std::pow(0.5, theta_)) {
        result = base_ + 1;
    } else {
        result = base_ + static_cast<uint64_t>(count_for_zeta_ * std::pow(eta_ * u - eta_ + 1.0, alpha_));
    }

    // Ensure the result doesn't exceed the max value for the current range.
    uint64_t max_value = base_ + count_for_zeta_ - 1;
    if (result > max_value) {
        result = max_value;
    }

    return result;
}

class ScrambledZipfianGenerator : public Generator<uint64_t> {
public:
    ScrambledZipfianGenerator(uint64_t min, uint64_t max, double theta, double write_ratio)
        : base_(min),
          num_items_(max - min + 1),
          generator_(0, kItemCount, theta, theta == kUsedZipfianConstant ? kZetan : utils::Zeta(num_items_, theta)),
          write_ratio_(write_ratio) {}

    ScrambledZipfianGenerator(uint64_t num_items, double theta = kUsedZipfianConstant, double write_ratio = 0.0)
        : ScrambledZipfianGenerator(0, num_items - 1, theta, write_ratio) {}

    inline char get_op() {
        double random_number = utils::ThreadLocalRandomDouble();
        if (random_number < write_ratio_) {
            return 'U';
        }
        return 'R';
    }

    inline uint64_t Next() override { return Scramble(generator_.Next()); }

private:
    static constexpr double kUsedZipfianConstant = 0.99;
    static constexpr double kZetan = 26.46902820178302;
    static constexpr uint64_t kItemCount = 10000000000LL;
    const uint64_t base_;
    const uint64_t num_items_;
    ZipfianGenerator generator_;
    double write_ratio_;

    uint64_t Scramble(uint64_t value) const;
};

inline uint64_t ScrambledZipfianGenerator::Scramble(uint64_t value) const {
    return base_ + utils::FNVHash64(value) % num_items_;
}

// CounterGenerator - thread-safe atomic counter for YCSB workload D inserts
class CounterGenerator : public Generator<uint64_t> {
    std::atomic<uint64_t> counter_;

public:
    explicit CounterGenerator(uint64_t start) : counter_(start) {}

    uint64_t Next() override { return counter_.fetch_add(1); }

    uint64_t Last() const { return counter_.load() - 1; }

    void Set(uint64_t value) { counter_.store(value); }

    uint64_t Current() const { return counter_.load(); }
};

// SkewedLatestGenerator - generates keys biased toward recent inserts for YCSB workload D
// Uses Zipfian distribution but maps it to favor recently inserted keys
class SkewedLatestGenerator : public Generator<uint64_t> {
    CounterGenerator& basis_;
    ZipfianGenerator zipfian_;
    std::atomic<uint64_t> last_;

public:
    explicit SkewedLatestGenerator(CounterGenerator& counter)
        : basis_(counter), zipfian_(std::max(basis_.Last() + 1, uint64_t(2))), last_(0) {
        Next();
    }

    uint64_t Next() override {
        uint64_t max = basis_.Last();
        if (max < 2) {
            last_.store(0);
            return 0;
        }
        // Generate value biased toward latest (higher) keys
        // zipfian_.Next(max+1) returns value in [0, max]
        // Subtracting from max inverts distribution to favor recent keys
        uint64_t result = max - zipfian_.Next(max + 1);
        last_.store(result);
        return result;
    }

    uint64_t Last() const { return last_.load(); }
};
