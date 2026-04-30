find_library(MEGALON megalon ${CMAKE_CURRENT_BINARY_DIR})

add_executable(ycsb_benchmark ycsb_benchmark.cc)
target_include_directories(ycsb_benchmark PRIVATE ${MEGALON_SOURCE_DIR}/include/)
target_link_libraries(ycsb_benchmark megalon numa absl::log_initialize)

add_executable(kv-store kv_store.cc)
target_include_directories(kv-store PRIVATE ${MEGALON_SOURCE_DIR}/include/)
target_link_libraries(kv-store megalon numa absl::log_initialize)

if(FILE_INTERFACE)
    add_executable(page-cache page_cache.cc)
    target_include_directories(page-cache PRIVATE ${MEGALON_SOURCE_DIR}/include/)
    target_link_libraries(page-cache megalon numa absl::log_initialize)
endif()
