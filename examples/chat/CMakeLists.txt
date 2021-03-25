cmake_minimum_required(VERSION 3.4)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

project(msgexchange CXX)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Build type (Debug, Release, RelWithDebInfo, MinSizeRel)" FORCE)
endif()

add_compile_options(-Wall -Werror)


set(protobuf_MODULE_COMPATIBLE TRUE)
find_package(Protobuf CONFIG REQUIRED HINTS /opt/grpc)
message(STATUS "Using protobuf ${Protobuf_VERSION}")

set(_PROTOBUF_PROTOC $<TARGET_FILE:protobuf::protoc>)

find_package(gRPC CONFIG REQUIRED HINTS /opt/grpc)
message(STATUS "Using gRPC ${gRPC_VERSION}")

set(_GRPC_CPP_PLUGIN_EXECUTABLE $<TARGET_FILE:gRPC::grpc_cpp_plugin>)

set(CMAKE_THREAD_PREFER_PTHREAD TRUE)
set(THREADS_PREFER_PTHREAD_FLAG TRUE)
find_package(Threads REQUIRED)


set(SRC_DIR ${CMAKE_SOURCE_DIR})
set(INC_DIR ${CMAKE_SOURCE_DIR}/../../..)
set(PROTO_DIR ${CMAKE_SOURCE_DIR}/../../proto)

get_filename_component(PROTO_FILE "${PROTO_DIR}/msgexchange.proto" ABSOLUTE)
get_filename_component(PROTO_PATH "${PROTO_FILE}" PATH)
get_filename_component(PROTO_NAME "${PROTO_FILE}" NAME_WE)

#generated sources
set(PROTO_SRCS "${CMAKE_CURRENT_BINARY_DIR}/${PROTO_NAME}.pb.cc")
set(PROTO_HDRS "${CMAKE_CURRENT_BINARY_DIR}/${PROTO_NAME}.pb.h")
set(GRPC_SRCS "${CMAKE_CURRENT_BINARY_DIR}/${PROTO_NAME}.grpc.pb.cc")
set(GRPC_HDRS "${CMAKE_CURRENT_BINARY_DIR}/${PROTO_NAME}.grpc.pb.h")

add_custom_command(
    OUTPUT "${PROTO_SRCS}" "${PROTO_HDRS}" "${GRPC_SRCS}" "${GRPC_HDRS}"
    COMMAND ${_PROTOBUF_PROTOC}
    ARGS --grpc_out "${CMAKE_CURRENT_BINARY_DIR}"
        --cpp_out "${CMAKE_CURRENT_BINARY_DIR}"
        -I "${PROTO_PATH}"
        --plugin=protoc-gen-grpc="${_GRPC_CPP_PLUGIN_EXECUTABLE}"
        "${PROTO_FILE}"
    DEPENDS "${PROTO_FILE}")



add_executable(msgxclient
    ${SRC_DIR}/client/main.cpp
    ${PROTO_SRCS}
    ${GRPC_SRCS})

target_include_directories(msgxclient PRIVATE
    ${INC_DIR}
    /opt/grpc/include
    ${CMAKE_CURRENT_BINARY_DIR}
    )
target_link_libraries(msgxclient
    gRPC::grpc++_unsecure
    protobuf::libprotobuf
)

add_executable(msgxserver
    ${SRC_DIR}/server/main.cpp
    ${PROTO_SRCS}
    ${GRPC_SRCS}
    )

target_include_directories(msgxserver PRIVATE
    ${INC_DIR}
    /opt/grpc/include
    ${CMAKE_CURRENT_BINARY_DIR}
    )

target_link_libraries(msgxserver
    gRPC::grpc++_unsecure
    protobuf::libprotobuf
)
