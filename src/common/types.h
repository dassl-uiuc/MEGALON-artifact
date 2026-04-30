#pragma once
#include <sys/types.h>

#include <string>
#include <utility>

namespace rackobj {

typedef int PosixError;

namespace common {
enum NrGcdError {
    GCD_NO_ERROR = 0,
    GCD_SLOT_WMETA_UPDATE_FAILED,
    GCD_SLOT_UPDATE_FAILED,
    GCD_ENTRY_NOEXIST,
};

enum NrGcdDeleteError {
    NR_GCD_DELETE_SUCCESS = 0,
    NR_GCD_DELETE_WMETA_VALID,    // wmeta valid
    NR_GCD_DELETE_ENTRY_NOEXIST,  // entry does not exist
    NR_GCD_DELETE_ALRDY_DELETED,  // the replica on idx is already deleted (not used in current implementation)
};
}  // namespace common

}  // namespace rackobj