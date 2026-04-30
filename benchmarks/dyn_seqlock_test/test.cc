#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

#include "seqcount.h"

// Simulate work
void simulate_work(int ms = 1) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

void writer_thread(seqlock_t* lock, int id) {
    for (int i = 0; i < 5; ++i) {
        if (!write_seqlock_begin(lock)) {
            std::cout << "[Writer " << id << "] lock is freed !!!\n";
            break;
        }
        std::cout << "[Writer " << id << "] acquired lock\n";

        simulate_work(3);  // simulate write

        unsigned int new_seq = write_seqlock_end(lock);
        std::cout << "[Writer " << id << "] released lock, new seq: " << new_seq << "\n";

        simulate_work(10);
    }
}

void reader_thread(seqlock_t* lock, int id) {
    for (int i = 0; i < 20; ++i) {
        unsigned int seq;

        seq = read_seqlock_begin(lock);
        simulate_work(1);  // simulate read
        if (read_seqlock_retry(lock, seq)) {
            std::cout << "[Reader " << id << "] read invalid\n";
        } else {
            std::cout << "[Reader " << id << "] read consistent data (seq=" << seq << ")\n";
            simulate_work(1);
        }
    }
}

void allocator_thread(seqlock_t* lock) {
    simulate_work(20);  // let readers/writers spin
    std::cout << "[Allocator] Trying to allocate lock (with_lock=true)...\n";

    if (try_allocate_seqlock(lock, true)) {
        std::cout << "[Allocator] Allocated lock with_lock=true\n";
    }

    simulate_work(2);

    write_seqlock_end(lock);

    simulate_work(200);

    std::cout << "[Allocator] Trying to free lock...\n";
    free_seqlock(lock);
    std::cout << "[Allocator] Freed lock\n";
}

int main() {
    seqlock_t lock;
    seqlock_init(&lock);

    // First allocate with lock held
    bool ok = try_allocate_seqlock(&lock, true);
    assert(ok);
    std::cout << "[Main] Lock allocated with_lock=true\n";

    std::thread writer1(writer_thread, &lock, 1);
    std::thread writer2(writer_thread, &lock, 2);
    std::thread reader1(reader_thread, &lock, 1);
    std::thread reader2(reader_thread, &lock, 2);
    std::thread allocator(allocator_thread, &lock);

    writer1.join();
    writer2.join();
    reader1.join();
    reader2.join();
    allocator.join();

    std::cout << "[Main] Test complete. Final sequence: " << (lock.sequence.load() & SEQ_MASK) << "\n";
    return 0;
}