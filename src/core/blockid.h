#pragma once

#include "common/constants.h"

#if KEY_SIZE == 24
#include "blockid_file.h"
#else
#include "blockid_variable.h"
#endif