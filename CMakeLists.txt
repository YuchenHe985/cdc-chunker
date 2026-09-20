cmake_minimum_required(VERSION 3.16)
project(cdc-chunker VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(CDC_WERROR "Treat warnings as errors" OFF)
option(CDC_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(CDC_TSAN "Build with ThreadSanitizer" OFF)

add_library(cdc STATIC src/common.cpp src/gear.cpp src/rabin.cpp src/parallel.cpp)
target_include_directories(cdc PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(cdc PUBLIC cxx_std_17)
find_package(Threads REQUIRED)
target_link_libraries(cdc PUBLIC Threads::Threads)

set(CDC_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
if(CDC_WERROR)
  list(APPEND CDC_WARNINGS -Werror)
endif()
if(CDC_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()
if(CDC_TSAN)
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
  add_link_options(-fsanitize=thread)
endif()

add_executable(cdc_cli tools/cdc_cli.cpp)
set_target_properties(cdc_cli PROPERTIES OUTPUT_NAME cdc)
target_link_libraries(cdc_cli PRIVATE cdc)

add_executable(cdc_bench bench/bench.cpp)
target_link_libraries(cdc_bench PRIVATE cdc)

add_executable(cdc_example examples/stream.cpp)
target_link_libraries(cdc_example PRIVATE cdc)

add_executable(cdc_tests
  tests/test_main.cpp
  tests/test_params.cpp
  tests/test_hash.cpp
  tests/test_chunkers.cpp
  tests/test_dedup.cpp
  tests/test_golden.cpp)
target_link_libraries(cdc_tests PRIVATE cdc)

foreach(t cdc cdc_cli cdc_bench cdc_example cdc_tests)
  target_compile_options(${t} PRIVATE ${CDC_WARNINGS})
endforeach()

enable_testing()
add_test(NAME unit COMMAND cdc_tests)

find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
  add_test(NAME cross_check
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/cross_check.py --cli $<TARGET_FILE:cdc_cli>)
endif()

install(TARGETS cdc cdc_cli ARCHIVE DESTINATION lib RUNTIME DESTINATION bin)
install(DIRECTORY include/ DESTINATION include)
