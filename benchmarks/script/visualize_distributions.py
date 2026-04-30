import numpy as np
import matplotlib.pyplot as plt
import subprocess
import json
import argparse
import os
import shutil
from scipy.stats import gaussian_kde

def compile_cpp_sampler():
    """Compile the C++ sampler program using direct g++ compilation"""
    # Create simplified access_pattern.h without Abseil dependencies
    header_code = """
    #pragma once

    #include <numa.h>
    #include <sys/types.h>

    #include <algorithm>
    #include <cassert>
    #include <fstream>
    #include <iostream>
    #include <random>
    #include <sstream>
    #include <vector>

    namespace rackobj::benchmark {

    static constexpr off_t PAGE_OFFSET = 4096;

    class AccessPattern {
    public:
        explicit AccessPattern(size_t page_count) : page_count_(static_cast<off_t>(page_count)) {}

        virtual ~AccessPattern() {}

        virtual off_t GenerateNextOffset() = 0;

        /** 0: read; else: write */
        virtual uint8_t GenerateRW() { return 0; }

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
            std::cout << "pattern init with threshold " << threshold_ << std::endl;
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

    class ZipfianAccessPattern : public AccessPattern {
    public:
        ZipfianAccessPattern(size_t page_count, double theta = 0.99, float write_ratio = 0.0)
            : AccessPattern(page_count), generator_(page_count, write_ratio) {
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
        class ZipfianGenerator {
        public:
            ZipfianGenerator(size_t n, float write_ratio) : n_(n), write_ratio_(write_ratio) {
                zeta_ = zeta(n_, theta_);
                eta_ = (1 - std::pow(2.0 / n_, 1 - theta_)) / (1 - zeta2_ / zeta_);
            }

            size_t Next() {
                double u = static_cast<double>(rand()) / RAND_MAX;
                double uz = u * zeta_;
                if (uz < 1.0) return 0;
                if (uz < 1.0 + std::pow(0.5, theta_)) return 1;
                return static_cast<size_t>(n_ * std::pow(eta_ * u - eta_ + 1, alpha_));
            }

            char get_op() {
                return (static_cast<float>(rand()) / RAND_MAX) < write_ratio_ ? 'U' : 'R';
            }

        private:
            static constexpr double theta_ = 0.99;
            static constexpr double zeta2_ = 1.6449;
            static constexpr double alpha_ = 1.0 / (1.0 - theta_);

            double zeta(size_t n, double theta) {
                double sum = 0;
                for (size_t i = 1; i <= n; i++) {
                    sum += 1.0 / std::pow(i, theta);
                }
                return sum;
            }

            size_t n_;
            float write_ratio_;
            double zeta_;
            double eta_;
        };

        ZipfianGenerator generator_;
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
            std::cout << "Access pattern HotspotAccessPattern: page_count " << page_count << "| hot_page_count "
                      << hot_page_count << "| write_ratio " << write_ratio << std::endl;
        }

        std::vector<off_t> WarmupSequence() override {
            std::vector<off_t> ret(static_cast<size_t>(GetPageCount()));
            std::iota(ret.begin(), ret.end(), 0);
            std::for_each(ret.begin(), ret.end(), [](off_t &i) { i *= PAGE_OFFSET; });
            return ret;
        }

        off_t GenerateNextOffset() {
            if (prob_distr_(generator_) < (1.0 - hot_ratio_)) {
                return hotspot_distribution_(generator_) * PAGE_OFFSET;
            } else {
                return coldspot_distribution_(generator_) * PAGE_OFFSET;
            }
        }

        uint8_t GenerateRW() { return prob_distr_(generator_) > write_ratio_ ? 0 : 1; }

    private:
        float hot_ratio_;
        float write_ratio_;
        std::random_device rd_;
        std::mt19937_64 generator_;
        std::uniform_int_distribution<off_t> hotspot_distribution_;
        std::uniform_int_distribution<off_t> coldspot_distribution_;
        std::uniform_real_distribution<> prob_distr_;
    };

    }  // namespace rackobj::benchmark
    """
    
    with open("access_pattern.h", "w") as f:
        f.write(header_code)
    
    # Write the C++ source
    cpp_code = """
    #include <iostream>
    #include <vector>
    #include <random>
    #include <fstream>
    #include "access_pattern.h"
    
    using namespace rackobj::benchmark;
    
    int main(int argc, char** argv) {
        if (argc < 4) {
            std::cerr << "Usage: " << argv[0] << " <pattern_type> <num_pages> <num_samples> [theta/hot_ratio]" << std::endl;
            return 1;
        }
        
        std::string pattern_type = argv[1];
        size_t num_pages = std::stoul(argv[2]);
        size_t num_samples = std::stoul(argv[3]);
        
        std::vector<off_t> samples;
        samples.reserve(num_samples);
        
        if (pattern_type == "sequential") {
            SequentialAccessPattern pattern(num_pages);
            for (size_t i = 0; i < num_samples; ++i) {
                samples.push_back(pattern.GenerateNextOffset());
            }
        }
        else if (pattern_type == "random") {
            RandomAccessPattern pattern(num_pages);
            for (size_t i = 0; i < num_samples; ++i) {
                samples.push_back(pattern.GenerateNextOffset());
            }
        }
        else if (pattern_type == "zipfian") {
            double theta = argc > 4 ? std::stod(argv[4]) : 0.99;
            ZipfianAccessPattern pattern(num_pages, theta);
            for (size_t i = 0; i < num_samples; ++i) {
                samples.push_back(pattern.GenerateNextOffset());
            }
        }
        else if (pattern_type == "hotspot") {
            float hot_ratio = argc > 4 ? std::stof(argv[4]) : 0.1;
            HotspotAccessPattern pattern(num_pages, hot_ratio);
            for (size_t i = 0; i < num_samples; ++i) {
                samples.push_back(pattern.GenerateNextOffset());
            }
        }
        else {
            std::cerr << "Unknown pattern type: " << pattern_type << std::endl;
            return 1;
        }
        
        // Output samples as JSON array
        std::cout << "[";
        for (size_t i = 0; i < samples.size(); ++i) {
            std::cout << samples[i];
            if (i < samples.size() - 1) std::cout << ",";
        }
        std::cout << "]" << std::endl;
        
        return 0;
    }
    """
    
    with open("pattern_sampler.cpp", "w") as f:
        f.write(cpp_code)
    
    # Compile with g++
    compile_cmd = [
        "g++",
        "-std=c++17",
        "-I.",
        "pattern_sampler.cpp",
        "-o",
        "pattern_sampler"
    ]
    
    subprocess.run(compile_cmd, check=True)

