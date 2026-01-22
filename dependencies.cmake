include(FetchContent)

FetchContent_Declare(ostree-cpp
  GIT_REPOSITORY "https://github.com/carbonXIII/ostree-cpp"
  GIT_TAG main
  OVERRIDE_FIND_PACKAGE)

FetchContent_Declare(erofs-utils
  GIT_REPOSITORY https://git.kernel.org/pub/scm/linux/kernel/git/xiang/erofs-utils.git
  GIT_TAG 42a630d3f0cb63d1ec70aa45c3def9ef00b61f4f)
FetchContent_MakeAvailable(erofs-utils)
add_library(erofs-utils INTERFACE)
target_include_directories(erofs-utils INTERFACE ${erofs-utils_SOURCE_DIR}/include)
