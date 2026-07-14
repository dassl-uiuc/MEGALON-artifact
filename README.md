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
