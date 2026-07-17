cmake_minimum_required(VERSION 3.22)
project(goblin_store_python VERSION 0.1.0 LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR
    "goblin-store-client's native network transports build only on Linux. "
    "Install the requested system transport SDKs and python3-dev first.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Python 3.10 REQUIRED COMPONENTS Interpreter Development.Module)
execute_process(
  COMMAND "${Python_EXECUTABLE}" -c "import nanobind; print(nanobind.cmake_dir())"
  OUTPUT_VARIABLE NANOBIND_CMAKE_DIR
  OUTPUT_STRIP_TRAILING_WHITESPACE
  RESULT_VARIABLE NANOBIND_RESULT)
if(NOT NANOBIND_RESULT EQUAL 0)
  message(FATAL_ERROR
    "nanobind was not found in ${Python_EXECUTABLE}; install nanobind>=2.12")
endif()
list(APPEND CMAKE_PREFIX_PATH "${NANOBIND_CMAKE_DIR}")
find_package(nanobind CONFIG REQUIRED)

# The wheel embeds the standalone client library into the extension. Its own
# install/export rules remain available when configuring python/cpp directly.
set(BUILD_SHARED_LIBS OFF)
# python/cpp supplies the platform defaults. Explicit command-line choices for
# RDMA and ExaSock must remain effective, including ExaSock-only local wheels.
set(GOBLIN_STORE_CLIENT_INSTALL OFF CACHE BOOL "" FORCE)
set(GOBLIN_STORE_CLIENT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(cpp)

nanobind_add_module(_goblin_store src/bindings.cpp)
target_compile_features(_goblin_store PRIVATE cxx_std_20)
target_compile_options(_goblin_store PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(_goblin_store PRIVATE GoblinStoreClient::client)

install(TARGETS _goblin_store LIBRARY DESTINATION goblin_store)
