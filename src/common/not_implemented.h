#pragma once
#include <type_traits>

namespace rackobj {

// See for more details on the motivation for this struct:
// https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2022/p2593r0.html#valid-workaround
template <typename T>
struct not_implemented_t : std::false_type {};

}  // namespace rackobj