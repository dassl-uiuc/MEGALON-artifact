CUR_DIR="$(dirname "$0")"
cd $CUR_DIR/..
cd third-party/nr_rust/cpp/; make libs
cd ../../../; rm -rf build
cmake -B build -S . -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_BUILD_TYPE=release -DCMAKE_TOOLCHAIN_FILE=cmake-variant/toolchains/clang17-gcc.cmake
cd build; make -j
cd ..
