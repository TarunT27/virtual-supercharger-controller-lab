cmake_minimum_required(VERSION 3.20)
project(virtual_supercharger_controller_lab VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ENABLE_COVERAGE "Enable GCC/Clang coverage instrumentation" OFF)
option(ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

include(FetchContent)
FetchContent_Declare(
    json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(json)

find_package(Threads REQUIRED)

function(enable_project_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /permissive-)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
endfunction()

if(ENABLE_COVERAGE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(--coverage -O0 -g)
    add_link_options(--coverage)
endif()

if(ENABLE_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

# Make MinGW test and release executables portable on Windows. This avoids
# requiring libgcc_s_seh-1.dll/libstdc++-6.dll beside every executable.
if(MINGW)
    add_link_options(-static -static-libgcc -static-libstdc++)
endif()

add_library(controller src/controller.cpp)
target_include_directories(controller PUBLIC src)
target_link_libraries(controller PUBLIC Threads::Threads)
enable_project_warnings(controller)

add_library(controller_protocol src/protocol.cpp)
target_include_directories(controller_protocol PUBLIC src)
target_link_libraries(controller_protocol PUBLIC controller nlohmann_json::nlohmann_json)
enable_project_warnings(controller_protocol)

add_library(controller_server src/server.cpp)
target_include_directories(controller_server PUBLIC src)
target_link_libraries(controller_server PUBLIC controller_protocol)
if(WIN32)
    target_link_libraries(controller_server PUBLIC ws2_32)
endif()
enable_project_warnings(controller_server)

add_executable(controller_lab src/main.cpp)
target_link_libraries(controller_lab PRIVATE controller_server)
enable_project_warnings(controller_lab)

include(CTest)
if(BUILD_TESTING)
    add_executable(controller_test tests/controller_test.cpp)
    target_link_libraries(controller_test PRIVATE controller)
    enable_project_warnings(controller_test)
    add_test(NAME controller_unit COMMAND controller_test)
    set_tests_properties(controller_unit PROPERTIES LABELS "unit")

    add_executable(protocol_test tests/protocol_test.cpp)
    target_link_libraries(protocol_test PRIVATE controller_protocol)
    enable_project_warnings(protocol_test)
    add_test(NAME protocol_unit COMMAND protocol_test)
    set_tests_properties(protocol_unit PROPERTIES LABELS "unit")

    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(
        NAME fleet_end_to_end
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/e2e_lab.py
                --binary $<TARGET_FILE:controller_lab>
    )
    set_tests_properties(fleet_end_to_end PROPERTIES LABELS "integration;e2e" TIMEOUT 30)

    add_test(NAME invalid_port_rejected COMMAND controller_lab --port 0)
    set_tests_properties(invalid_port_rejected PROPERTIES LABELS "integration" WILL_FAIL TRUE)
    add_test(NAME non_loopback_host_rejected COMMAND controller_lab --host 0.0.0.0)
    set_tests_properties(non_loopback_host_rejected PROPERTIES LABELS "integration" WILL_FAIL TRUE)
    add_test(NAME malformed_port_rejected COMMAND controller_lab --port 9000oops)
    set_tests_properties(malformed_port_rejected PROPERTIES LABELS "integration" WILL_FAIL TRUE)
endif()

install(TARGETS controller_lab RUNTIME DESTINATION bin)
