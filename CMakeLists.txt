cmake_minimum_required(VERSION 3.28)

project(FastImageViewer VERSION 0.1.0 LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)
include(CTest)
find_package(Threads REQUIRED)

add_library(viewer_core STATIC
  src/core/directory_navigator.cpp
  src/core/format_probe.cpp
  src/core/natural_sort.cpp
  src/core/priority_executor.cpp
  src/core/view_transform.cpp
)
target_compile_features(viewer_core PUBLIC cxx_std_20)
target_include_directories(viewer_core
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)
viewer_enable_warnings(viewer_core)
target_link_libraries(viewer_core PUBLIC Threads::Threads)

if(WIN32)
  add_library(viewer_wic STATIC
    src/platform/wic_decoder.cpp
  )
  target_compile_features(viewer_wic PUBLIC cxx_std_20)
  target_include_directories(viewer_wic
    PUBLIC
      "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  )
  viewer_enable_warnings(viewer_wic)
  target_link_libraries(viewer_wic PUBLIC
    viewer_core
    windowscodecs
    ole32
  )

  add_library(viewer_render STATIC
    src/render/d3d_renderer.cpp
  )
  target_compile_features(viewer_render PUBLIC cxx_std_20)
  target_include_directories(viewer_render
    PUBLIC
      "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  )
  viewer_enable_warnings(viewer_render)
  target_link_libraries(viewer_render PUBLIC
    viewer_core
    d2d1
    d3d11
    dxgi
  )
endif()

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
