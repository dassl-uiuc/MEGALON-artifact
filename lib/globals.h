#pragma once

#include "common/config.h"
#include "common/thread_local.h"
#include "file.h"
#include "kv.h"
#include "shm_obj_handle.h"

namespace rackobj::lib {

using CurrPolicy = common::LruPolicy;

extern const RackOBJConfig client_cfg;

extern SharedMemoryObjectHandle<CurrPolicy> page_cache;

bool ShouldSlowdownRemoteMemoryMemcpy();

extern thread_local ThreadLocalMeta<CurrPolicy> thread_local_meta;

}  // namespace rackobj::lib

bool AreGlobalsInitialized();

std::shared_ptr<rackobj::lib::RackOBJFile> FindFilePointer(int fd);

bool DoesFilePointerExist(rackobj::lib::RackOBJFile* fp);

void InsertFilePointer(std::shared_ptr<rackobj::lib::RackOBJFile> fp);

void EraseFilePointer(rackobj::lib::RackOBJFile* fp);

void RegisterLocalMem();