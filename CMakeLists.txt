cmake_minimum_required(VERSION 3.5)
cmake_policy(SET CMP0048 NEW)
if(POLICY CMP0091)
  cmake_policy(SET CMP0091 NEW)
endif()
project (Bloaty VERSION 1.1)
include(CTest)
set(CMAKE_CXX_STANDARD 17)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)  # Group projects in visual studio

# Options we define for users.
option(BLOATY_ENABLE_ASAN "Enable address sanitizer." OFF)
option(BLOATY_ENABLE_UBSAN "Enable undefined behavior sanitizer." OFF)
option(BLOATY_ENABLE_CMAKETARGETS "Enable installing cmake target files." ON)
option(BLOATY_ENABLE_BUILDID "Enable build id." ON)
option(BLOATY_ENABLE_RE2 "Enable the support for regular expression functions." ON)

if(UNIX)
find_package(PkgConfig)
find_package(ZLIB)
if(BLOATY_ENABLE_RE2)
  pkg_search_module(RE2 re2)
endif()
pkg_search_module(CAPSTONE capstone)
pkg_search_module(PROTOBUF protobuf)
if(BLOATY_ENABLE_RE2)
  if(RE2_FOUND)
    MESSAGE(STATUS "System re2 found, using")
  else()
    MESSAGE(STATUS "System re2 not found, using bundled version")
  endif()
endif()
if(CAPSTONE_FOUND)
  MESSAGE(STATUS "System capstone found, using")
else()
  MESSAGE(STATUS "System capstone not found, using bundled version")
endif()
if(PROTOBUF_FOUND)
  MESSAGE(STATUS "System protobuf found, using")
else()
  MESSAGE(STATUS "System protobuf not found, using bundled version")
endif()
if (ZLIB_FOUND)
  MESSAGE(STATUS "System zlib found, using")
else()
  MESSAGE(STATUS "System zlib not found, using bundled version")
endif()
endif()

# Set default build type.
if(NOT CMAKE_BUILD_TYPE)
  message(STATUS "Setting build type to 'RelWithDebInfo' as none was specified.")
  set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING
      "Choose the type of build, options are: Debug Release RelWithDebInfo MinSizeRel."
      FORCE)
endif()

# Check out Git submodules.
if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.gitmodules")
  execute_process (COMMAND git submodule update --init --recursive
                   WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endif()

# Add third_party libraries, disabling as much as we can of their builds.

add_definitions(-D_LIBCXXABI_FUNC_VIS=)  # For Demumble.
if(BLOATY_ENABLE_RE2)
  add_definitions(-DUSE_RE2)
endif()

# Set MSVC runtime before including thirdparty libraries
if(MSVC)
  if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.15)
    set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded$<$<CONFIG:Debug>:Debug>)
  else()
    # Link also the runtime library statically so that MSVCR*.DLL is not required at runtime.
    # https://msdn.microsoft.com/en-us/library/2kzt1wy3.aspx
    # This is achieved by replacing msvc option /MD with /MT and /MDd with /MTd
    # http://www.cmake.org/Wiki/CMake_FAQ#How_can_I_build_my_MSVC_application_with_a_static_runtime.3F
    foreach(flag_var
        CMAKE_CXX_FLAGS CMAKE_CXX_FLAGS_DEBUG CMAKE_CXX_FLAGS_RELEASE
        CMAKE_CXX_FLAGS_MINSIZEREL CMAKE_CXX_FLAGS_RELWITHDEBINFO)
      if (flag_var MATCHES "/MD")
        string(REGEX REPLACE "/MD" "/MT" ${flag_var} "${${flag_var}}")
      endif()
    endforeach()
  endif()
endif()

set(THREADS_PREFER_PTHREAD_FLAG TRUE)
find_package(Threads REQUIRED)

if(UNIX)
  if(BLOATY_ENABLE_RE2)
    if(RE2_FOUND)
      include_directories(${RE2_INCLUDE_DIRS})
    else()
      set(RE2_BUILD_TESTING OFF CACHE BOOL "enable testing for RE2" FORCE)
      add_subdirectory(third_party/re2)
      include_directories(third_party/re2)
    endif()
  endif()
  if(CAPSTONE_FOUND)
    include_directories(${CAPSTONE_INCLUDE_DIRS})
  else()
    set(CAPSTONE_BUILD_SHARED OFF CACHE BOOL "Build shared library" FORCE)
    set(CAPSTONE_BUILD_TESTS OFF CACHE BOOL "Build tests" FORCE)
    add_subdirectory(third_party/capstone)
    include_directories(third_party/capstone/include)
  endif()
  if(PROTOBUF_FOUND)
    include_directories(${PROTOBUF_INCLUDE_DIRS})
  else()
    set(protobuf_BUILD_TESTS OFF CACHE BOOL "enable tests for proto2" FORCE)
    set(protobuf_BUILD_SHARED_LIBS OFF CACHE BOOL "enable shared libs for proto2" FORCE)
    add_subdirectory(third_party/protobuf/cmake)
    include_directories(SYSTEM third_party/protobuf/src)
  endif()
  if(NOT ZLIB_FOUND)
    add_subdirectory(third_party/zlib)
    include_directories(SYSTEM third_party/zlib)
  endif()
