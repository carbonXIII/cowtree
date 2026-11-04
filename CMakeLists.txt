cmake_minimum_required(VERSION 4.0)
project(cowtree)
set(CMAKE_CXX_STANDARD 26)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)

include(dependencies.cmake)

add_subdirectory(src)
add_subdirectory(test)

install(TARGETS cowtree EXPORT cowtree DESTINATION bin)
