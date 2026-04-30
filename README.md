# MEGALON: Efficient Data Sharing for Partly Coherent CXL Memory

This repo is the artifact for:\
MEGALON: Efficient Data Sharing for Partly Coherent CXL Memory

## Getting Started

### Prerequisites
- Ubuntu with sudo access (tested on Ubuntu 22.04 with kernel version `5.15.0-131`)
- git, python3, pip available in PATH
- At least two NUMA nodes (one used to simulate CXL memory)
- Intel CPU (setup loads `intel_uncore_frequency` kernel module). The artifact is not tested on machines from other vendors.

### Node Setup
1. Initialize submodules and install system dependencies:
```
./scripts/setup.sh
```
2. For `NR` libraries (included in `scripts/setup.sh`):\
First install the dependencies (note that the script installs Rust nightly version, the installation script might show option prompts. Just select default by hitting 'enter')
```
./third-party/nr_rust/install_deps.sh
```
3. Re-source your shell (or open a new one) to pick up compiler and result-dir exports:
```
source ~/.bashrc
```
4. For Experiment 10 (page-cache application), create the backing file expected at `/mydata/KV_STORE`:
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
Refer to the experiment scripts under `scripts/eval/` (for example: `eval1.sh`, `eval2.sh`, ..., `eval10.sh`). Use the script corresponding to the paper section you want to reproduce.

Note: there is no script for Section 6.8 because it does not run any experiments. Figure 8 is not included int the experiment suite as it is collected through manual `perf` run.

### Interpreting Results
Throughput numbers can be found in experiment runs under the corresponding log directory (for example: `logs/sample/`, `logs/eval6/`, `logs/eval7/`).

Note: runs with the same configuration except for thread count should appear in the same log file.

Raw latency files are written under `RESULT_ROOT=${RACKOBJ_RESULT_DIR}eval#` by the evaluation scripts (for example: `${RACKOBJ_RESULT_DIR}sample/`, `${RACKOBJ_RESULT_DIR}eval6/`). To compute latency statistics from the raw files, refer to `scripts/eval/sample.sh`, which calls `benchmarks/script/avg_lat.py` on each result directory.

### HCMeta variant
Checkout to `hcmeta` branch for running experiments corresponding to the `HCMeta` variants. The branch follows a similar hierarchy as the `main` branch.
