#include <iostream>
#include <thread>
#include <random>
#include <mutex>
#include <unordered_map>
#include <cstring>

#include "nr_hashmap.hpp"
#include "helper.hpp"
#include <unistd.h>

#define NOP 2500000
// #define NOP 253951
#define KEY_SPACE 10000000
#define LOG_PERIOD 500000
#define BGROUND_SYNC_US 10

using namespace NrFfi;

bool is_entry_empty(GCDEntry* entry) {
    for (int i = 0; i < NUM_NODE; i++) {
        if (entry->cn_array_[i].cn_) return false;
    }
    return true;
}

void print_entry(GCDEntry* entry) {
    std::cout << "{(" << entry->cn_array_[0].cn_ << ", " << entry->cn_array_[0].invalidate_
              << "), (" << entry->cn_array_[1].cn_ << ", " << entry->cn_array_[1].invalidate_
              << "), (" << entry->cn_array_[2].cn_ << ", " << entry->cn_array_[2].invalidate_
              << "), (" << entry->cn_array_[3].cn_ << ", " << entry->cn_array_[3].invalidate_
              << ")}" << std::endl;
}

void worker_thread(NrRapper* nrht_wrapper, uint64_t rid, uint64_t *a, uint64_t tid, uint64_t n_threads) {
    pin_thread_to_numa_node(rid + 1);
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint64_t> dist(1, KEY_SPACE);

    NrMeta* metadata = register_node_replicated(nrht_wrapper, rid);

    std::cout << "sleep..." << std::endl;
    sleep(2);

    for (int i = 0; i < NOP; ++i) {
        if (i % LOG_PERIOD == 0) std::cout << "finished " << i << std::endl;
        size_t key = i + NOP*tid;
        PutArray(metadata, BlockId(key), key, 0);
        // bool res = CheckPutArray(metadata, BlockId(key), reinterpret_cast<void*>(a + tid), 0);

        // bool res = CheckCoherence(metadata, BlockId(key));
        // // bool res = CheckPutArray(metadata, BlockId(key), reinterpret_cast<void*>(a + tid), 0);
        // if (res == true) {
        //     std::cerr << "coherence issue true";
        //     exit(1);
        // }
    }

    for (int i = 0; i < NOP; ++i) {
        if (i % LOG_PERIOD == 0) std::cout << "finished " << i << std::endl;
        size_t key = i + NOP*tid;
        bool res = CheckCoherence(metadata, BlockId(key));
        // bool res = CheckPutArray(metadata, BlockId(key), reinterpret_cast<void*>(a + tid), 0);
        if (res == true) {
            std::cerr << "coherence issue true";
            exit(1);
        }
    }

    for (int i = 0; i < NOP; ++i) {
        if (i % LOG_PERIOD == 0) std::cout << "finished " << i << std::endl;
        size_t key = i + NOP*tid;
        bool res = CheckCoherence(metadata, BlockId(key));
        // bool res = CheckPutArray(metadata, BlockId(key), reinterpret_cast<void*>(a + tid), 0);
        if (res == false) {
            std::cerr << "coherence issue false";
            exit(1);
        }
    }

    unregister_node_replicated(metadata);
}

int main() {
    // NrRapper* nrht_wrapper = create_node_replicated_numa_async(KEY_SPACE, BGROUND_SYNC_US);
    NrRapper* nrht_wrapper = create_node_replicated_numa(KEY_SPACE, 3); // 3 replicas

    uint64_t a = 0;
    std::vector<std::thread> threads;

    for (int i = 0; i < 9; ++i) {
        // rid set to 0, 1, 2
        // threads.emplace_back(worker_thread, nrht_wrapper, (i % 3), &a, i, 12);
        threads.emplace_back(worker_thread, nrht_wrapper, (i % 3), &a, i, 12);
    }

    std::cout << "waiting stop..." << std::endl;

    for (auto& t : threads) {
        if (t.joinable()) {
            t.join();
        }
    }

    free_node_replicated(nrht_wrapper);

    std::cout << "done" << std::endl;
    
    return 0;
}
