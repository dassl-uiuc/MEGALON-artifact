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
              << "), wmeta = " << entry->wmeta_idx << "}" << std::endl;
}

void print_key(BlockId *key) {
    std::cout << "{server_id_ = " << key->server_id_
              << ", inode_ = " << key->inode_
              << ", offset_ = " << key->offset_ << "}" << std::endl;
}

void worker_thread(NrRapper* nrht_wrapper, uint64_t rid, uint64_t *a, uint64_t tid, uint64_t n_threads) {
    pin_thread_to_numa_node(rid + 1);
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint64_t> dist(1, KEY_SPACE);

    NrMeta* metadata = register_node_replicated(nrht_wrapper, rid);

    sleep(2);

    for (uint64_t i = 0; i < NOP; ++i) {
        if (i % LOG_PERIOD == 0) std::cout << "finished " << i << std::endl;
        // uint64_t key = dist(gen);
        uint64_t key = tid + n_threads*i;
        BlockId blkkey = BlockId(key);
        // uint64_t value = dist(gen);
        auto res = CheckPutArray(metadata, blkkey, reinterpret_cast<size_t>(a + tid + n_threads*i), 0, -1);
        // bool res = CheckPutArray(metadata, BlockId(key), reinterpret_cast<void*>(a + tid), 0);
        if (res != 0) {
            // GCDEntry entry = Get(metadata, BlockId(key));
            // if (reinterpret_cast<uint64_t *>(entry.cn_array_[0].cn_) == a + tid + n_threads*i) {
            // // if (reinterpret_cast<uint64_t *>(entry.cn_array_[0].cn_) == a + tid) {
            //     std::cerr << "pointer value matches " << entry.cn_array_[0].cn_ << " " << a;
            //     exit(1);
            // }

            std::cerr << "initial checkput error " << res << std::endl;
            if (res == 2) {
                GCDEntry entry = Get(metadata, blkkey);
                print_key(&blkkey);
                print_entry(&entry);
            }
            exit(1);
        }

        res = CheckPutArray(metadata, blkkey, reinterpret_cast<size_t>(a + tid + n_threads*i), 0, key + 1);
        if (res != 2) {
            std::cerr << "update wmeta fail " << res << std::endl;
            exit(1);
        }

        res = CheckPutArray(metadata, blkkey, reinterpret_cast<size_t>(a + tid + n_threads*i), 0, key + 2);
        if (res != 1) {
            std::cerr << "fail report error " << res << std::endl;
            if (res == 2) {
                GCDEntry entry = Get(metadata, blkkey);
                print_key(&blkkey);
                print_entry(&entry);
            }
            exit(1);
        }

        res = CheckPutArray(metadata, blkkey, reinterpret_cast<size_t>(a + tid + n_threads*i), 1, -1);
        // if (res != 1) {
        //     std::cerr << "fail report error " << res << std::endl;
        //     if (res == 2) {
        //         GCDEntry entry = Get(metadata, blkkey);
        //         print_key(&blkkey);
        //         print_entry(&entry);
        //     }
        //     exit(1);
        // }

        res = CheckSwitchWmetaArray(metadata, blkkey);
        if (!res) {
            std::cerr << "fail switch " << std::endl;
            GCDEntry entry = Get(metadata, blkkey);
            print_key(&blkkey);
            print_entry(&entry);
            exit(1);
        }

        res = CheckPutArray(metadata, blkkey, reinterpret_cast<size_t>(a + tid + n_threads*i), 1, -1);
        if (res != 0) {
            std::cerr << "local replicate fail " << res << std::endl;
            if (res == 2) {
                GCDEntry entry = Get(metadata, blkkey);
                print_key(&blkkey);
                print_entry(&entry);
            }
            exit(1);
        }
    }

    sleep(5);
#if 0
    std::cout << "start checkputswap " << std::endl;
    for (int i = 0; i < NOP/2; ++i) {
        if (i % LOG_PERIOD == 0) std::cout << "finished " << i << std::endl;
        uint64_t key = dist(gen);
        // uint64_t value = dist(gen);
        GCDEntry entry = Get(metadata, BlockId(key));
        if (is_entry_empty(&entry)) continue;
        if (reinterpret_cast<uint64_t *>(entry.cn_array_[0].cn_) == a) {
            CheckPutSwapArray(metadata, BlockId(key), reinterpret_cast<void *>(a), 1, 0);
        } else if (reinterpret_cast<uint64_t *>(entry.cn_array_[1].cn_) != a) {
            std::cerr << "pointer value does not match " << a << std::endl;
            print_entry(&entry);
            exit(1);
        }    
    }

    sleep(5);
    std::cout << "start checkput " << std::endl;

    for (int i = 0; i < NOP/2; ++i) {
        if (i % LOG_PERIOD == 0) std::cout << "finished " << i << std::endl;
        uint64_t key = dist(gen);
        uint64_t *b = a+1;
        // uint64_t value = dist(gen);
        GCDEntry entry = Get(metadata, BlockId(key));
        if (is_entry_empty(&entry)) continue;
        if (reinterpret_cast<uint64_t *>(entry.cn_array_[0].cn_) == a) {
            if (reinterpret_cast<uint64_t *>(entry.cn_array_[1].cn_) == a) {
                std::cerr << "double existence" << std::endl;
                print_entry(&entry);
                exit(1);
            }
            CheckPutArray(metadata, BlockId(key), reinterpret_cast<void *>(b), 0);
        } else if (reinterpret_cast<uint64_t *>(entry.cn_array_[1].cn_) != a) {
            std::cerr << "exist elsewhere " << std::endl;
            print_entry(&entry);
            exit(1);
        }    
    }

    sleep(5);
    std::cout << "start get " << std::endl;

    for (int i = 0; i < NOP/2; ++i) {
        if (i % LOG_PERIOD == 0) std::cout << "finished " << i << std::endl;
        uint64_t key = dist(gen);
        // uint64_t value = dist(gen);
        GCDEntry entry = Get(metadata, BlockId(key));
        if (is_entry_empty(&entry)) continue;
        if (reinterpret_cast<uint64_t *>(entry.cn_array_[0].cn_) == a) {
            if (reinterpret_cast<uint64_t *>(entry.cn_array_[1].cn_) == a) {
                std::cerr << "double existence" << std::endl;
                print_entry(&entry);
                exit(1);
            }
        } else if (reinterpret_cast<uint64_t *>(entry.cn_array_[1].cn_) != a) {
            std::cerr << "exist elsewhere " << std::endl;
            print_entry(&entry);
            exit(1);
        }    
    }
#endif // 0
    unregister_node_replicated(metadata);
}

int main() {
    // NrRapper* nrht_wrapper = create_node_replicated_numa_async(KEY_SPACE, BGROUND_SYNC_US);
    NrRapper* nrht_wrapper = create_node_replicated_numa(KEY_SPACE, 3); // 3 replicas

    uint64_t a = 0;
    std::vector<std::thread> threads;

    for (int i = 0; i < 12; ++i) {
        // rid set to 0, 1, 2
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
