find_package(GTest CONFIG REQUIRED)

add_executable(viewer_tests
  core/smoke_test.cpp
)

target_compile_features(viewer_tests PRIVATE cxx_std_20)
target_link_libraries(viewer_tests PRIVATE
  viewer_core
  GTest::gtest_main
)
viewer_enable_warnings(viewer_tests)

include(GoogleTest)
gtest_discover_tests(viewer_tests)
