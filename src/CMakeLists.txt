find_package(ostree-cpp REQUIRED)
find_package(fmt REQUIRED)
find_package(argparse REQUIRED)

add_executable(cowtree
  main.cpp
  extents.cpp
  mirror.cpp
  meta.cpp
  image.cpp)

target_link_libraries(cowtree PRIVATE
  ostree-cpp
  fmt::fmt
  erofs-utils
  argparse::argparse)
