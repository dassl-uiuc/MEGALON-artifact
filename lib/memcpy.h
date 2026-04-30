#pragma once

#include <cstddef>

namespace rackobj::lib {

void *slow_memcpy(void *__restrict__ __dest, const void *__restrict__ __src, size_t __n) noexcept(true);

}