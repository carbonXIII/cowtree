find_package(ostree-cpp REQUIRED)
find_package(fmt REQUIRED)
find_package(argparse REQUIRED)

add_executable(cowtree
  driver/main.cpp
  driver/tree.cpp
  hostfs/extents.cpp
  hostfs/mirror.cpp
  erofs/image.cpp)

target_include_directories(cowtree PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(cowtree PRIVATE
  ostree-cpp
  fmt::fmt
  erofs-utils
  argparse::argparse)
