# TPMExtractor fork - CMake build. Copyright 2026 the TPMExtractor fork contributors.
# Licensed under the Apache License, Version 2.0 (see LICENSE). New file in the fork (see CHANGES.md).
#
#   cmake -S . -B build/cmake -DCMAKE_BUILD_TYPE=Release && cmake --build build/cmake
#   cmake -S . -B build/asan -DTPMX_SANITIZE=ON            (ASan + UBSan, every target)
#   cmake -S . -B build/mingw64 -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw64.cmake
#   ctest --test-dir build/cmake --output-on-failure        (unit and end-to-end tests)
#
# Without CMake: the plain Makefile builds the same targets (`make`, `make check`).
# Visual Studio: TPMExtractor.sln / TPMExtractor.vcxproj.

cmake_minimum_required(VERSION 3.16)
project(TPMExtractor VERSION 0.1.0 LANGUAGES C CXX)

option(TPMX_SANITIZE "Build every target with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(TPMX_WERROR "Treat compiler warnings as errors" OFF)
option(TPMX_FUZZ "Build the fuzz targets tests/fuzz_*.cpp (with clang: libFuzzer; see tests/CMakeLists.txt)" OFF)
option(TPMX_FUZZ_REPLAY "Link the fuzz targets with the replay driver even with clang" OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------------------------
# Flags for every target in this directory and below (tests/ included).
if(MSVC)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS _MBCS)
  add_compile_options(/W3)
  if(TPMX_WERROR)
    add_compile_options(/WX)
  endif()
  if(TPMX_SANITIZE)
    add_compile_options(/fsanitize=address)  # MSVC has no UBSan
  endif()
else()
  # -ffp-contract=off: no fused multiply-add, so float results (and the %f text written from
  # them) are the same on every compiler and CPU, as with MSVC's default /fp:precise.
  add_compile_options(-Wall -Wextra -ffp-contract=off)
  if(TPMX_WERROR)
    add_compile_options(-Werror)
  endif()
  # 32-bit x86: SSE2 single-precision arithmetic like MSVC's /arch:SSE2, not x87 extended precision.
  if(CMAKE_SIZEOF_VOID_P EQUAL 4 AND CMAKE_SYSTEM_PROCESSOR MATCHES "^([iI][3-6]86|x86|X86)$")
    add_compile_options(-msse2 -mfpmath=sse)
  endif()
  if(TPMX_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
  endif()
  # libFuzzer steers by the coverage of the code under test, so every target is instrumented,
  # not only the fuzz targets (which link with -fsanitize=fuzzer, tests/CMakeLists.txt).
  if(TPMX_FUZZ AND CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND NOT TPMX_FUZZ_REPLAY)
    add_compile_options($<$<COMPILE_LANG_AND_ID:CXX,Clang,AppleClang>:-fsanitize=fuzzer-no-link>
                        $<$<COMPILE_LANG_AND_ID:C,Clang,AppleClang>:-fsanitize=fuzzer-no-link>)
    add_link_options(-fsanitize=fuzzer-no-link)
  endif()
  if(MINGW)
    add_link_options(-static)  # no libstdc++/libgcc DLLs next to the .exe
  endif()
endif()

# ---------------------------------------------------------------------------------------------
# unshieldv3 (vendored, Apache-2.0, see unshieldv3/LICENSE): InstallShield v3 (.Z) archives.
add_library(unshieldv3 STATIC
  unshieldv3/blast.c
  unshieldv3/blast.h
  unshieldv3/ISArchiveV3.cpp
  unshieldv3/ISArchiveV3.h
  unshieldv3/unshieldv3.cpp
  unshieldv3/unshieldv3.h)
target_include_directories(unshieldv3 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# The vendored code is kept as upstream wrote it (std::tm aggregate initialisation). Its header
# checks are reported errors in the fork, not assert()s, so NDEBUG builds are fine.
if(NOT MSVC)
  target_compile_options(unshieldv3 PRIVATE -Wno-missing-field-initializers)
endif()

# ---------------------------------------------------------------------------------------------
# tpmx_core: every top-level .cpp except main.cpp. Globbed so that new files build without
# editing this file; TPMExtractor.vcxproj lists files explicitly and must be updated by hand
# (`make check-project-files` checks it).
file(GLOB TPMX_CORE_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
list(REMOVE_ITEM TPMX_CORE_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp)
file(GLOB TPMX_CORE_HEADERS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.h)
add_library(tpmx_core STATIC ${TPMX_CORE_SOURCES} ${TPMX_CORE_HEADERS})
target_include_directories(tpmx_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(tpmx_core PUBLIC unshieldv3)

# ---------------------------------------------------------------------------------------------
add_executable(tpmextractor main.cpp)
target_link_libraries(tpmextractor PRIVATE tpmx_core)
install(TARGETS tpmextractor RUNTIME DESTINATION bin)

# ---------------------------------------------------------------------------------------------
# Tests (tests/CMakeLists.txt). Each tests/unit_*.cpp is one program (own main) linked with
# tpmx_core; it gets TPMX_EXE, TPMX_SOURCE_DIR and TPMX_TEST_TMP in the environment and exits 0
# (pass) or 77 (skipped); anything else fails. tests/run_tests.py runs the tool end to end
# (e2e_synthetic always, e2e_real with TPM_DISC set). The Makefile's `make check` does the same.
enable_testing()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt)
  add_subdirectory(tests)
endif()
