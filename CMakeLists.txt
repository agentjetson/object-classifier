cmake_minimum_required(VERSION 3.20)
project(object_classifier VERSION 0.3.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(FetchContent)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)

find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs videoio video)
if(OpenCV_VERSION VERSION_LESS "4.5")
  message(FATAL_ERROR "OpenCV >= 4.5 required (found ${OpenCV_VERSION})")
endif()

if(NOT DEFINED ONNXRUNTIME_ROOT AND DEFINED ENV{ONNXRUNTIME_ROOT})
  set(ONNXRUNTIME_ROOT $ENV{ONNXRUNTIME_ROOT})
endif()
if(ONNXRUNTIME_ROOT)
  set(ONNXRUNTIME_INCLUDE_DIRS ${ONNXRUNTIME_ROOT}/include)
  find_library(ONNXRUNTIME_LIB onnxruntime
    PATHS ${ONNXRUNTIME_ROOT}/lib ${ONNXRUNTIME_ROOT}/lib64 NO_DEFAULT_PATH)
else()
  find_path(ONNXRUNTIME_INCLUDE_DIRS onnxruntime_cxx_api.h QUIET)
  find_library(ONNXRUNTIME_LIB onnxruntime QUIET)
endif()
if(NOT ONNXRUNTIME_LIB)
  message(FATAL_ERROR "ONNX Runtime required — set ONNXRUNTIME_ROOT")
endif()