def sample_pattern(pattern_type, num_pages, num_samples, param=None):
    """Sample from the C++ access pattern and return the samples"""
    cmd = ["./pattern_sampler", pattern_type, str(num_pages), str(num_samples)]
    if param is not None:
        cmd.append(str(param))
    
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"Failed to sample pattern: {result.stderr}")
    
    return np.array(json.loads(result.stdout))

def plot_distribution(samples, pattern_type, param=None):
    """Plot the distribution of samples and save to files"""
    # Create plots directory if it doesn't exist
    plots_dir = "access_pattern_plots"
    os.makedirs(plots_dir, exist_ok=True)
    
    # Generate filename based on pattern type and parameters
    filename_base = f"{pattern_type}"
    if param is not None:
        if pattern_type == 'zipfian':
            filename_base += f"_theta{param}"
        elif pattern_type == 'hotspot':
            filename_base += f"_hotratio{param}"
    
    # Plot 1: Access sequence
    plt.figure(figsize=(12, 5))
    plt.subplot(1, 2, 1)
    plt.plot(range(len(samples)), samples, '.', alpha=0.5)
    title = f'{pattern_type.capitalize()} Access Pattern'
    if param is not None:
        if pattern_type == 'zipfian':
            title += f' (θ={param})'
        elif pattern_type == 'hotspot':
            title += f' (hot_ratio={param})'
    plt.title(title + ' - Sequence')
    plt.xlabel('Access Number')
    plt.ylabel('Page Offset')
    plt.grid(True)
    
    # Plot 2: Distribution with smoothed line
    plt.subplot(1, 2, 2)
    # Create histogram
    n, bins, patches = plt.hist(samples, bins=min(50, len(np.unique(samples))), 
                              density=True, alpha=0.6, label='Histogram')
    
    # Add smoothed line using kernel density estimation
    kde = gaussian_kde(samples)
    x_range = np.linspace(min(samples), max(samples), 200)
    plt.plot(x_range, kde(x_range), 'r-', linewidth=2, label='Smoothed')
    
    plt.title(title + ' - Distribution')
    plt.xlabel('Page Offset')
    plt.ylabel('Density')
    plt.grid(True)
    plt.legend()
    
    plt.tight_layout()
    
    # Save the plot
    plot_path = os.path.join(plots_dir, f"{filename_base}.png")
    plt.savefig(plot_path, dpi=300, bbox_inches='tight')
    plt.close()
    
    print(f"Plot saved to: {plot_path}")

