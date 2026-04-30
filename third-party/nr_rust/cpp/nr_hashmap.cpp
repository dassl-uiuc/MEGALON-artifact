#include <iostream>
#include <thread>
#include <random>
#include <mutex>
#include <unordered_map>

#include "nr_hashmap.hpp"
#include "helper.hpp"

#define NOP 2500000
#define KEY_SPACE 10000000
#define LOG_PERIOD 500000
#define BGROUND_SYNC_US 10

using namespace NrFfi;

void worker_thread(NrRapper* nrht_wrapper, uint64_t rid, uint64_t *a) {
    pin_thread_to_numa_node(rid + 1);
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint64_t> dist(1, KEY_SPACE);

    NrMeta* metadata = register_node_replicated(nrht_wrapper, rid);

    for (int i = 0; i < NOP; ++i) {
        if (i % LOG_PERIOD == 0) std::cout << "finished " << i << std::endl;
        uint64_t key = dist(gen);
        uint64_t value = dist(gen);
        CheckPut(metadata, BlockId(key), {0, reinterpret_cast<void *>(a)});
    }

    std::cout << std::endl << "start read " << std::endl;

    for (int i = 0; i < NOP/2; ++i) {
        if (i % LOG_PERIOD == 0) std::cout << "finished " << i << std::endl;
        uint64_t key = dist(gen);
        uint64_t value = dist(gen);
        GCDEntry entry = Get(metadata, BlockId(key));
        if (entry.cached_server_id_ != -1 && reinterpret_cast<uint64_t *>(entry.cn_) != a) {
            std::cerr << "pointer value does not match " << entry.cn_ << " " << a;
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

    for (int i = 0; i < 12; ++i) {
        // rid set to 0, 1, 2
        threads.emplace_back(worker_thread, nrht_wrapper, (i % 3), &a);
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