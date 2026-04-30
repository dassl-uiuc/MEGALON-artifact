#pragma once

#include "absl/log/log.h"
#include "common/types.h"
#include "globals.h"

namespace rackobj::lib {
class RackOBJKV {
public:
    static ssize_t Get(void* buf, uint64_t count, off_t offset);

    static ssize_t Put(const void* buf, uint64_t count, off_t offset);

    static ssize_t Preload(const void* buf, uint64_t count, off_t offset);
};

}  // namespace rackobj::lib