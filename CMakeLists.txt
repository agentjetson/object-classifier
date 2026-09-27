cmake_minimum_required(VERSION 3.20)
project(object_classifier VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(FetchContent)
find_package(Threads REQUIRED)
find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs videoio calib3d features2d)
find_package(PkgConfig REQUIRED)

# Eigen3 + yaml-cpp (required by motcpp)
# Eigen 3.3+ or 5.x (version scheme changed after 3.4)
find_package(Eigen3 CONFIG REQUIRED)
find_package(yaml-cpp QUIET)
if(NOT yaml-cpp_FOUND)
  pkg_check_modules(YAML_CPP REQUIRED yaml-cpp)
  if(NOT TARGET yaml-cpp::yaml-cpp)
    add_library(yaml-cpp::yaml-cpp INTERFACE IMPORTED)
    set_target_properties(yaml-cpp::yaml-cpp PROPERTIES
      INTERFACE_INCLUDE_DIRECTORIES "${YAML_CPP_INCLUDE_DIRS}"
      INTERFACE_LINK_LIBRARIES "${YAML_CPP_LIBRARIES}")
  endif()
endif()

# agentjetson/rf-detr
find_package(rfdetr_onnx QUIET)
if(NOT rfdetr_onnx_FOUND)
  message(STATUS "rfdetr_onnx not found — fetching agentjetson/rf-detr")
  set(ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(ENABLE_BENCHMARKS OFF CACHE BOOL "" FORCE)
  set(ENABLE_TESTS OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(rfdetr
    GIT_REPOSITORY https://github.com/agentjetson/rf-detr.git
    GIT_TAG main
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(rfdetr)
  set(RFDETR_TARGET rfdetr_onnx)
else()
  set(RFDETR_TARGET rfdetr_onnx::rfdetr_onnx)
endif()

# gRPC / protobuf
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

find_package(spdlog QUIET)
if(NOT spdlog_FOUND)
  FetchContent_Declare(spdlog GIT_REPOSITORY https://github.com/gabime/spdlog.git GIT_TAG v1.15.3 GIT_SHALLOW TRUE)
  set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
  set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(spdlog)
endif()

find_package(NATS QUIET)
if(NOT NATS_FOUND)
  FetchContent_Declare(nats GIT_REPOSITORY https://github.com/nats-io/nats.c.git GIT_TAG v3.9.1 GIT_SHALLOW TRUE)
  set(NATS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(NATS_BUILD_STREAMING OFF CACHE BOOL "" FORCE)
  set(NATS_BUILD_LIB_SHARED OFF CACHE BOOL "" FORCE)
  set(NATS_BUILD_LIB_STATIC ON CACHE BOOL "" FORCE)
  set(NATS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(nats)
  set(NATS_LIBRARIES nats_static)
  set(NATS_INCLUDE_DIRS ${nats_SOURCE_DIR}/src)
endif()

# motcpp — ByteTrack (motion-only, no ReID)
set(MOTCPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(MOTCPP_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(MOTCPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(MOTCPP_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(MOTCPP_ENABLE_ONNX OFF CACHE BOOL "" FORCE)
set(MOTCPP_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  motcpp
  GIT_REPOSITORY https://github.com/Geekgineer/motcpp.git
  GIT_TAG        main
  GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(motcpp)

# motcpp upstream omits some OpenCV headers; ensure imgproc/calib3d are visible
if(TARGET motcpp)
  target_include_directories(motcpp PUBLIC ${OpenCV_INCLUDE_DIRS})
  target_link_libraries(motcpp PUBLIC ${OpenCV_LIBS})
  # Force-include modules used by OBB IoU + CMC (missing in upstream headers)
  target_compile_options(motcpp PRIVATE
    -include opencv2/imgproc.hpp
    -include opencv2/calib3d.hpp
  )
endif()

# protos
set(PROTO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/proto)
set(PROTO_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${PROTO_GEN_DIR})
set(PROTO_FILES
  ${PROTO_ROOT}/common/v1/error.proto
  ${PROTO_ROOT}/detection/v1/detection.proto
  ${PROTO_ROOT}/detection/v1/annotated_video.proto
  ${PROTO_ROOT}/ingest/v1/ingest_service.proto
  ${PROTO_ROOT}/capture/v1/frame.proto
)
set(PROTO_SRCS)
set(GRPC_SRCS)
foreach(proto ${PROTO_FILES})
  get_filename_component(proto_name ${proto} NAME_WE)
  get_filename_component(proto_dir ${proto} DIRECTORY)
  file(RELATIVE_PATH rel_dir ${PROTO_ROOT} ${proto_dir})
  set(pb_cc ${PROTO_GEN_DIR}/${rel_dir}/${proto_name}.pb.cc)
  set(grpc_cc ${PROTO_GEN_DIR}/${rel_dir}/${proto_name}.grpc.pb.cc)
  add_custom_command(OUTPUT ${pb_cc} ${PROTO_GEN_DIR}/${rel_dir}/${proto_name}.pb.h
                             ${grpc_cc} ${PROTO_GEN_DIR}/${rel_dir}/${proto_name}.grpc.pb.h
    COMMAND ${_PROTOBUF_PROTOC} --proto_path=${PROTO_ROOT} --proto_path=${Protobuf_INCLUDE_DIRS}
      --cpp_out=${PROTO_GEN_DIR} --grpc_out=${PROTO_GEN_DIR}
      --plugin=protoc-gen-grpc=${_GRPC_CPP_PLUGIN} ${proto}
    DEPENDS ${proto})
  list(APPEND PROTO_SRCS ${pb_cc})
  list(APPEND GRPC_SRCS ${grpc_cc})
endforeach()

add_library(edge_proto ${PROTO_SRCS} ${GRPC_SRCS})
target_include_directories(edge_proto PUBLIC ${PROTO_GEN_DIR} ${Protobuf_INCLUDE_DIRS})
target_link_libraries(edge_proto PUBLIC protobuf::libprotobuf ${_GRPC_GRPCPP})

add_library(edge_common STATIC src/common/otel.cpp)
target_include_directories(edge_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src ${NATS_INCLUDE_DIRS})
target_link_libraries(edge_common PUBLIC edge_proto spdlog::spdlog ${NATS_LIBRARIES} Threads::Threads)

add_library(edge_cv
  src/cv/src/detector.cpp
  src/cv/src/postprocess.cpp
  src/cv/src/tracker.cpp
  src/cv/src/consumer_pipeline.cpp
)
target_include_directories(edge_cv PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cv/include
  ${OpenCV_INCLUDE_DIRS}
)
target_link_libraries(edge_cv PUBLIC
  ${OpenCV_LIBS}
  ${RFDETR_TARGET}
  motcpp::motcpp
  Threads::Threads
  spdlog::spdlog
)

add_library(camera_capture src/capture/capture.cpp)
target_include_directories(camera_capture PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${OpenCV_INCLUDE_DIRS}
)
target_link_libraries(camera_capture PUBLIC ${OpenCV_LIBS} spdlog::spdlog Threads::Threads)

add_executable(object_classifier src/classifier/main.cpp)
target_include_directories(object_classifier PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(object_classifier PRIVATE edge_cv edge_common edge_proto camera_capture)

message(STATUS "object-classifier (RF-DETR + motcpp ByteTrack) configured")
message(STATUS "  RF-DETR target: ${RFDETR_TARGET}")
