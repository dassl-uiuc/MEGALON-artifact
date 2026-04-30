capnp_generate_cpp(RackOBJProtoSources RackOBJProtoHeaders rackobj.capnp)
add_library(rackobj-proto ${RackOBJProtoSources})
target_link_libraries(rackobj-proto CapnProto::capnp)
target_include_directories(rackobj-proto PUBLIC ${CMAKE_CURRENT_BINARY_DIR})
