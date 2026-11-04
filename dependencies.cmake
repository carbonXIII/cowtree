include(FetchContent)

FetchContent_Declare(ostree-cpp
  GIT_REPOSITORY "https://github.com/carbonXIII/ostree-cpp"
  GIT_TAG main
  OVERRIDE_FIND_PACKAGE)

FetchContent_Declare(erofs-utils
  GIT_REPOSITORY https://git.kernel.org/pub/scm/linux/kernel/git/xiang/erofs-utils.git
  GIT_TAG v1.9.1)
FetchContent_MakeAvailable(erofs-utils)
add_library(erofs-utils INTERFACE)
target_include_directories(erofs-utils INTERFACE ${erofs-utils_SOURCE_DIR}/include)

FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/03597a01ee50ed33e9dfd640b249b4be3799d395.zip
)
# For Windows: Prevent overriding the parent project's compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)
