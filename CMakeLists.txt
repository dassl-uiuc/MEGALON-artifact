cmake_minimum_required(VERSION 3.24)

set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

project(MEGALON VERSION 1.0 LANGUAGES CXX)

list(APPEND CMAKE_BUILD_RPATH "${CMAKE_BINARY_DIR}")
list(APPEND CMAKE_INSTALL_RPATH "${CMAKE_BINARY_DIR}")

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_CXX_STANDARD 23)
set(FETCHCONTENT_QUIET OFF)
set(ABSL_PROPAGATE_CXX_STD ON)
set(ABSL_ENABLE_INSTALL ON)
set(BUILD_TESTING OFF)
set(NR_DIR ${MEGALON_SOURCE_DIR}/third-party/nr_rust)
# message(STATUS "Using server implementation: ${MEGALON_SERVER_IMPL}")

set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "Disable yaml-cpp tests" FORCE)
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "Disable yaml-cpp tools" FORCE)
set(YAML_CPP_INSTALL OFF CACHE BOOL "Disable yaml-cpp install" FORCE)

add_subdirectory(${MEGALON_SOURCE_DIR}/third-party/yaml-cpp)

# Clang compile options
# set(MEGALON_COMPILE_OPTIONS "-Wall -Werror -Wpedantic -Wextra -Wshadow -Wunused-parameter -Wpointer-arith -Wcast-align -Wwrite-strings -Wredundant-decls -Wmissing-prototypes -Wmissing-declarations -Wnested-externs -Winline -Wuninitialized -Wconversion -Wstrict-prototypes -felide-constructors -Wno-gcc-compat -Wno-deprecated-volatile -Wno-cast-align -Wno-error=shadow -Wno-error=sign-conversion -Wno-error=c11-extensions")
set(MEGALON_COMPILE_OPTIONS "-Wall -Wpedantic -Wextra -Wshadow -Wunused-parameter -Wpointer-arith -Wcast-align -Wwrite-strings -Wredundant-decls -Wmissing-prototypes -Wmissing-declarations -Wnested-externs -Winline -Wuninitialized -Wconversion -Wstrict-prototypes -felide-constructors -Wno-gcc-compat -Wno-deprecated-volatile -Wno-cast-align -Wno-error=shadow -Wno-error=sign-conversion -Wno-error=c11-extensions")
# GCC compile options
# set(MEGALON_COMPILE_OPTIONS "-Wall -Werror -Wextra -Wcast-qual -Wconversion-null -Wmissing-declarations -Woverlength-strings -Wpointer-arith -Wunused-local-typedefs -Wunused-result -Wvarargs -Wvla -Wwrite-strings")

# cmake options (refer to compile flags for detailed explanation)
option(SCR "Enable SCR memory handling")
option(NO_COHERENCE "Enable non-coherent memory handling")
option(FULL_COHERENCE "assumes the entire cxl cache is coherent")
option(LIMITED_SCR "limited SCR")
option(PARTITIONED_NODE "enable partitioned node")
option(FILE_INTERFACE "Enable file-interface benchmark/code paths" ON)

set(SCR ON CACHE BOOL "Enable SCR memory handling" FORCE)
set(NO_COHERENCE OFF CACHE BOOL "Enable non-coherent memory handling" FORCE)
set(LIMITED_SCR ON CACHE BOOL "limited SCR" FORCE)
set(FULL_COHERENCE OFF CACHE BOOL "assumes the entire cxl cache is coherent" FORCE)
set(PARTITIONED_NODE OFF CACHE BOOL "enable partitioned node" FORCE)
set(FILE_INTERFACE OFF CACHE BOOL "Enable file-interface benchmark/code paths" FORCE)

if (NOT SCR AND LIMITED_SCR)
    message(FATAL_ERROR "LIMITED_SCR can not be ON without SCR!")
endif()
if (SCR AND NO_COHERENCE)
    message(FATAL_ERROR "SCR and NO_COHERENCE cannot be ON at the same time!")
endif()
if (FULL_COHERENCE AND (NO_COHERENCE OR SCR))
    message(FATAL_ERROR "SCR and FULL_COHERENCE cannot be ON at the same time!")
endif()

# compile flags
add_definitions(-DNR=1)                 # use NR for gcd

if (SCR)
    add_definitions(-DSCR=1)            # sequence numbers and cache flush for object data
endif()

if (NO_COHERENCE)
    add_definitions(-DNO_COHERENCE=1)   # assumes all memory region is non-coherent; should be enabled with NR
endif()

add_definitions(-DSAMPLING)             # using sampling to reclaim dynamic wmeta (sequence lock)     

if (LIMITED_SCR)
    add_definitions(-DDYN_WMETA=1)      # with limited coherence region, seqlock needs to be dynamic
endif()

if (FULL_COHERENCE)
    add_definitions(-DFULL_COHERENCE=1)
endif()

if (PARTITIONED_NODE)
    add_definitions(-DPARTITIONED_NODE=1)
endif()

if (FILE_INTERFACE)
    add_definitions(-DFILE_INTERFACE=1)
endif()

