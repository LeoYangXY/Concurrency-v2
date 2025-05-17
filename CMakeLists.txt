cmake_minimum_required(VERSION 3.12)
project(Concurrency-v2)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)



add_executable(UnitTest Unit_Test.cpp "CentralCache_LockFree.h")
target_link_libraries(UnitTest PRIVATE Threads::Threads)

add_executable(PerformanceTest Performance_Test.cpp "CentralCache_LockFree.h")
target_link_libraries(PerformanceTest PRIVATE Threads::Threads)

include_directories(${PROJECT_SOURCE_DIR}/include)