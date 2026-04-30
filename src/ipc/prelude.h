#pragma once
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc11-extensions"
#pragma clang diagnostic ignored "-Wimplicit-int-conversion"
#include "hostrpc/base_types.hpp"
#pragma clang diagnostic pop

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wsign-conversion"
#pragma clang diagnostic ignored "-Wc11-extensions"
#pragma clang diagnostic ignored "-Wshorten-64-to-32"
#include "hostrpc/detail/client_impl.hpp"
#pragma clang diagnostic pop

#include "hostrpc/detail/server_impl.hpp"

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc11-extensions"
#include "hostrpc/memory.hpp"
#pragma clang diagnostic pop

#include "hostrpc/platform/host.hpp"