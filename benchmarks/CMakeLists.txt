find_library(HCMETA hcmeta ${CMAKE_CURRENT_BINARY_DIR})

add_executable(kv-store kv_store.cc)
target_include_directories(kv-store PRIVATE ${HCMeta_SOURCE_DIR}/include/)
target_link_libraries(kv-store hcmeta numa absl::log_initialize)

add_executable(ycsb_benchmark ycsb_benchmark.cc)
target_include_directories(ycsb_benchmark PRIVATE ${HCMeta_SOURCE_DIR}/include/)
target_link_libraries(ycsb_benchmark hcmeta numa absl::log_initialize)

if(FILE_INTERFACE)
    add_executable(page-cache page_cache.cc)
    target_include_directories(page-cache PRIVATE ${HCMeta_SOURCE_DIR}/include/)
    target_link_libraries(page-cache hcmeta numa absl::log_initialize)
endif()