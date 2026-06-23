find_package(GTest CONFIG REQUIRED)

add_executable(viewer_tests
  core/directory_navigator_test.cpp
  core/format_probe_test.cpp
  core/image_types_test.cpp
  core/natural_sort_test.cpp
  core/priority_executor_test.cpp
  core/smoke_test.cpp
)

if(WIN32)
  target_sources(viewer_tests PRIVATE
    platform/wic_decoder_test.cpp
  )
endif()

target_compile_features(viewer_tests PRIVATE cxx_std_20)
target_link_libraries(viewer_tests PRIVATE
  viewer_core
  GTest::gtest_main
)
if(WIN32)
  target_link_libraries(viewer_tests PRIVATE viewer_wic)
  target_compile_definitions(viewer_tests PRIVATE
    "TEST_FIXTURE_DIR=L\"${CMAKE_CURRENT_SOURCE_DIR}/fixtures\""
  )
endif()
viewer_enable_warnings(viewer_tests)

include(GoogleTest)
gtest_discover_tests(viewer_tests)
