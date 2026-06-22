cmake_minimum_required(VERSION 3.28)

project(FastImageViewer VERSION 0.1.0 LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)
include(CTest)

add_library(viewer_core STATIC
  src/core/directory_navigator.cpp
  src/core/format_probe.cpp
  src/core/natural_sort.cpp
)
target_compile_features(viewer_core PUBLIC cxx_std_20)
target_include_directories(viewer_core
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)
viewer_enable_warnings(viewer_core)

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
