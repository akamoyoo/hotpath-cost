cmake_minimum_required(VERSION 3.20)
project(llvm_fn_effects C CXX)   # C only because LLVMConfig.cmake probes with it

# Build against an LLVM installation: pass -DLLVM_DIR=<prefix>/lib/cmake/llvm, or have llvm-config on PATH.
if(NOT LLVM_DIR)
  find_program(LLVM_CONFIG_EXE llvm-config)
  if(LLVM_CONFIG_EXE)
    execute_process(COMMAND ${LLVM_CONFIG_EXE} --cmakedir OUTPUT_VARIABLE LLVM_DIR OUTPUT_STRIP_TRAILING_WHITESPACE)
  endif()
endif()
find_package(LLVM REQUIRED CONFIG)
message(STATUS "LLVM ${LLVM_PACKAGE_VERSION} at ${LLVM_DIR}")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

include_directories(SYSTEM ${LLVM_INCLUDE_DIRS})
separate_arguments(LLVM_DEFS_LIST NATIVE_COMMAND ${LLVM_DEFINITIONS})
add_definitions(${LLVM_DEFS_LIST})
if(NOT LLVM_ENABLE_RTTI)
  add_compile_options(-fno-rtti)
endif()

add_library(FnEffects MODULE src/FnEffects.cpp src/PointsTo.cpp)
set_target_properties(FnEffects PROPERTIES PREFIX "" CXX_VISIBILITY_PRESET default)
target_compile_options(FnEffects PRIVATE -Wall -Wno-unused-parameter)
