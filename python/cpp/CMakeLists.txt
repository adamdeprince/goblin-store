cmake_minimum_required(VERSION 3.22)
project(GoblinStoreClient VERSION 0.1.0 LANGUAGES CXX)

include(CMakePackageConfigHelpers)
include(GNUInstallDirs)
find_package(Threads REQUIRED)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(_rdma_default ON)
else()
  set(_rdma_default OFF)
endif()
option(GOBLIN_STORE_CLIENT_ENABLE_RDMA "Build the native libibverbs/librdmacm transport" ${_rdma_default})
option(GOBLIN_STORE_CLIENT_INSTALL "Install the C++ library, headers, and package metadata" ${PROJECT_IS_TOP_LEVEL})
option(GOBLIN_STORE_CLIENT_BUILD_TESTS "Build hardware-free C++ client tests" ${PROJECT_IS_TOP_LEVEL})
option(GOBLIN_STORE_CLIENT_BUILD_BENCHMARKS "Build the native RDMA benchmark tools" ${PROJECT_IS_TOP_LEVEL})

set(_client_sources src/client.cpp)
if(GOBLIN_STORE_CLIENT_ENABLE_RDMA)
  if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "Goblin Store's native RDMA transport is Linux-only")
  endif()
  find_path(GOBLIN_IBVERBS_INCLUDE_DIR NAMES infiniband/verbs.h)
  find_path(GOBLIN_RDMACM_INCLUDE_DIR NAMES rdma/rdma_cma.h)
  find_library(GOBLIN_IBVERBS_LIBRARY NAMES ibverbs)
  find_library(GOBLIN_RDMACM_LIBRARY NAMES rdmacm)
  if(NOT GOBLIN_IBVERBS_INCLUDE_DIR OR NOT GOBLIN_RDMACM_INCLUDE_DIR OR
     NOT GOBLIN_IBVERBS_LIBRARY OR NOT GOBLIN_RDMACM_LIBRARY)
    message(FATAL_ERROR
      "Native RDMA needs libibverbs and librdmacm development files. "
      "On Ubuntu: apt install libibverbs-dev librdmacm-dev")
  endif()
  list(APPEND _client_sources src/rdma_transport.cpp)
else()
  list(APPEND _client_sources src/rdma_unavailable.cpp)
endif()

add_library(goblin_memcache_client ${_client_sources})
add_library(GoblinStoreClient::client ALIAS goblin_memcache_client)
set_target_properties(goblin_memcache_client PROPERTIES
  EXPORT_NAME client
  OUTPUT_NAME goblin-store-client
  POSITION_INDEPENDENT_CODE ON
  VERSION ${PROJECT_VERSION}
  SOVERSION 0)
target_compile_features(goblin_memcache_client PUBLIC cxx_std_20)
target_include_directories(goblin_memcache_client PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_options(goblin_memcache_client PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(goblin_memcache_client PUBLIC Threads::Threads)
if(GOBLIN_STORE_CLIENT_ENABLE_RDMA)
  target_include_directories(goblin_memcache_client PRIVATE
    ${GOBLIN_IBVERBS_INCLUDE_DIR} ${GOBLIN_RDMACM_INCLUDE_DIR})
  # Build-tree users get the discovered absolute libraries. Installed static
  # consumers retain ordinary -libverbs/-lrdmacm transitive dependencies.
  target_link_libraries(goblin_memcache_client PUBLIC
    $<BUILD_INTERFACE:${GOBLIN_IBVERBS_LIBRARY}>
    $<BUILD_INTERFACE:${GOBLIN_RDMACM_LIBRARY}>
    $<INSTALL_INTERFACE:ibverbs>
    $<INSTALL_INTERFACE:rdmacm>)
endif()

if(GOBLIN_STORE_CLIENT_BUILD_TESTS)
  enable_testing()
  add_executable(goblin_store_client_tests tests/test_client.cpp)
  target_link_libraries(goblin_store_client_tests PRIVATE GoblinStoreClient::client)
  add_test(NAME goblin_store_client_tests COMMAND goblin_store_client_tests)

  add_executable(goblin_store_rdma_wire_tests tests/test_rdma_wire.cpp)
  target_link_libraries(goblin_store_rdma_wire_tests PRIVATE GoblinStoreClient::client)
  add_test(NAME goblin_store_rdma_wire_tests COMMAND goblin_store_rdma_wire_tests)
endif()

if(GOBLIN_STORE_CLIENT_BUILD_BENCHMARKS)
  add_executable(goblin_store_rdma_benchmark tools/rdma_benchmark.cpp)
  target_link_libraries(goblin_store_rdma_benchmark PRIVATE GoblinStoreClient::client)
  target_compile_options(goblin_store_rdma_benchmark PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(GOBLIN_STORE_CLIENT_INSTALL)
  install(TARGETS goblin_memcache_client EXPORT GoblinStoreClientTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
  install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
  install(EXPORT GoblinStoreClientTargets
    FILE GoblinStoreClientTargets.cmake
    NAMESPACE GoblinStoreClient::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/GoblinStoreClient)

  configure_package_config_file(
    cmake/GoblinStoreClientConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/GoblinStoreClientConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/GoblinStoreClient)
  write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/GoblinStoreClientConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)
  install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/GoblinStoreClientConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/GoblinStoreClientConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/GoblinStoreClient)

  if(GOBLIN_STORE_CLIENT_ENABLE_RDMA)
    set(GOBLIN_STORE_CLIENT_PC_REQUIRES "libibverbs librdmacm")
  else()
    set(GOBLIN_STORE_CLIENT_PC_REQUIRES "")
  endif()
  set(GOBLIN_STORE_CLIENT_PC_PREFIX "${CMAKE_INSTALL_PREFIX}")
  if(NOT IS_ABSOLUTE "${CMAKE_INSTALL_LIBDIR}")
    file(RELATIVE_PATH GOBLIN_STORE_CLIENT_PC_PREFIX_FROM_PCFILEDIR
      "/${CMAKE_INSTALL_LIBDIR}/pkgconfig" "/")
    set(GOBLIN_STORE_CLIENT_PC_PREFIX
      "\${pcfiledir}/${GOBLIN_STORE_CLIENT_PC_PREFIX_FROM_PCFILEDIR}")
  endif()
  if(IS_ABSOLUTE "${CMAKE_INSTALL_LIBDIR}")
    set(GOBLIN_STORE_CLIENT_PC_LIBDIR "${CMAKE_INSTALL_LIBDIR}")
  else()
    set(GOBLIN_STORE_CLIENT_PC_LIBDIR "\${prefix}/${CMAKE_INSTALL_LIBDIR}")
  endif()
  if(IS_ABSOLUTE "${CMAKE_INSTALL_INCLUDEDIR}")
    set(GOBLIN_STORE_CLIENT_PC_INCLUDEDIR "${CMAKE_INSTALL_INCLUDEDIR}")
  else()
    set(GOBLIN_STORE_CLIENT_PC_INCLUDEDIR "\${prefix}/${CMAKE_INSTALL_INCLUDEDIR}")
  endif()
  configure_file(cmake/goblin-store-client.pc.in
    ${CMAKE_CURRENT_BINARY_DIR}/goblin-store-client.pc @ONLY)
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/goblin-store-client.pc
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
endif()

message(STATUS "GoblinStoreClient ${PROJECT_VERSION}: native RDMA=${GOBLIN_STORE_CLIENT_ENABLE_RDMA}")
