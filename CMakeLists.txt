
cmake_minimum_required (VERSION 2.6)
project (Bloaty)

set(RE2_BUILD_TESTING OFF CACHE BOOL "enable testing for RE2" FORCE)
add_subdirectory(third_party/re2)
include_directories(third_party/re2)

include_directories(.)
add_executable(bloaty
    src/bloaty.cc
    src/dwarf.cc
    src/elf.cc
    src/macho.cc
    src/main.cc
    )
target_link_libraries(bloaty re2)
set(CMAKE_CXX_FLAGS "-std=c++11")

