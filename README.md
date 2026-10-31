# MEGALON: Efficient Data Sharing for Partly Coherent CXL Memory

This repo is the artifact for:
> _MEGALON: Efficient Data Sharing for Partly Coherent CXL Memory_,
> Jiyu Hu, Seokjoo Cho, Landon Johnson, Kiran Hombal, Shreesha G. Bhat, Marcos K. Aguilera, Ramnatthan Alagappan, Aishwarya Ganesan,
> 20th USENIX Symposium on Operating Systems Design and Implementation, OSDI '26

To reproduce the experiments corresponding to the accepted version of the paper, switch to tag `artifact-eval-v1`.

For extra experiments in the final paper, please refer to tag `cr-v2`.

## Getting Started

### Prerequisites
- Ubuntu with sudo access (tested on Ubuntu 22.04 with kernel version `5.15.0-131`)
- git, python3, pip available in PATH
- At least two NUMA nodes (one used to simulate CXL memory)
- Intel CPU (setup loads `intel_uncore_frequency` kernel module). The artifact is not tested on machines from other vendors.
- The CMake toolchain hard-codes the GCC 11 install directory (`/usr/lib/gcc/x86_64-linux-gnu/11`). On distros newer than Ubuntu 22.04, `g++-11` may not be installed by default and must be present before configuring the project.

### Node Setup
1. Initialize submodules and install system dependencies:
    ```bash
    ./scripts/setup.sh
    ```
    On newer Ubuntu releases and other newer distros, `scripts/setup.sh` installs `g++-11` automatically when it is missing so the hard-coded toolchain path continues to resolve.

2. For `NR` libraries (included in `scripts/setup.sh`):\
First install the dependencies (note that the script installs Rust nightly version, the installation script might show option prompts. Just select default by hitting 'enter')
    ```bash
    ./third-party/nr_rust/install_deps.sh
    ```
3. Re-source your shell (or open a new one) to pick up compiler and result-dir exports:
    ```bash
    source ~/.bashrc
    ```
4. For Experiment 6.12 (page-cache application), create the backing file expected at `/mydata/KV_STORE`:
    ```bash
    dd bs=4096 count=262144 if=/dev/random of=/mydata/KV_STORE
    ```

### Build
`./scripts/build.sh` is the all-in-one build script. It performs these steps:
1. Builds NR static libraries in `third-party/nr_rust/cpp`.
2. Removes the existing `build/` directory.
3. Configures CMake with the clang17 toolchain and Release mode.
4. Builds with `make -j` in `build/`.
5. MEGALON configures abstract nodes on top of the physical NUMA nodes on the machine. To set that up properly, please run `scripts/setup_logical_node.sh` before compilation, with the target `NUMA_NODE` as the cxl memory node (where no threads will run on), and `LOGICAL_NODE_NUM` for number of logical nodes.
Arguments:
- `NUMA_MEM`: NUMA node id designated for CXL memory simulation; this node will be slowed down during experiments.
- `LOGICAL_NODE_NUM`: number of logical nodes to simulate; multiple logical nodes can colocate on the same physical NUMA node.
- `KEY_SIZE`: key size in bytes for MEGALON.

### Sample Run
Use the sample evaluation script (~5 minutes). It rebuilds the megalon variant, runs a zipfian workload benchmark, then analyzes results with `avg_lat.py`.
```
./scripts/eval/sample.sh
```
Outputs:
- Logs: `logs/sample/megalon/` (build errors, benchmark output including the throughput, and `stat.log` analysis including the latency)
- Results: `${RACKOBJ_RESULT_DIR}sample/megalon/` (per-run output directories)

## Reproducing Experiments

The table below maps each paper section to its experiment script under `scripts/eval/`.

| § | Section | Script | Notes |
|---|---------|--------|-------|
| 6.1 | Performance under Read-Only Workload | `eval1.sh` | |
| 6.2 | Read-only Performance with Growing Dataset Sizes | `eval2.sh` | |
| 6.3 | Performance under Read-Write Workloads | `eval3.sh` | |
| 6.4 | Performance when Coherence Records Do Not Fit | `eval4.sh` | |
| 6.5 | Performance vs. Object Key Size | `eval5.sh` | |
| 6.6 | Local-DRAM Usage and Performance | *(none)* | Reuses data from `eval2.sh` |
| 6.7 | Performance with Smaller Coherence Records | `eval7.sh` | |
| 6.8 | Performance for Partitioned Workloads | `eval8.sh` | |
| 6.9 | Benefit of Replicating Objects Locally | `eval9.sh` | |
| 6.10 | Performance of All-Log Variant | *(none)* | Reuses data from `eval2.sh` and `eval3.sh` |
| 6.11 | YCSB Macrobenchmark | `eval11.sh` | |
| 6.12 | Page Cache Application | `eval12.sh` | |