else()
  if(BLOATY_ENABLE_RE2)
    set(RE2_BUILD_TESTING OFF CACHE BOOL "enable testing for RE2" FORCE)
    add_subdirectory(third_party/re2)
    include_directories(third_party/re2)
    set_property(TARGET re2 PROPERTY FOLDER "third_party")
  endif()

  set(CAPSTONE_BUILD_SHARED OFF CACHE BOOL "Build shared library" FORCE)
  set(CAPSTONE_BUILD_TESTS OFF CACHE BOOL "Build tests" FORCE)
  add_subdirectory(third_party/capstone)
  include_directories(third_party/capstone/include)
  set_property(TARGET capstone-static PROPERTY FOLDER "third_party")

  set(protobuf_BUILD_TESTS OFF CACHE BOOL "enable tests for proto2" FORCE)
  set(protobuf_BUILD_SHARED_LIBS OFF CACHE BOOL "enable shared libs for proto2" FORCE)
  add_subdirectory(third_party/protobuf/cmake)
  include_directories(SYSTEM third_party/protobuf/src)

  add_subdirectory(third_party/zlib)
  include_directories(third_party/zlib)
  include_directories(${CMAKE_CURRENT_BINARY_DIR}/third_party/zlib)
  set_property(TARGET example PROPERTY FOLDER "third_party")
  set_property(TARGET minigzip PROPERTY FOLDER "third_party")
  set_property(TARGET zlib PROPERTY FOLDER "third_party")
  set_property(TARGET zlibstatic PROPERTY FOLDER "third_party")
  set_property(TARGET libprotobuf PROPERTY FOLDER "third_party")
  set_property(TARGET libprotobuf-lite PROPERTY FOLDER "third_party")
  set_property(TARGET libprotoc PROPERTY FOLDER "third_party")
  set_property(TARGET protoc PROPERTY FOLDER "third_party")
endif()

include_directories(.)
include_directories(src)
include_directories(third_party/abseil-cpp)
include_directories("${CMAKE_CURRENT_BINARY_DIR}/src")

# Baseline build flags.
if(MSVC)
  set(CMAKE_CXX_FLAGS "/EHsc /wd4018 /D_CRT_SECURE_NO_WARNINGS /DNOMINMAX")
else()
  set(CMAKE_CXX_FLAGS "-W -Wall -Wno-sign-compare")
  set(CMAKE_CXX_FLAGS_DEBUG "-g1")
  set(CMAKE_CXX_FLAGS_RELEASE "-O2")
  set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O2 -g1")
  set_source_files_properties(third_party/demumble/third_party/libcxxabi/cxa_demangle.cpp PROPERTIES COMPILE_FLAGS -Wno-implicit-fallthrough)
endif()

if(APPLE)
elseif(UNIX)
  if(BLOATY_ENABLE_BUILDID)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--build-id")
  endif()
endif()

# When using Ninja, compiler output won't be colorized without this.
include(CheckCXXCompilerFlag)
CHECK_CXX_COMPILER_FLAG(-fdiagnostics-color=always SUPPORTS_COLOR_ALWAYS)
if(SUPPORTS_COLOR_ALWAYS)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fdiagnostics-color=always")
endif()

# Implement ASAN/UBSAN options
if(BLOATY_ENABLE_ASAN)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address")
  set(CMAKE_LINKER_FLAGS_DEBUG "${CMAKE_LINKER_FLAGS_DEBUG} -fsanitize=address")
endif()

if(BLOATY_ENABLE_UBSAN)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=undefined")
  set(CMAKE_LINKER_FLAGS_DEBUG "${CMAKE_LINKER_FLAGS_DEBUG} -fsanitize=undefined")
endif()

if(DEFINED ENV{CXXFLAGS})
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} $ENV{CXXFLAGS}")
endif()