find_package(rfdetr_onnx QUIET)
if(NOT rfdetr_onnx_FOUND)
  set(ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(ENABLE_BENCHMARKS OFF CACHE BOOL "" FORCE)
  set(ENABLE_TESTS OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(rfdetr
    GIT_REPOSITORY https://github.com/agentjetson/rf-detr.git
    GIT_TAG main GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(rfdetr)
  set(RFDETR_TARGET rfdetr_onnx)
else()
  set(RFDETR_TARGET rfdetr_onnx::rfdetr_onnx)
endif()

set(_use_grpc_config FALSE)
if(NOT APPLE)
  find_package(gRPC CONFIG QUIET)
  if(gRPC_FOUND)
    set(_use_grpc_config TRUE)
  endif()
endif()
if(_use_grpc_config)
  set(_GRPC_GRPCPP gRPC::grpc++)
  if(TARGET gRPC::grpc_cpp_plugin)
    set(_GRPC_CPP_PLUGIN $<TARGET_FILE:gRPC::grpc_cpp_plugin>)
  else()
    find_program(_GRPC_CPP_PLUGIN grpc_cpp_plugin REQUIRED)
  endif()
  if(NOT TARGET protobuf::libprotobuf)
    find_package(Protobuf REQUIRED)
  endif()
else()
  pkg_check_modules(PROTOBUF REQUIRED IMPORTED_TARGET protobuf)
  pkg_check_modules(GRPC REQUIRED IMPORTED_TARGET grpc++)
  if(NOT TARGET protobuf::libprotobuf)
    add_library(protobuf::libprotobuf ALIAS PkgConfig::PROTOBUF)
  endif()
  if(PROTOBUF_INCLUDE_DIRS)
    set(Protobuf_INCLUDE_DIRS ${PROTOBUF_INCLUDE_DIRS})
  endif()
  set(_GRPC_GRPCPP PkgConfig::GRPC)
  find_program(_GRPC_CPP_PLUGIN grpc_cpp_plugin REQUIRED)
  find_program(_PROTOBUF_PROTOC protoc REQUIRED)
endif()
if(NOT DEFINED _PROTOBUF_PROTOC)
  if(TARGET protobuf::protoc)
    set(_PROTOBUF_PROTOC $<TARGET_FILE:protobuf::protoc>)
  else()
    find_program(_PROTOBUF_PROTOC protoc REQUIRED)
  endif()
endif()

set(_PROTO_INCLUDE_PATH "")
if(DEFINED Protobuf_INCLUDE_DIRS)
  foreach(_inc ${Protobuf_INCLUDE_DIRS})
    if(EXISTS "${_inc}/google/protobuf/descriptor.proto")
      set(_PROTO_INCLUDE_PATH "${_inc}")
      break()
    endif()
  endforeach()
endif()
if(NOT _PROTO_INCLUDE_PATH)
  foreach(_cand "/opt/homebrew/include" "/usr/local/include" "/usr/include")
    if(EXISTS "${_cand}/google/protobuf/descriptor.proto")
      set(_PROTO_INCLUDE_PATH "${_cand}")
      break()
    endif()
  endforeach()
endif()

find_package(spdlog QUIET)
if(NOT spdlog_FOUND)
  FetchContent_Declare(spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.17.0 GIT_SHALLOW TRUE)
  set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
  set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(spdlog)
endif()

# agentjetson/edge-common (OTEL, env, signal, NATS publisher)
set(AJ_EDGE_COMMON_GIT_TAG feat/nats-publisher CACHE STRING "agentjetson/edge-common ref")
FetchContent_Declare(aj_edge_common
  GIT_REPOSITORY https://github.com/agentjetson/edge-common.git
  GIT_TAG ${AJ_EDGE_COMMON_GIT_TAG}
  GIT_SHALLOW TRUE)
set(AJ_EDGE_WITH_OTEL ON CACHE BOOL "" FORCE)
set(AJ_EDGE_WITH_NATS ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(aj_edge_common)

# Canonical protos from agentjetson/core
set(AGENTJETSON_CONTRACT_GIT_TAG main CACHE STRING "agentjetson/core ref for protos")
FetchContent_Declare(agentjetson_contract
  GIT_REPOSITORY https://github.com/agentjetson/core.git
  GIT_TAG ${AGENTJETSON_CONTRACT_GIT_TAG}
  GIT_SHALLOW TRUE)
FetchContent_GetProperties(agentjetson_contract)
if(NOT agentjetson_contract_POPULATED)
  FetchContent_Populate(agentjetson_contract)
endif()
set(PROTO_ROOT ${agentjetson_contract_SOURCE_DIR}/proto)
set(PROTO_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${PROTO_GEN_DIR})

set(PROTO_FILES
  ${PROTO_ROOT}/common/v1/error.proto
  ${PROTO_ROOT}/detection/v1/detection.proto
  ${PROTO_ROOT}/detection/v1/annotated_video.proto
  ${PROTO_ROOT}/ingest/v1/ingest_service.proto
  ${PROTO_ROOT}/capture/v1/frame.proto
  ${PROTO_ROOT}/capture/v1/frame_service.proto
)

set(PROTO_SRCS)
set(GRPC_SRCS)
foreach(proto ${PROTO_FILES})
  get_filename_component(proto_name ${proto} NAME_WE)
  get_filename_component(proto_dir ${proto} DIRECTORY)
  file(RELATIVE_PATH rel_dir ${PROTO_ROOT} ${proto_dir})
  set(pb_cc   ${PROTO_GEN_DIR}/${rel_dir}/${proto_name}.pb.cc)
  set(grpc_cc ${PROTO_GEN_DIR}/${rel_dir}/${proto_name}.grpc.pb.cc)
  set(_PROTOC_CMD
    ${_PROTOBUF_PROTOC}
    --proto_path=${PROTO_ROOT}
    --cpp_out=${PROTO_GEN_DIR}
    --grpc_out=${PROTO_GEN_DIR}
    --plugin=protoc-gen-grpc=${_GRPC_CPP_PLUGIN}
    ${proto}
  )
  if(_PROTO_INCLUDE_PATH)
    list(INSERT _PROTOC_CMD 2 --proto_path=${_PROTO_INCLUDE_PATH})
  endif()
  add_custom_command(
    OUTPUT ${pb_cc} ${PROTO_GEN_DIR}/${rel_dir}/${proto_name}.pb.h
           ${grpc_cc} ${PROTO_GEN_DIR}/${rel_dir}/${proto_name}.grpc.pb.h
    COMMAND ${_PROTOC_CMD}
    DEPENDS ${proto}
    COMMENT "Generating ${proto_name}.pb.cc / .grpc.pb.cc"
    VERBATIM
  )
  list(APPEND PROTO_SRCS ${pb_cc})
  list(APPEND GRPC_SRCS ${grpc_cc})
endforeach()

add_library(edge_proto ${PROTO_SRCS} ${GRPC_SRCS})
target_include_directories(edge_proto PUBLIC ${PROTO_GEN_DIR} ${Protobuf_INCLUDE_DIRS})
target_link_libraries(edge_proto PUBLIC protobuf::libprotobuf ${_GRPC_GRPCPP})

add_library(edge_cv
  src/cv/src/detector.cpp
  src/cv/src/postprocess.cpp
  src/cv/src/tracker.cpp
  src/cv/src/consumer_pipeline.cpp
)
target_include_directories(edge_cv PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cv/include
  ${OpenCV_INCLUDE_DIRS}
  ${ONNXRUNTIME_INCLUDE_DIRS}
)
target_link_libraries(edge_cv PUBLIC
  ${OpenCV_LIBS}
  ${ONNXRUNTIME_LIB}
  ${RFDETR_TARGET}
  Threads::Threads
  spdlog::spdlog
)

add_library(camera_capture src/capture/capture.cpp)
target_include_directories(camera_capture PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${OpenCV_INCLUDE_DIRS}
)
target_link_libraries(camera_capture PUBLIC
  ${OpenCV_LIBS}
  spdlog::spdlog
  Threads::Threads
)

add_executable(object_classifier src/classifier/main.cpp)
target_include_directories(object_classifier PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(object_classifier PRIVATE
  edge_cv
  aj::edge_common
  edge_proto
  camera_capture
)

message(STATUS "object-classifier configured")
message(STATUS "  OpenCV:    ${OpenCV_VERSION}")
message(STATUS "  RF-DETR:   ${RFDETR_TARGET}")
message(STATUS "  edge-common: ${AJ_EDGE_COMMON_GIT_TAG}")
message(STATUS "  Protos:    agentjetson/core (${AGENTJETSON_CONTRACT_GIT_TAG})")
