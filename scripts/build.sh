#!/usr/bin/env bash

cd $(dirname "$0")/..
PROJECT_ROOT="$(pwd)"

rm -rf build
cmake -B build -S . -DCMAKE_POLICY_VERSION_MINIMUM=3.5  -DCMAKE_BUILD_TYPE=release -DCMAKE_TOOLCHAIN_FILE=cmake-variant/toolchains/clang17-gcc.cmake
# cmake -B build -S . -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_TOOLCHAIN_FILE=cmake-variant/toolchains/clang17-gcc.cmake
cd build; make -j
cd ..