file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/src)
if(PROTOC_FOUND)
add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/src/bloaty.pb.cc
  DEPENDS protoc ${CMAKE_CURRENT_SOURCE_DIR}/src/bloaty.proto
  COMMAND protoc ${CMAKE_CURRENT_SOURCE_DIR}/src/bloaty.proto
      --cpp_out=${CMAKE_CURRENT_BINARY_DIR}/src
      -I${CMAKE_CURRENT_SOURCE_DIR}/src
)
else()
add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/src/bloaty.pb.cc
  COMMAND protoc ${CMAKE_CURRENT_SOURCE_DIR}/src/bloaty.proto
      --cpp_out=${CMAKE_CURRENT_BINARY_DIR}/src
      -I${CMAKE_CURRENT_SOURCE_DIR}/src
)
endif()

file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/src/bloaty_package.bloaty
     DESTINATION ${CMAKE_CURRENT_BINARY_DIR})

add_library(libbloaty STATIC
    src/bloaty.cc
    src/bloaty.h
    src/disassemble.cc
    ${CMAKE_CURRENT_BINARY_DIR}/src/bloaty.pb.cc
    src/dwarf/attr.h
    src/dwarf/attr.cc
    src/dwarf/dwarf_util.cc
    src/dwarf/debug_info.cc
    src/dwarf/line_info.cc
    src/dwarf.cc
    src/dwarf_constants.h
    src/eh_frame.cc
    src/elf.cc
    src/macho.cc
    src/pe.cc
    third_party/lief_pe/pe_structures.h
    src/range_map.cc
    src/range_map.h
    src/re.h
    src/util.cc
    src/util.h
    src/webassembly.cc
    # Until Abseil has a proper CMake build system
    third_party/abseil-cpp/absl/base/internal/raw_logging.cc # Grrrr...
    third_party/abseil-cpp/absl/base/internal/throw_delegate.cc
    third_party/abseil-cpp/absl/debugging/internal/demangle.cc
    third_party/abseil-cpp/absl/numeric/int128.cc
    third_party/abseil-cpp/absl/strings/ascii.cc
    third_party/abseil-cpp/absl/strings/charconv.cc
    third_party/abseil-cpp/absl/strings/escaping.cc
    third_party/abseil-cpp/absl/strings/internal/charconv_bigint.cc
    third_party/abseil-cpp/absl/strings/internal/charconv_parse.cc
    third_party/abseil-cpp/absl/strings/internal/escaping.cc
    third_party/abseil-cpp/absl/strings/internal/memutil.cc
    third_party/abseil-cpp/absl/strings/internal/utf8.cc
    third_party/abseil-cpp/absl/strings/match.cc
    third_party/abseil-cpp/absl/strings/numbers.cc
    third_party/abseil-cpp/absl/strings/str_cat.cc
    third_party/abseil-cpp/absl/strings/string_view.cc
    third_party/abseil-cpp/absl/strings/str_split.cc
    third_party/abseil-cpp/absl/strings/substitute.cc
    third_party/abseil-cpp/absl/types/bad_optional_access.cc
    # One source file, no special build system needed.
    third_party/demumble/third_party/libcxxabi/cxa_demangle.cpp
    )
set_property(TARGET libbloaty PROPERTY FOLDER "bloaty")

if(UNIX)
  set(LIBBLOATY_LIBS libbloaty)
  if(PROTOBUF_FOUND)
    list(APPEND LIBBLOATY_LIBS ${PROTOBUF_LIBRARIES})
  else()
    list(APPEND LIBBLOATY_LIBS libprotoc)
  endif()
  if(BLOATY_ENABLE_RE2)
    if(RE2_FOUND)
      list(APPEND LIBBLOATY_LIBS ${RE2_LIBRARIES})
    else()
      list(APPEND LIBBLOATY_LIBS re2)
    endif()
  endif()
  if(CAPSTONE_FOUND)
    list(APPEND LIBBLOATY_LIBS ${CAPSTONE_LIBRARIES})
  else()
    list(APPEND LIBBLOATY_LIBS capstone-static)
  endif()
  if(ZLIB_FOUND)
    list(APPEND LIBBLOATY_LIBS ZLIB::ZLIB)
  else()
    list(APPEND LIBBLOATY_LIBS zlibstatic)
  endif()
else()
  set(LIBBLOATY_LIBS libbloaty libprotoc capstone-static)
  if(BLOATY_ENABLE_RE2)
    list(APPEND LIBBLOATY_LIBS  re2)
  endif()
  list(APPEND LIBBLOATY_LIBS zlibstatic)
endif()

if(UNIX)
  if(BLOATY_ENABLE_RE2)
    if(RE2_FOUND)
      link_directories(${RE2_LIBRARY_DIRS})
    endif()
  endif()
  if(CAPSTONE_FOUND)
    link_directories(${CAPSTONE_LIBRARY_DIRS})
  endif()
  if(PROTOBUF_FOUND)
    link_directories(${PROTOBUF_LIBRARY_DIRS})
  endif()
