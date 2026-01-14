include(FetchContent)

FetchContent_Declare(ostree-cpp
  GIT_REPOSITORY "https://github.com/carbonXIII/ostree-cpp"
  GIT_TAG main
  OVERRIDE_FIND_PACKAGE)

FetchContent_Declare(erofs-utils
  GIT_REPOSITORY https://git.kernel.org/pub/scm/linux/kernel/git/xiang/erofs-utils.git
  GIT_TAG v1.8.10)
FetchContent_MakeAvailable(erofs-utils)
add_library(erofs-utils INTERFACE)
target_include_directories(erofs-utils INTERFACE ${erofs-utils_SOURCE_DIR}/include)