# add_definitions(-DC3_RWLOCK=1)        # use rwlock as consistency primitive (1 for std::shared_mutex, 2 for absl::Mutex)
# add_definitions(-DBTREE_REPLICATE=1)    # enable btree replication policy (no effect if REPLICATION is OFF)

# Make the shared library
file(GLOB MEGALON_SOURCES CONFIGURE_DEPENDS
     "${MEGALON_SOURCE_DIR}/src/common/*.cc"
     "${MEGALON_SOURCE_DIR}/src/core/*.cc"
     "${MEGALON_SOURCE_DIR}/src/index/*.cc"
     "${MEGALON_SOURCE_DIR}/src/manager/*.cc"
     "${MEGALON_SOURCE_DIR}/src/shared_memory/*.cc"
     "${MEGALON_SOURCE_DIR}/lib/*.cc"
     "${MEGALON_SOURCE_DIR}/lib/glibc/*.cc")
add_library(megalon SHARED ${MEGALON_SOURCES})
message(STATUS "Using sources: ${MEGALON_SOURCES}")
target_compile_features(megalon PUBLIC cxx_std_23)

# support -mclflushopt
include(CheckCXXCompilerFlag)
check_cxx_compiler_flag("-mclflushopt" COMPILER_SUPPORTS_CLFLUSHOPT)
if(COMPILER_SUPPORTS_CLFLUSHOPT)
    string(APPEND MEGALON_COMPILE_OPTIONS " -mclflushopt")
    # list(APPEND MEGALON_COMPILE_OPTIONS "-mclflushopt")
else()
    message(ERROR "CPU does not support CLFLUSHOPT")
endif()

add_dependencies(megalon fmt) # for auto formatting

set_source_files_properties(${MEGALON_SOURCES} PROPERTIES COMPILE_FLAGS ${MEGALON_COMPILE_OPTIONS})
target_include_directories(megalon PRIVATE ${MEGALON_SOURCE_DIR}/lib/ ${MEGALON_SOURCE_DIR}/src/)
target_include_directories(megalon PRIVATE ${MEGALON_SOURCE_DIR}/third-party/robin-map/include/)
target_include_directories(megalon PRIVATE ${MEGALON_SOURCE_DIR}/third-party/unordered_dense/include/)
target_include_directories(megalon PRIVATE ${MEGALON_SOURCE_DIR}/third-party/hostrpc/)
target_include_directories(megalon SYSTEM PRIVATE ${MEGALON_SOURCE_DIR}/third-party/oneTBB/include/)
target_include_directories(megalon SYSTEM PUBLIC ${MEGALON_SOURCE_DIR}/third-party/gtl/include/)
target_include_directories(megalon SYSTEM PUBLIC ${MEGALON_SOURCE_DIR}/include/)
# set_property(TARGET megalon PROPERTY POSITION_INDEPENDENT_CODE ON)

# surpress yaml-cpp warnings
get_target_property(YAML_CPP_INCLUDE_DIR yaml-cpp::yaml-cpp INTERFACE_INCLUDE_DIRECTORIES)
target_include_directories(megalon SYSTEM PRIVATE ${YAML_CPP_INCLUDE_DIR})

add_subdirectory(${MEGALON_SOURCE_DIR}/third-party/abseil-cpp)

target_link_libraries(megalon memkind absl::log absl::check absl::flat_hash_map absl::status yaml-cpp::yaml-cpp numa atomic)

# linking nr
target_link_directories(megalon PUBLIC "${NR_DIR}/ffi/target/release")
if (LIMITED_SCR)
    message("linking limited scr nr variant")
    target_link_libraries(megalon nr_hashmap hwloc)
elseif (NO_COHERENCE)
    message("linking no-coherence nr variant")
    target_link_libraries(megalon nr_hashmap_noco hwloc)
else()
    message("linking original nr variant")
    target_link_libraries(megalon nr_hashmap_orig hwloc)
endif()

# find_library(oneTBB megalon ${CMAKE_CURRENT_BINARY_DIR})

set(TBB_TEST OFF CACHE INTERNAL "" FORCE)
set(TBB_CPF ON CACHE INTERNAL "" FORCE)
set_directory_properties(PROPERTIES
    COMPILE_OPTIONS "-w"
)
add_subdirectory(${MEGALON_SOURCE_DIR}/third-party/oneTBB)

target_link_libraries(megalon TBB::tbb TBB::tbbmalloc)

add_subdirectory(benchmarks)

set_property(TARGET base strings status str_format_internal check
             log log_internal_log_sink_set log_internal_message log_globals
             cord cordz_info cordz_functions cordz_handle raw_hash_set
             raw_logging_internal time_zone hash debugging_internal 
             malloc_internal int128 crc32c crc_internal synchronization
             PROPERTY POSITION_INDEPENDENT_CODE TRUE)

# Add clang-format as a target
add_custom_target(
    fmt COMMAND find 
        ${MEGALON_SOURCE_DIR}/lib
        ${MEGALON_SOURCE_DIR}/src
        ${MEGALON_SOURCE_DIR}/benchmarks
        -type f "\\(" -name \"\*.h\" -o -iname \"\*.cc\" -o -iname \"\*.hpp\" "\\)"
        | tee /dev/stderr
        | xargs clang-format -i
)
