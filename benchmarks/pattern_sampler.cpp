
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
    