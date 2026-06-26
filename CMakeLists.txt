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
    dwrite
    dxgi
  )

  add_executable(fast_viewer WIN32
    src/app/app.cpp
    src/app/app.rc
    src/app/main_window.cpp
    src/app/resource.h
    src/app/winmain.cpp
  )
  target_compile_features(fast_viewer PRIVATE cxx_std_20)
  target_compile_definitions(fast_viewer PRIVATE
    UNICODE
    _UNICODE
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
  )
  target_include_directories(fast_viewer PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
  )
  viewer_enable_warnings(fast_viewer)
  target_link_libraries(fast_viewer PRIVATE
    viewer_core
    viewer_wic
    viewer_render
    comdlg32
    comctl32
    msimg32
    shell32
    ole32
  )
endif()

if(BUILD_TESTING)
  add_subdirectory(tests)
  target_sources(viewer_tests PRIVATE
    tests/app/window_state_test.cpp
  )
endif()
