# Force compilers
set(CMAKE_C_COMPILER clang-17)
set(CMAKE_CXX_COMPILER clang++-17)

# Tell Clang to use GCC 11 toolchain
set(GCC_TOOLCHAIN_ROOT "/usr")
set(GCC_INSTALL_DIR "/usr/lib/gcc/x86_64-linux-gnu/11")

# Compile flags
set(CMAKE_C_FLAGS_INIT
    "--gcc-install-dir=${GCC_INSTALL_DIR}"
)

set(CMAKE_CXX_FLAGS_INIT
    "--gcc-install-dir=${GCC_INSTALL_DIR}"
)

# Linker flags (CRITICAL)
set(CMAKE_EXE_LINKER_FLAGS_INIT
    "--gcc-toolchain=${GCC_TOOLCHAIN_ROOT} --gcc-install-dir=${GCC_INSTALL_DIR}"
)

set(CMAKE_SHARED_LINKER_FLAGS_INIT
    "--gcc-toolchain=${GCC_TOOLCHAIN_ROOT} --gcc-install-dir=${GCC_INSTALL_DIR}"
)

# Ensure correct runtime libstdc++
set(CMAKE_BUILD_RPATH "${GCC_INSTALL_DIR}")
set(CMAKE_INSTALL_RPATH "${GCC_INSTALL_DIR}")
set(CMAKE_SKIP_BUILD_RPATH FALSE)
set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)