### Interpreting Results
Throughput numbers can be found in experiment runs under the corresponding log directory (for example: `logs/sample/`, `logs/eval7/`, `logs/eval8/`).

Note: runs with the same configuration except for thread count should appear in the same log file.

Raw latency files are written under `RESULT_ROOT=${RACKOBJ_RESULT_DIR}eval#` by the evaluation scripts (for example: `${RACKOBJ_RESULT_DIR}sample/`, `${RACKOBJ_RESULT_DIR}eval7/`). To compute latency statistics from the raw files, refer to `scripts/eval/sample.sh`, which calls `benchmarks/script/avg_lat.py` on each result directory.

### HCMeta variant
Checkout to `hcmeta` branch for running experiments corresponding to the `HCMeta` variants. The branch follows a similar hierarchy as the `main` branch.

Checkout to `alllog` branch for running experiments corresponding to the `AllLog` variants for experiment 6.10.

## Coherence Patch

The `main`, `hcmeta` and `alllog` branches each carry one commit, "Fix coherence bugs in shared CXL state", that fixes implementation bugs in how shared state is kept coherent across hosts in the non-coherent CXL region. Its parent is the code used for the paper. The paper's trends and claims are unaffected (see below).

All three branches:
- **Shared object header in SCR.** The header holding the bitmap pointers and the shared slot counters is now allocated in the SCR instead of the NCR. Each counter sits on its own cache line, and the dirty counter is updated only when the dirty bit actually flips.
- **One cache line per `CacheNode`.** Shared `CacheNode`s are padded to 64 B so neighbours never share a line, and the per-access eviction hint lives in a separate array instead of the node.
- **Retry after `META_OUTDATE` invalidates the page**, as the `PAGE_INCOHERENT` retry already did.
- **Acquire fence before the seqlock re-check.**
- **`cache_flush` covers every line of an unaligned range**, including the last one.
- **File-backed admission writes the fetched data back**, and the evictor invalidates a `CacheNode` before reading its block id.

MEGALON and AllLog (`main`, `alllog`):
- **Shared log entries are fully written back before being published.** An appender writes back its entries (several cache lines each), then publishes them in log order by advancing a committed tail kept in the SCR next to the log tail. Replicas replay only up to the committed tail, which they read without flushing, and invalidate the published entries with a single fence per batch.

AllLog only (`alllog`):
- Links the `scr` NR library, which has the log write-backs and replay invalidations; it previously linked the original NR, which had none.
- Adds the acquire fence to the GCD seqlock, and declares the missing `ReplicateManager` constructor argument so the branch builds.

HCMeta only (`hcmeta`):
- In HCMeta-local (move-to-share) mode, the CXL slot arrays are sized to cover every coherence-record index, which could previously exceed the number of slots.
- A reused coherence record continues its sequence number instead of restarting at 0, so stale per-host copies can't match it.
- The shared copy is invalidated before it is copied back to its partition.

### Re-measuring before and after the patch

```bash
./scripts/eval/patch_compare.sh -e eval2   # §6.2, Figure 5:      MEGALON, HCMeta, HCMeta-local (~3 h)
./scripts/eval/patch_compare.sh -e eval3   # §6.3, Figure 6:      MEGALON, HCMeta (~4 h)
./scripts/eval/patch_compare.sh -e eval4   # §6.4, Figure 7(b)(ii): MEGALON, HCMeta (~2 h)
```

The script runs the experiment's own evaluation script on the original commits (`3be2fa7` on `main`, `7566c76` on `hcmeta`) and on the patched branches (`main`, `hcmeta`). It writes `logs/patch-compare/<eval>/results.csv` and `logs/patch-compare/<eval>/<eval>_before_after.png`, a before/after plot in the style of the paper's figure. Use `-s` and `-p` to run a subset, and `-c` to re-collect and re-plot existing results without running. HCMeta needs about 50 GB free on NUMA node 0 and 40 GB on each other node (`numactl -H`).

On our machine, MEGALON's throughput was unchanged or higher in every configuration we measured (eval 2: +0.4% to +3.5%; eval 3: +1% to +9%; eval 4: +11% to +26%), and HCMeta still collapses once its metadata no longer fits in the SCR.
