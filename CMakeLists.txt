cmake_minimum_required(VERSION 3.28)
project(goblin-store LANGUAGES CXX C VERSION 0.0.3)

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
option(GOBLIN_ENABLE_EXASOCK
       "Build the explicitly selected ExaSock TCP network backend"
       OFF)

# ExaSock is an installed, preload-based Cisco/Exablaze userspace TCP stack.  Keep the
# integration opt-in and consume only its public per-socket header; no ExaSock code is vendored or
# linked into Goblin Store.  The installed `exasock` runner supplies the interposition library at
# process startup.
set(GOBLIN_HAVE_EXASOCK 0)
if(GOBLIN_ENABLE_EXASOCK)
  if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "goblin-store: ExaSock support is Linux-only")
  endif()
  find_path(GOBLIN_EXASOCK_INCLUDE_DIR NAMES exasock/socket.h)
  find_program(GOBLIN_EXASOCK_RUNNER NAMES exasock)
  if(NOT GOBLIN_EXASOCK_INCLUDE_DIR OR
     NOT EXISTS "${GOBLIN_EXASOCK_INCLUDE_DIR}/exasock/extensions.h" OR
     NOT GOBLIN_EXASOCK_RUNNER)
    message(FATAL_ERROR
      "goblin-store: GOBLIN_ENABLE_EXASOCK=ON requires an installed ExaSock public "
      "API (exasock/socket.h and exasock/extensions.h) and the exasock runner")
  endif()
  set(GOBLIN_HAVE_EXASOCK 1)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(_goblin_rdma_default ON)
else()
  set(_goblin_rdma_default OFF)
endif()
option(GOBLIN_ENABLE_RDMA "Build the native libibverbs/librdmacm memcache endpoint"
       ${_goblin_rdma_default})

