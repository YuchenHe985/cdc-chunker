cmake_minimum_required(VERSION 3.16)
project(cdc-chunker VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(CDC_WERROR "Treat warnings as errors" OFF)
option(CDC_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

add_library(cdc STATIC src/common.cpp src/gear.cpp src/rabin.cpp)
target_include_directories(cdc PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(cdc PUBLIC cxx_std_17)

set(CDC_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
if(CDC_WERROR)
  list(APPEND CDC_WARNINGS -Werror)
endif()
if(CDC_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

foreach(t cdc)
  target_compile_options(${t} PRIVATE ${CDC_WARNINGS})
endforeach()

install(TARGETS cdc ARCHIVE DESTINATION lib)
install(DIRECTORY include/ DESTINATION include)