def plot_multiple_thetas(pattern_type, num_pages, num_samples, thetas):
    """Plot multiple theta values side by side for comparison"""
    plots_dir = "access_pattern_plots"
    os.makedirs(plots_dir, exist_ok=True)
    
    # Create a figure with subplots for each theta
    fig, axes = plt.subplots(len(thetas), 2, figsize=(15, 5*len(thetas)))
    
    for i, theta in enumerate(thetas):
        # Sample the pattern
        samples = sample_pattern(pattern_type, num_pages, num_samples, theta)
        
        # Plot sequence
        axes[i, 0].plot(range(len(samples)), samples, '.', alpha=0.5)
        axes[i, 0].set_title(f'{pattern_type.capitalize()} Access Pattern (θ={theta}) - Sequence')
        axes[i, 0].set_xlabel('Access Number')
        axes[i, 0].set_ylabel('Page Offset')
        axes[i, 0].grid(True)
        
        # Plot distribution
        n, bins, patches = axes[i, 1].hist(samples, bins=min(50, len(np.unique(samples))), 
                                         density=True, alpha=0.6, label='Histogram')
        
        # Add smoothed line
        kde = gaussian_kde(samples)
        x_range = np.linspace(min(samples), max(samples), 200)
        axes[i, 1].plot(x_range, kde(x_range), 'r-', linewidth=2, label='Smoothed')
        
        axes[i, 1].set_title(f'{pattern_type.capitalize()} Access Pattern (θ={theta}) - Distribution')
        axes[i, 1].set_xlabel('Page Offset')
        axes[i, 1].set_ylabel('Density')
        axes[i, 1].grid(True)
        axes[i, 1].legend()
    
    plt.tight_layout()
    
    # Save the plot
    plot_path = os.path.join(plots_dir, f"{pattern_type}_comparison.png")
    plt.savefig(plot_path, dpi=300, bbox_inches='tight')
    plt.close()
    
    print(f"Comparison plot saved to: {plot_path}")

def main():
    parser = argparse.ArgumentParser(description='Visualize access pattern distributions')
    parser.add_argument('--pattern', type=str, required=True,
                      choices=['sequential', 'random', 'zipfian', 'hotspot'],
                      help='Access pattern to visualize')
    parser.add_argument('--pages', type=int, default=1000,
                      help='Number of pages')
    parser.add_argument('--samples', type=int, default=1000,
                      help='Number of samples to generate')
    parser.add_argument('--theta', type=float, default=0.99,
                      help='Theta parameter for Zipfian distribution')
    parser.add_argument('--hot-ratio', type=float, default=0.1,
                      help='Hot ratio for hotspot pattern')
    parser.add_argument('--compare-thetas', action='store_true',
                      help='Compare multiple theta values for Zipfian distribution')
    
    args = parser.parse_args()
    
    # Compile the C++ sampler if it doesn't exist
    if not os.path.exists("pattern_sampler"):
        compile_cpp_sampler()
    
    if args.compare_thetas and args.pattern == 'zipfian':
        thetas = [0.99, 0.9, 0.8, 0.7]
        plot_multiple_thetas(args.pattern, args.pages, args.samples, thetas)
    else:
        # Sample from the pattern
        param = None
        if args.pattern == 'zipfian':
            param = args.theta
        elif args.pattern == 'hotspot':
            param = args.hot_ratio
        
        samples = sample_pattern(args.pattern, args.pages, args.samples, param)
        plot_distribution(samples, args.pattern, param)

if __name__ == '__main__':
    main() 