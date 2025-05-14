cmake_minimum_required(VERSION 3.12)
project(Concurrency-v2)

# 设置 C++20 标准
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 只需要编译 Test.cpp，其他头文件会自动包含
# 构建 UnitTest 可执行文件
add_executable(UnitTest Unit_Test.cpp)

# 构建 PerformanceTest 可执行文件
add_executable(PerformanceTest Performance_Test.cpp)

# 如果你的头文件在其他目录，可以添加包含路径
# include_directories(include)