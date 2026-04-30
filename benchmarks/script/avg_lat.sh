#!/usr/bin/env bash

# RESULT_DIR=results/limits/hotcachepartial-local/0.000000
# RESULT_DIR=${RACKOBJ_RESULT_DIR}limits/hotcache-local-0.0
# RESULT_DIR=${RACKOBJ_RESULT_DIR}limits/hotcache-local-0.1
# RESULT_DIR=${RACKOBJ_RESULT_DIR}limits/hotcache-local-0.01
# RESULT_DIR=${RACKOBJ_RESULT_DIR}limits/zipfian-local-0.0
# RESULT_DIR=${RACKOBJ_RESULT_DIR}limits/zipfian-local-0.1
RESULT_DIR=${RACKOBJ_RESULT_DIR}limits/zipfian-local-0.05
# RESULT_DIR=${RACKOBJ_RESULT_DIR}limits/zipfian-local-0.01
# RESULT_DIR=${RACKOBJ_RESULT_DIR}limits/hotspot-local-local-0.1
# RESULT_DIR=/mydata/jiyu/rackobj/benchmarks/results/limits/c3po-hotcache-local-0.0/

# RESULT_DIR=/mydata/jiyu/thread_sub_reply_flush_0/hotcache-local-0.0

workload="r"

while getopts "w:d:" opt; do
  case $opt in
    w)
      workload="$OPTARG"
      ;;
    d)
      RESULT_DIR="$OPTARG"
      ;;
    \?)
      echo "Usage: $0 [-w workload] [-d result_dir]"
      exit 1
      ;;
  esac
done

echo "RESULT_DIR is ${RESULT_DIR}"
echo "analyzing workload: ${workload}"

echo "avg(ns),std,p50,p99,p99.9"
# for i in 1 3 6 9 12; do
# for i in 1 3 6 9 12 18 24 35 48 60; do
for i in {40..40}; do
    python3 avg_lat.py ${RESULT_DIR}/$i $workload
done
