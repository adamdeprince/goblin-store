cmake_minimum_required(VERSION 3.28)
project(goblincache LANGUAGES CXX VERSION 0.0.1)

# ---- Standard (ADR-0004): C++23, no extensions, modules off for now ----
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(GOBLIN_WERROR   "Treat warnings as errors"        ON)
option(GOBLIN_SANITIZE "ASan/UBSan in Debug builds"      ON)

# ---- liburing (ADR-0002): optional; a stub reactor is used when absent ----
find_package(PkgConfig QUIET)
set(GOBLIN_HAVE_URING 0)
if(PkgConfig_FOUND)
  pkg_check_modules(LIBURING IMPORTED_TARGET liburing)
  if(LIBURING_FOUND)
    set(GOBLIN_HAVE_URING 1)
  endif()
endif()

# ---- core library (everything under src/goblin except main.cpp) ----
# GLOB is used deliberately for a fast-moving scaffold (CONFIGURE_DEPENDS re-globs on build).
file(GLOB_RECURSE GOBLIN_LIB_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/goblin/*.cpp")
list(REMOVE_ITEM GOBLIN_LIB_SOURCES "${CMAKE_SOURCE_DIR}/src/goblin/main.cpp")
list(REMOVE_ITEM GOBLIN_LIB_SOURCES "${CMAKE_SOURCE_DIR}/src/goblin/tools/path_prep.cpp")

add_library(goblin_core STATIC ${GOBLIN_LIB_SOURCES})
target_include_directories(goblin_core PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_compile_features(goblin_core PUBLIC cxx_std_23)
target_compile_definitions(goblin_core PUBLIC GOBLIN_HAVE_URING=${GOBLIN_HAVE_URING})

set(GOBLIN_WARNINGS -Wall -Wextra -Wpedantic)
if(GOBLIN_WERROR)
  list(APPEND GOBLIN_WARNINGS -Werror)
endif()
target_compile_options(goblin_core PUBLIC ${GOBLIN_WARNINGS})

if(GOBLIN_HAVE_URING)
  target_link_libraries(goblin_core PUBLIC PkgConfig::LIBURING)
endif()

if(GOBLIN_SANITIZE)
  target_compile_options(goblin_core PUBLIC
    $<$<CONFIG:Debug>:-fsanitize=address,undefined;-fno-omit-frame-pointer>)
  target_link_options(goblin_core PUBLIC
    $<$<CONFIG:Debug>:-fsanitize=address,undefined>)
endif()

# ---- main executable ----
add_executable(goblincache "${CMAKE_SOURCE_DIR}/src/goblin/main.cpp")
target_link_libraries(goblincache PRIVATE goblin_core)

# CLI helper that blesses a directory as a Goblin pool store (ADR-0013).
add_executable(goblin-store-path-prep "${CMAKE_SOURCE_DIR}/src/goblin/tools/path_prep.cpp")
target_link_libraries(goblin-store-path-prep PRIVATE goblin_core)

# ---- tests (dependency-free; see tests/mini_test.hpp) ----
include(CTest)
if(BUILD_TESTING)
  file(GLOB GOBLIN_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/tests/*.cpp")
  if(GOBLIN_TEST_SOURCES)
    add_executable(goblin_tests ${GOBLIN_TEST_SOURCES})
    target_include_directories(goblin_tests PRIVATE "${CMAKE_SOURCE_DIR}/tests")
    target_link_libraries(goblin_tests PRIVATE goblin_core)
    add_test(NAME goblin_tests COMMAND goblin_tests)
  endif()
endif()

message(STATUS "goblincache ${PROJECT_VERSION}  io_uring=${GOBLIN_HAVE_URING}  build=${CMAKE_BUILD_TYPE}  werror=${GOBLIN_WERROR}")