endif()

list(APPEND LIBBLOATY_LIBS Threads::Threads)

if(DEFINED ENV{LIB_FUZZING_ENGINE})
  message("LIB_FUZZING_ENGINE set, building fuzz_target instead of Bloaty")
  add_executable(fuzz_target tests/fuzz_target.cc)
  target_link_libraries(fuzz_target ${LIBBLOATY_LIBS} $ENV{LIB_FUZZING_ENGINE})
else()
  add_executable(bloaty src/main.cc)
  target_link_libraries(bloaty ${LIBBLOATY_LIBS})

  set_property(TARGET bloaty PROPERTY FOLDER "bloaty")

  if(BLOATY_ENABLE_CMAKETARGETS)
    install(
      TARGETS bloaty
      EXPORT ${PROJECT_NAME}Targets
      RUNTIME DESTINATION bin
    )
  else()
    install(
      TARGETS bloaty
      RUNTIME DESTINATION bin
    )
  endif()

  if (IS_DIRECTORY "${PROJECT_SOURCE_DIR}/tests")
    enable_testing()

    find_package(Python COMPONENTS Interpreter)
    find_program(LIT_EXECUTABLE NAMES lit-script.py lit.py lit)
    find_program(FILECHECK_EXECUTABLE FileCheck)
    find_program(YAML2OBJ_EXECUTABLE yaml2obj)
    if(Python_FOUND AND LIT_EXECUTABLE AND FILECHECK_EXECUTABLE AND YAML2OBJ_EXECUTABLE)
      set(BLOATY_SRC_DIR ${PROJECT_SOURCE_DIR})
      set(BLOATY_OBJ_DIR ${PROJECT_BINARY_DIR})
      configure_file(tests/lit.site.cfg.in tests/lit.site.cfg @ONLY)

      add_custom_target(check-bloaty
        COMMAND ${Python_EXECUTABLE} ${LIT_EXECUTABLE} -sv ${PROJECT_BINARY_DIR}/tests --param bloaty=$<TARGET_FILE:bloaty>
        DEPENDS
          bloaty
          ${CMAKE_CURRENT_SOURCE_DIR}/tests/lit.cfg
          ${CMAKE_CURRENT_BINARY_DIR}/tests/lit.site.cfg
        COMMENT "Running bloaty tests..."
        USES_TERMINAL)
      set_property(TARGET check-bloaty PROPERTY FOLDER "tests")
    endif()

    if(BUILD_TESTING)
      option(INSTALL_GTEST "" OFF)
      add_subdirectory(third_party/googletest)
      include_directories(third_party/googletest/googletest/include)
      include_directories(third_party/googletest/googlemock/include)

      set(TEST_TARGETS
          bloaty_test
          bloaty_test_pe
          bloaty_misc_test
          range_map_test
          )

      foreach(target ${TEST_TARGETS})
        add_executable(${target} tests/${target}.cc)
        target_link_libraries(${target} ${LIBBLOATY_LIBS} gtest_main gmock)
        set_property(TARGET ${target} PROPERTY FOLDER "tests")
      endforeach(target)

      add_executable(fuzz_test tests/fuzz_target.cc tests/fuzz_driver.cc)
      target_link_libraries(fuzz_test ${LIBBLOATY_LIBS})
      set_property(TARGET fuzz_test PROPERTY FOLDER "tests")

      foreach(testlib gmock gmock_main gtest gtest_main)
        set_property(TARGET ${testlib} PROPERTY FOLDER "tests/libs")
      endforeach(testlib)

      file(GLOB fuzz_corpus tests/testdata/fuzz_corpus/*)

      add_test(NAME range_map_test COMMAND range_map_test)
      add_test(NAME bloaty_test_x86-64 COMMAND bloaty_test WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/tests/testdata/linux-x86_64)
      add_test(NAME bloaty_test_x86 COMMAND bloaty_test WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/tests/testdata/linux-x86)
      add_test(NAME bloaty_test_pe_x64 COMMAND bloaty_test_pe WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/tests/testdata/PE/x64)
      add_test(NAME bloaty_test_pe_x86 COMMAND bloaty_test_pe WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/tests/testdata/PE/x86)
      add_test(NAME bloaty_misc_test COMMAND bloaty_misc_test WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/tests/testdata/misc)
      add_test(NAME fuzz_test COMMAND fuzz_test ${fuzz_corpus} WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/tests/testdata/fuzz_corpus)
    endif()
  endif()

  if(BLOATY_ENABLE_CMAKETARGETS)
    install(EXPORT ${PROJECT_NAME}Targets NAMESPACE ${PROJECT_NAME} DESTINATION lib/${PROJECT_NAME})
  endif()
endif()
