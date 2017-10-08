cmake_minimum_required (VERSION 2.6)
project (Bloaty)

set(RE2_BUILD_TESTING OFF CACHE BOOL "enable testing for RE2" FORCE)
add_subdirectory(third_party/re2)
include_directories(third_party/re2)

include_directories(.)
include_directories(src)
include_directories(third_party/abseil-cpp)
set(CMAKE_CXX_FLAGS "-std=c++11 -W -Wall -Wno-sign-compare")
set(CMAKE_CXX_FLAGS_DEBUG "-g")
set(CMAKE_CXX_FLAGS_RELEASE "-O2")

add_library(libbloaty
    src/bloaty.cc
    src/dwarf.cc
    src/elf.cc
    src/macho.cc
    # Until Abseil has a proper CMake build system
    third_party/abseil-cpp/absl/base/internal/throw_delegate.cc
    third_party/abseil-cpp/absl/base/internal/raw_logging.cc # Grrrr...
    third_party/abseil-cpp/absl/strings/ascii.cc
    third_party/abseil-cpp/absl/strings/internal/memutil.cc
    third_party/abseil-cpp/absl/strings/numbers.cc
    third_party/abseil-cpp/absl/strings/str_cat.cc
    third_party/abseil-cpp/absl/strings/string_view.cc
    )

add_executable(bloaty src/main.cc)
target_link_libraries(bloaty libbloaty re2)

# All of this is to add -pthread, which is required by re2 (not us).
find_package(Threads REQUIRED)
if(THREADS_HAVE_PTHREAD_ARG)
  set_property(TARGET bloaty PROPERTY COMPILE_OPTIONS "-pthread")
  set_property(TARGET bloaty PROPERTY INTERFACE_COMPILE_OPTIONS "-pthread")
endif()
if(CMAKE_THREAD_LIBS_INIT)
  target_link_libraries(bloaty "${CMAKE_THREAD_LIBS_INIT}")
endif()

enable_testing()

if(BUILD_TESTING)
  add_subdirectory(third_party/googletest)
  include_directories(third_party/googletest/googletest/include)
  include_directories(third_party/googletest/googlemock/include)

  set(TEST_TARGETS
      bloaty_test
      bloaty_misc_test
      range_map_test
      )

  foreach(target ${TEST_TARGETS})
    add_executable(${target} tests/${target}.cc)
    target_link_libraries(${target} libbloaty re2 gtest_main gmock)
  endforeach(target)

  add_test(NAME range_map_test COMMAND range_map_test)
  add_test(NAME bloaty_test_x86-64 COMMAND bloaty_test WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/tests/testdata/linux-x86_64)
  add_test(NAME bloaty_test_x86 COMMAND bloaty_test WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/tests/testdata/linux-x86)
  add_test(NAME bloaty_misc_test COMMAND bloaty_misc_test WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/tests/testdata/misc)
endif()
