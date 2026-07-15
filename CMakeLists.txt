cmake_minimum_required(VERSION 3.20)
project(virtual_supercharger_controller_lab LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(controller src/controller.cpp)
target_include_directories(controller PUBLIC src)

add_executable(controller_lab src/main.cpp)
target_link_libraries(controller_lab PRIVATE controller)

enable_testing()
add_executable(controller_test tests/controller_test.cpp)
target_link_libraries(controller_test PRIVATE controller)
add_test(NAME controller_test COMMAND controller_test)
