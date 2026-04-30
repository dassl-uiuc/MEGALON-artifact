# AGENTS

## Scope and source of truth
- Treat root `CMakeLists.txt`, `benchmarks/CMakeLists.txt`, and `scripts/*.sh` as authoritative; `README.md` is useful but partially stale.
- This repo includes many vendored trees under `third-party/`; avoid searching/editing there unless the task is explicitly about dependencies.

## Build + setup that agents often miss
- First-time setup is `./scripts/setup.sh` (not `scripts/setup`): it initializes submodules, applies `third-party/hostrpc.diff`, installs packages with `sudo`, edits `~/.bashrc`, runs NR Rust dependency install, and runs logical-node setup.
- `scripts/setup_logical_node.sh <NUMA_MEM> <LOGICAL_NODE_NUM> <KEY_SIZE>` mutates checked-in files in place (`src/common/constants.h`, `third-party/nr_rust/ffi/constants.rs`, `benchmarks/common.h`). Re-run it intentionally when changing NUMA/logical-node/key-size assumptions.
- NR static libs must exist before CMake link succeeds: run `make libs` in `third-party/nr_rust/cpp` to produce `third-party/nr_rust/ffi/target/release/libnr_hashmap*.a`.

## Canonical commands
- Full rebuild path used by repo: `./scripts//build.sh` (rebuilds NR libs, deletes `build/`, configures CMake with `cmake-variant/toolchains/clang17-gcc.cmake`, then `make -j`).
- Manual configure/build equivalent:
  - `cmake -B build -S . -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=cmake-variant/toolchains/clang17-gcc.cmake`
  - `cmake --build build -j`
- Focused build: `cmake --build build -j --target <target>` (library target: `rackobj`; benchmark targets from `benchmarks/CMakeLists.txt` like `kv_store`, `rack_btree`, `trace`, `load`).

## Non-obvious build behavior
- `rackobj` has `add_dependencies(rackobj fmt)`: building the library triggers the `fmt` target, which runs `clang-format -i` across `lib/`, `src/`, and `benchmarks/`. Expect formatting diffs after builds.
- Build hard-requires CPU/compiler support for `-mclflushopt` (`CMakeLists.txt` fails otherwise).
- Root CMake forces compile mode defaults (`SCR=ON`, `LIMITED_SCR=ON`, `NO_COHERENCE=OFF`, `FULL_COHERENCE=OFF`), and links a different NR library based on those flags.

## Runtime/config facts
- Runtime config path comes from `RACKOBJ_CONFIG`; fallback is `/opt/rackobj/config` (`src/common/config.cc`). If neither exists, initialization aborts.
- Example configs live in `config/*.yaml` and include required keys (`id`, `base_address`, `host_address`, `slots`, `key_space`, etc.).
- NUMA pinning helper: `./scripts/numabind <node> <command...>` wraps `numactl --membind --cpunodebind`.

## Architecture map (minimal)
- Public API surface: `include/rackobj.h`.
- Syscall interposition/wrappers: `lib/glibc/*.cc` plus `lib/original_syscalls.*`.
- Core implementation is in `src/{common,core,index,manager,shared_memory}` and linked as shared library `rackobj`.
- Bench/execution entrypoints are standalone binaries in `benchmarks/` (declared in `benchmarks/CMakeLists.txt`).

## Validation guidance
- There is no first-party test suite/CI config in this repo root; validate by compiling relevant targets and, for behavior changes, running the specific benchmark binary you touched.