# ---- native RDMA: optional on Linux; the server source supplies an unsupported stub otherwise ----
set(GOBLIN_HAVE_RDMA 0)
if(GOBLIN_ENABLE_RDMA AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  find_path(GOBLIN_IBVERBS_INCLUDE_DIR NAMES infiniband/verbs.h)
  find_path(GOBLIN_RDMACM_INCLUDE_DIR NAMES rdma/rdma_cma.h)
  find_library(GOBLIN_IBVERBS_LIBRARY NAMES ibverbs)
  find_library(GOBLIN_RDMACM_LIBRARY NAMES rdmacm)
  if(GOBLIN_IBVERBS_INCLUDE_DIR AND GOBLIN_RDMACM_INCLUDE_DIR AND
     GOBLIN_IBVERBS_LIBRARY AND GOBLIN_RDMACM_LIBRARY)
    set(GOBLIN_HAVE_RDMA 1)
  else()
    message(STATUS
      "goblin-store: native RDMA disabled (install libibverbs-dev and librdmacm-dev)")
  endif()
endif()

# ---- liburing (ADR-0002): optional; a stub reactor is used when absent ----
find_package(PkgConfig QUIET)
set(GOBLIN_HAVE_URING 0)
if(PkgConfig_FOUND)
  pkg_check_modules(LIBURING IMPORTED_TARGET liburing)
  if(LIBURING_FOUND)
    set(GOBLIN_HAVE_URING 1)
  endif()
endif()

# ---- OpenSSL (HTTPS/memcache TLS, ADR-0005; kTLS data path, ADR-0011): optional.
# System libssl by default — the kTLS speed win is the kernel's, identical to a vendored build, so
# swapping in a pinned OpenSSL submodule later is a CMake-only change. ----
set(GOBLIN_HAVE_TLS 0)
find_package(OpenSSL 3.0 QUIET)
if(OpenSSL_FOUND)
  set(GOBLIN_HAVE_TLS 1)
endif()

# ---- upstream HTTP(S) mirror client: optional at build time; --mirror diagnoses its absence. ----
# Consume the system libcurl. It supplies DNS, TLS verification, proxy handling, and correct HTTP
# transfer decoding; no HTTP client implementation is vendored into Goblin Store.
set(GOBLIN_HAVE_CURL 0)
find_package(CURL 7.68 QUIET)
if(CURL_FOUND)
  set(GOBLIN_HAVE_CURL 1)
endif()

# ---- core library (everything under src/goblin except main.cpp) ----
# GLOB is used deliberately for a fast-moving scaffold (CONFIGURE_DEPENDS re-globs on build).
file(GLOB_RECURSE GOBLIN_LIB_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/goblin/*.cpp")
list(REMOVE_ITEM GOBLIN_LIB_SOURCES "${CMAKE_SOURCE_DIR}/src/goblin/main.cpp")
list(REMOVE_ITEM GOBLIN_LIB_SOURCES "${CMAKE_SOURCE_DIR}/src/goblin/tools/path_prep.cpp")
list(REMOVE_ITEM GOBLIN_LIB_SOURCES "${CMAKE_SOURCE_DIR}/src/goblin/tools/bench.cpp")

# ---- third-party: picohttpparser (HTTP/1.1 request parsing) — built without our strict warnings ----
add_library(picohttpparser STATIC "${CMAKE_SOURCE_DIR}/third_party/picohttpparser/picohttpparser.c")
target_include_directories(picohttpparser SYSTEM PUBLIC
  "${CMAKE_SOURCE_DIR}/third_party/picohttpparser")

add_library(goblin_core STATIC ${GOBLIN_LIB_SOURCES})
target_include_directories(goblin_core PUBLIC
  "${CMAKE_SOURCE_DIR}/src"
  "${CMAKE_SOURCE_DIR}/python/cpp/include")
target_compile_features(goblin_core PUBLIC cxx_std_23)
target_compile_definitions(goblin_core PUBLIC GOBLIN_HAVE_URING=${GOBLIN_HAVE_URING})
target_compile_definitions(goblin_core PUBLIC GOBLIN_HAVE_TLS=${GOBLIN_HAVE_TLS})
target_compile_definitions(goblin_core PUBLIC GOBLIN_HAVE_RDMA=${GOBLIN_HAVE_RDMA})
target_compile_definitions(goblin_core PUBLIC GOBLIN_HAVE_EXASOCK=${GOBLIN_HAVE_EXASOCK})
target_compile_definitions(goblin_core PUBLIC GOBLIN_HAVE_CURL=${GOBLIN_HAVE_CURL})

set(GOBLIN_WARNINGS -Wall -Wextra -Wpedantic)
if(GOBLIN_WERROR)
  list(APPEND GOBLIN_WARNINGS -Werror)
endif()
target_compile_options(goblin_core PUBLIC ${GOBLIN_WARNINGS})
target_link_libraries(goblin_core PUBLIC picohttpparser)

if(GOBLIN_HAVE_URING)
  target_link_libraries(goblin_core PUBLIC PkgConfig::LIBURING)
endif()

if(GOBLIN_HAVE_TLS)
  target_link_libraries(goblin_core PUBLIC OpenSSL::SSL OpenSSL::Crypto)
endif()

if(GOBLIN_HAVE_CURL)
  target_link_libraries(goblin_core PUBLIC CURL::libcurl)
endif()

if(GOBLIN_HAVE_RDMA)
  target_include_directories(goblin_core SYSTEM PUBLIC
    "${GOBLIN_IBVERBS_INCLUDE_DIR}"
    "${GOBLIN_RDMACM_INCLUDE_DIR}")
  target_link_libraries(goblin_core PUBLIC
    "${GOBLIN_IBVERBS_LIBRARY}"
    "${GOBLIN_RDMACM_LIBRARY}")
endif()

if(GOBLIN_HAVE_EXASOCK)
  target_include_directories(goblin_core SYSTEM PRIVATE "${GOBLIN_EXASOCK_INCLUDE_DIR}")
  # Extension symbols are supplied only by the exasock preloader.  Resolve them from the process
  # at runtime; deliberately do not create a libexasock_ext DT_NEEDED dependency.
  target_link_libraries(goblin_core PUBLIC ${CMAKE_DL_LIBS})
endif()

if(GOBLIN_SANITIZE)
  target_compile_options(goblin_core PUBLIC
    $<$<CONFIG:Debug>:-fsanitize=address,undefined;-fno-omit-frame-pointer>)
  target_link_options(goblin_core PUBLIC
    $<$<CONFIG:Debug>:-fsanitize=address,undefined>)
endif()

# ---- main executable ----
add_executable(goblin-store "${CMAKE_SOURCE_DIR}/src/goblin/main.cpp")
target_link_libraries(goblin-store PRIVATE goblin_core)

# CLI helper that blesses a directory as a Goblin pool store (ADR-0013).
add_executable(goblin-store-path-prep "${CMAKE_SOURCE_DIR}/src/goblin/tools/path_prep.cpp")
target_link_libraries(goblin-store-path-prep PRIVATE goblin_core)

# Load generator for large-object GET throughput/latency (memcache + HTTP); pure socket client.
find_package(Threads REQUIRED)
add_executable(goblin-bench "${CMAKE_SOURCE_DIR}/src/goblin/tools/bench.cpp")
target_compile_features(goblin-bench PRIVATE cxx_std_23)
target_link_libraries(goblin-bench PRIVATE Threads::Threads)

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

message(STATUS "goblin-store ${PROJECT_VERSION}  io_uring=${GOBLIN_HAVE_URING}  rdma=${GOBLIN_HAVE_RDMA}  exasock=${GOBLIN_HAVE_EXASOCK}  mirror=${GOBLIN_HAVE_CURL}  build=${CMAKE_BUILD_TYPE}  werror=${GOBLIN_WERROR}")
