# MEGALON: Efficient Data Sharing for Partly Coherent CXL Memory

This branch contains the `HCMeta` variant of the artifact for:
MEGALON: Efficient Data Sharing for Partly Coherent CXL Memory

## Getting Started

### Prerequisites

- Ubuntu with sudo access (tested on Ubuntu 22.04 with kernel version `5.15.0-131`)
- `git`, `python3`, and `pip` available in `PATH`
- At least two NUMA nodes, with one node used to simulate CXL memory
- Intel CPU support for `clflushopt` and `clwb`
- Intel uncore frequency driver support. The setup script loads `intel_uncore_frequency`; this artifact has not been tested on machines from other vendors.

### Node Setup

1. Initialize submodules and install system dependencies:

```bash
./scripts/setup.sh
```

The setup script installs dependencies, configures clang 17, loads the Intel uncore frequency module, sets `RACKOBJ_RESULT_DIR` in `~/.bashrc`, and runs a default logical-node setup.

2. Re-source your shell, or open a new one, to pick up compiler and result-directory exports:

```bash
source ~/.bashrc
```

3. Configure logical nodes before compilation when changing the CXL-memory NUMA node, number of logical nodes, or key size:

```bash
./scripts/setup_logical_node.sh <NUMA_MEM> <LOGICAL_NODE_NUM> <KEY_SIZE>
```

Arguments:

- `NUMA_MEM`: NUMA node id designated for CXL memory simulation; this node is slowed down during experiments.
- `LOGICAL_NODE_NUM`: number of logical nodes to simulate; multiple logical nodes can colocate on the same physical NUMA node.
- `KEY_SIZE`: key size in bytes for HCMeta.

4. For Experiment 6.12, the page-cache application expects a backing file. Create it at `/mydata/KV_STORE`:

```bash
dd bs=4096 count=262144 if=/dev/random of=/mydata/KV_STORE
```

### Build

`./scripts/build.sh` is the all-in-one build script. It performs these steps:

1. Removes the existing `build/` directory.
2. Configures CMake with the clang 17 toolchain and Release mode.
3. Builds with `make -j` in `build/`.

```bash
./scripts/build.sh
```

For a focused rebuild after configuration, use:

```bash
cmake --build build -j --target hcmeta
```

Benchmark targets are defined in `benchmarks/CMakeLists.txt`, including `kv-store` and `ycsb_benchmark`. The `page-cache` target is built only when `FILE_INTERFACE` is enabled.

### Runtime Configuration

Runtime configuration is selected with `RACKOBJ_CONFIG`; if it is unset, HCMeta falls back to `/opt/rackobj/config`.

Example configuration files are under `config/`. Evaluation scripts set `RACKOBJ_CONFIG` automatically, for example to `config/a.yaml`.

### Sample Run

Use the sample evaluation script. It rebuilds the `hcmeta` variant, runs a zipfian workload benchmark, then analyzes throughput results.

```bash
./scripts/eval/sample.sh
```

Outputs:

- Logs: `logs/sample/hcmeta/`, including build output, benchmark output, and `stats.log` analysis.
- Results: `${RACKOBJ_RESULT_DIR}sample/hcmeta/`, with per-run output directories.

## Reproducing Experiments

The table below maps each paper section to its experiment script(s) under `scripts/eval/`. The `*-datamove.sh` scripts run the HCMeta-local variant in the paper.

| § | Section | Script(s) | Notes |
|---|---------|-----------|-------|
| 6.1 | Performance under Read-Only Workload | `eval1.sh`, `eval1-datamove.sh` | `eval1-datamove.sh` runs the HCMeta-local variant |
| 6.2 | Read-only Performance with Growing Dataset Sizes | `eval2.sh`, `eval2-datamove.sh` | `eval2-datamove.sh` runs the HCMeta-local variant |
| 6.3 | Performance under Read-Write Workloads | `eval3.sh` | |
| 6.4 | Performance when Coherence Records Do Not Fit | `eval4.sh` | |
| 6.5 | Performance vs. Object Key Size | `eval5.sh` | |
| 6.6 | Local-DRAM Usage and Performance | *(none)* | |
| 6.7 | Performance with Smaller Coherence Records | *(none)* | Megalon-specific section |
| 6.8 | Performance for Partitioned Workloads | `eval8.sh` | |
| 6.9 | Benefit of Replicating Objects Locally | *(none)* | Megalon-specific comparison |
| 6.10 | Performance of All-Log Variant | *(none)* | |
| 6.11 | YCSB Macrobenchmark | `eval11.sh` | |
| 6.12 | Page Cache Application | `eval12.sh` | |

### Interpreting Results

Throughput numbers can be found in experiment runs under the corresponding log directory, for example `logs/sample/`, `logs/eval8/`, or `logs/eval11/`.

Runs with the same configuration except for thread count should appear in the same log file.

Raw result files are written under `RESULT_ROOT=${RACKOBJ_RESULT_DIR}eval#` by the evaluation scripts, for example `${RACKOBJ_RESULT_DIR}sample/` or `${RACKOBJ_RESULT_DIR}eval8/`. To compute statistics from the raw files, refer to `scripts/eval/sample.sh`, which calls `benchmarks/script/analysis.py` on each result directory.

## Main MEGALON Variant

Check out the `main` branch for the main MEGALON artifact. This branch follows a similar hierarchy and keeps the HCMeta-specific CMake and evaluation configuration.
