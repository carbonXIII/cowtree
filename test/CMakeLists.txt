enable_testing()

add_executable(test_alloc test_alloc.cpp)
target_include_directories(test_alloc PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_alloc PUBLIC fmt GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(test_alloc)
