# rackobj

## Binding to a NUMA Node

```bash
numactl --membind=0 --cpunodebind=0 -- <command> <args...>
```

Alternatively...

```bash
./scripts/numabind 0 <command> <args...>
```

## Build Prerequisites
2. Install required modules

```
./scripts/setup
```

3. For `NR` libraries:\
First install the dependencies (note that the script installs Rust nightly version, the installation script might show option prompts. Just select default by hitting 'enter')
```
./third-party/nr_rust/install_deps.sh
```

Also, `libnr_hashmap.a` and `libnr_hashmap_orig.a` need to be present at `third-party/nr_rust/ffi/target/release`. To build the target libraries:
```
cd third-party/nr_rust/cpp
make libs
```

4. the build requires `clflushopt`. Please contact the author if your hardware does not support this

## For Developers
1. To support custom `gcd` implemetation, follow how `gcd_nr.h` and `gcd.h` are used in the codebase. The key to `gcd`:
```
class BlockId {
    uint64_t server_id_;
    ino_t inode_;
    off_t offset_;
};
```
Should be only hashed on `inode_` and `offset_`. (i.e. `BlockId`s with different `server_id_`s should be hashed to the same key)

## Unoptimized Debug Build

```bash
export CXX=clang++-17
export CC=clang-17
export LD=clang++-17
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Debug ..
make -j
```

## Optimized Release Build

```bash
export CXX=clang++-17
export CC=clang-17
export LD=clang++-17
mkdir build-release && cd build-release
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j
```

## Debugging Build Errors

If your build fails with a `cmake` error, reset the `cmake` cache and rebuild:
```bash
cd build
rm CMakeCache.txt
export CXX=clang++-17
export CC=clang-17
export LD=clang++-17
cmake ..
make -j
```

## VSCode Include Paths

```
${workspaceFolder}/**
${workspaceFolder}/common/
${workspaceFolder}/include/
${workspaceFolder}/lib/
${workspaceFolder}/server/
${workspaceFolder}/third-party/abseil-cpp/
${workspaceFolder}/third-party/hostrpc/
${workspaceFolder}/third-party/robin-map/include/
${workspaceFolder}/third-party/unordered_dense/include/
${workspaceFolder}/build/protos/
${workspaceFolder}/build/_deps/yaml-cpp-src/include/
```

## Misc

```bash
dd bs=4096 count=262144 if=/dev/random of=<some path>
```

## Updates
1. Updating to multiple logical node (abstraction from physical numa nodes).