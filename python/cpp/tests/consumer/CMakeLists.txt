cmake_minimum_required(VERSION 3.22)
project(GoblinStoreClientConsumer LANGUAGES CXX)

find_package(GoblinStoreClient CONFIG REQUIRED)
add_executable(goblin_store_client_consumer main.cpp)
target_link_libraries(goblin_store_client_consumer PRIVATE GoblinStoreClient::client)
