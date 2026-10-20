cmake_minimum_required(VERSION 3.15)

if(POLICY CMP0065)
  # do not export symbols from executables
  # affects compiler checks in project(), so must be set before it
  cmake_policy(SET CMP0065 NEW)
endif()

project(tgrtmp VERSION 0.1.0 LANGUAGES CXX)

if(NOT DEFINED CMAKE_MODULE_PATH)
  set(CMAKE_MODULE_PATH "")
endif()

set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/CMake" "${CMAKE_MODULE_PATH}")

if(POLICY CMP0054)
  # do not expand quoted arguments
  cmake_policy(SET CMP0054 NEW)
endif()

if(POLICY CMP0074)
  # use environment variables to find libraries
  cmake_policy(SET CMP0074 NEW)
endif()

include(PreventInSourceBuild)
prevent_in_source_build()

find_package(OpenSSL REQUIRED)

if (CMAKE_VERSION VERSION_LESS 3.18)
    set(DEV_MODULE Development)
else()
    set(DEV_MODULE Development.Module)
endif()

find_package(Python COMPONENTS Interpreter ${DEV_MODULE} REQUIRED)
find_package(nanobind CONFIG REQUIRED)

if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the type of build." FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "MinSizeRel" "RelWithDebInfo")
endif()

nanobind_add_module(_core FREE_THREADED NOMINSIZE LTO
  tgrtmp/cpp/bindings.cpp
  tgrtmp/cpp/amf0.cpp
  tgrtmp/cpp/flv.cpp
  tgrtmp/cpp/h264.cpp
  tgrtmp/cpp/chunk.cpp
  tgrtmp/cpp/client.cpp
  tgrtmp/cpp/publisher.cpp
  tgrtmp/cpp/demux_ts.cpp
  tgrtmp/cpp/demux_mp4.cpp
  tgrtmp/cpp/demux_mkv.cpp
)

nanobind_add_stub(
  _core_stub
  INSTALL_TIME
  MODULE _core
  OUTPUT tgrtmp/_core.pyi
  MARKER_FILE tgrtmp/py.typed
  PYTHON_PATH $<TARGET_FILE_DIR:_core>
  DEPENDS _core
)

target_link_libraries(_core PRIVATE OpenSSL::SSL OpenSSL::Crypto)

if(MSVC)
  target_compile_options(_core PRIVATE /O2 /EHsc)
  target_link_libraries(_core PRIVATE ws2_32)
else()
  target_compile_options(_core PRIVATE -O2 -Wall -Wextra)
endif()

target_include_directories(_core PRIVATE tgrtmp/cpp)

install(TARGETS _core LIBRARY DESTINATION tgrtmp)
