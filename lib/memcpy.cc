#include "memcpy.h"

#include <x86intrin.h>

#include <cstring>
#include <queue>

#include "absl/log/log.h"

namespace rackobj::lib {

static constexpr size_t max_latency = 10000;
static constexpr size_t max_sample_size = 10000;
static std::queue<uint64_t> last_n_ts;
static uint64_t ts_sum = 0;

void *slow_memcpy(void *__restrict__ __dest, const void *__restrict__ __src, size_t __n) noexcept(true) {
    uint64_t t1, t2, t3, wait_time;
    uint32_t tsc_aux;
    void *ret;

    t1 = __rdtscp(&tsc_aux);
    ret = memcpy(__dest, __src, __n);
    t2 = __rdtscp(&tsc_aux);

    if (last_n_ts.size() == max_sample_size) {
        ts_sum -= last_n_ts.front();
        last_n_ts.pop();
    }
    last_n_ts.push(std::min(t2 - t1, max_latency));
    ts_sum += last_n_ts.back();

    wait_time = (ts_sum / last_n_ts.size()) / 4;
    t3 = __rdtscp(&tsc_aux);
    while (t3 - t2 < wait_time) {
        t3 = __rdtscp(&tsc_aux);
    }

    DLOG(INFO) << "memcpy took " << (t2 - t1) << " cycles, target extra " << wait_time << " cycles, actual extra "
               << (t3 - t2) << " cycles";
    return ret;
}

}  // namespace rackobj::lib