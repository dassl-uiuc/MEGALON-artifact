#pragma once

#include <cstdint>
#include <unistd.h>
#include <string>

namespace rackobj {

void EnableRemoteMemcpySlowdown(bool enable);

uint64_t GetPageCacheMissCount();

uint64_t GetPageCacheOperationCount();

uint64_t GetReadCount();

uint64_t GetReadRetryCount();

uint64_t GetReadRetryInvoc();

uint64_t GetAdmitCount();

uint64_t GetTigonLocalAccess();

uint64_t GetTigonRemoteAccess();

uint64_t GetTigonRemoteAccessMiss();

uint64_t GetTigonRemoteAccessHit();
uint64_t GetTigonLocalAccessMiss();
uint64_t GetTigonLocalAccessHit();

/* kv interface */
ssize_t Put(const uint8_t* buf, uint64_t count, off_t key);

ssize_t Get(uint8_t* buf, uint64_t count, off_t key);

ssize_t Preload(uint8_t* buf, uint64_t count, off_t key);

void Register(int tid);

void UnRegister();

std::string GetConfigStr(std::string c_name);

size_t GetConfigSize_t(std::string c_name);

// internal use
void SetWmetaWaterMark(size_t wmeta_water_mark);

void StartReplMgr();

void ClearMovementCounter();

}  // namespace rackobj