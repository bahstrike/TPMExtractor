# TPMExtractor fork - tests. Copyright 2026 the TPMExtractor fork contributors. Licensed under the
# Apache License, Version 2.0 (see LICENSE). New file in the fork (see CHANGES.md).
#
# Two kinds of tests, both run by ctest (and by the plain Makefile's `make check`):
#   unit_<name>.cpp   one program each (own main), linked with tpmx_core; exit 0 pass, 77 skip.
#   run_tests.py      end-to-end runs of the tpmextractor executable on synthetic fixtures
#                     (make_fixtures.py), and on a real disc when TPM_DISC is set (skipped
#                     otherwise). Needs Python 3 (standard library only); skipped without it.
# With -DTPMX_FUZZ=ON also short runs of the fuzz targets (label "fuzz"; see the end of this file).
# Every test gets TPMX_EXE, TPMX_SOURCE_DIR and TPMX_TEST_TMP (a folder of its own) in its
# environment. Optional, from the calling environment: TPM_DISC, TPM_REF_MANIFEST,
# TPM_REF_UNDEFINED, TPM_WORK (real data), TPMX_REF_EXE and TPMX_REF_B3D_DECOMPRESS (builds of the
# original tool and decoder, compared with when given).
#
#   ctest --test-dir build/<dir> --output-on-failure
#   ctest --test-dir build/<dir> -R e2e_synthetic -V

file(GLOB TEST_SUPPORT CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/support/*.cpp)
file(GLOB UNIT_TESTS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/unit_*.cpp)

set(TPMX_TESTS_TMP ${CMAKE_CURRENT_BINARY_DIR}/tmp)

foreach(src ${UNIT_TESTS})
  get_filename_component(name ${src} NAME_WE)
  add_executable(${name} ${src} ${TEST_SUPPORT})
  target_link_libraries(${name} PRIVATE tpmx_core)
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  set(tmp ${TPMX_TESTS_TMP}/${name})
  file(MAKE_DIRECTORY ${tmp})
  add_test(NAME ${name} COMMAND ${name} WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  set_tests_properties(${name} PROPERTIES SKIP_RETURN_CODE 77
    ENVIRONMENT "TPMX_EXE=$<TARGET_FILE:tpmextractor>;TPMX_SOURCE_DIR=${CMAKE_SOURCE_DIR};TPMX_TEST_TMP=${tmp}")
endforeach()

# ---------------------------------------------------------------------------------------------
# End-to-end runner. The unit_b3d_decompress program doubles as its decoder helper.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  set(helper_args)
  if(TARGET unit_b3d_decompress)
    set(helper_args --helper $<TARGET_FILE:unit_b3d_decompress>)
  endif()
  foreach(suite synthetic real)
    set(tmp ${TPMX_TESTS_TMP}/e2e_${suite})
    file(MAKE_DIRECTORY ${tmp})
    add_test(NAME e2e_${suite}
      COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/run_tests.py --suite ${suite}
              --exe $<TARGET_FILE:tpmextractor> ${helper_args}
      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    set_tests_properties(e2e_${suite} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 7200
      ENVIRONMENT "TPMX_EXE=$<TARGET_FILE:tpmextractor>;TPMX_SOURCE_DIR=${CMAKE_SOURCE_DIR};TPMX_TEST_TMP=${tmp};PYTHONDONTWRITEBYTECODE=1")
  endforeach()
else()
  message(STATUS "tests: Python 3 not found; the end-to-end tests (tests/run_tests.py) are not registered")
endif()

# ---------------------------------------------------------------------------------------------
# Fuzz targets: each tests/fuzz_<name>.cpp defines LLVMFuzzerTestOneInput.
#   in memory:   fuzz_lab (LAB directory), fuzz_b3d (level section walk, LZSS), fuzz_b3d_geom
#                (level geometry decoder and OBJ/MTL text), fuzz_cmp (CMP)
#   file level:  fuzz_baf (process_baf: BAF parsers and every BAF writer, --legacy and default
#                mode), fuzz_b3d_file (process_b3d, with the geometry files), fuzz_lab_file
#                (extract_lab), fuzz_z (extract_z). These run in a scratch folder of their own and
#                abort when anything is written outside the output folder (tests/fuzz/sandbox.h).
#                Set TPMX_FUZZ_TMP (or TMPDIR) to a RAM disk such as /dev/shm for speed. fuzz_baf
#                builds a full CMP for every input with a colormap: tens of inputs per second
#                under ASan, so give it several times the others' time.
# With clang they link with libFuzzer, and the top-level CMakeLists.txt (which defines TPMX_FUZZ
# and TPMX_FUZZ_REPLAY) instruments every target for coverage (-fsanitize=fuzzer-no-link), so
# libFuzzer is guided by the tool's own code; with other compilers (or TPMX_FUZZ_REPLAY=ON) they
# link with tests/fuzz/replay_main.cpp, which runs them on files plus truncations and seeded
# mutations. Configure with -DTPMX_SANITIZE=ON as well. Seed inputs: make_fixtures.py OUTDIR
# writes OUTDIR/fuzz/<name>/ (<name> without "fuzz_"). With Python 3, ctest runs every target
# briefly on those seeds (label "fuzz": TPMX_FUZZ_SMOKE_RUNS inputs with libFuzzer, a few
# truncations and mutations of each seed with the replay driver).
#   cmake -S . -B build/fuzz -DTPMX_FUZZ=ON -DTPMX_SANITIZE=ON && cmake --build build/fuzz
#   ctest --test-dir build/fuzz -L fuzz --output-on-failure
#   python3 -I tests/make_fixtures.py build/fuzz/fixtures
#   build/fuzz/tests/fuzz_baf -max_total_time=600 corpus/baf build/fuzz/fixtures/fuzz/baf    (libFuzzer)
#   build/fuzz/tests/fuzz_b3d -mutations 2000 build/fuzz/fixtures/fuzz/b3d                    (replay driver)
set(TPMX_FUZZ_SMOKE_RUNS 600 CACHE STRING "Inputs each fuzz target runs in its ctest smoke test (libFuzzer)")
if(TPMX_FUZZ)
  file(GLOB FUZZ_TARGETS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/fuzz_*.cpp)
  set(use_libfuzzer OFF)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND NOT TPMX_FUZZ_REPLAY)
    set(use_libfuzzer ON)
  endif()
  foreach(src ${FUZZ_TARGETS})
    get_filename_component(name ${src} NAME_WE)
    if(use_libfuzzer)
      add_executable(${name} ${src})
      target_compile_options(${name} PRIVATE -fsanitize=fuzzer)
      target_link_options(${name} PRIVATE -fsanitize=fuzzer)
    else()
      add_executable(${name} ${src} ${CMAKE_CURRENT_SOURCE_DIR}/fuzz/replay_main.cpp)
    endif()
    target_link_libraries(${name} PRIVATE tpmx_core)
  endforeach()

  # Smoke tests: the seeds, then a short deterministic run of every target on them.
  if(Python3_Interpreter_FOUND)
    set(seeds ${TPMX_TESTS_TMP}/fuzz_seeds)
    add_test(NAME fuzz_seeds
      COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/make_fixtures.py ${seeds}
      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    set_tests_properties(fuzz_seeds PROPERTIES FIXTURES_SETUP tpmx_fuzz_seeds LABELS fuzz
      ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
    foreach(src ${FUZZ_TARGETS})
      get_filename_component(name ${src} NAME_WE)
      string(REGEX REPLACE "^fuzz_" "" corpus ${name})
      set(tmp ${TPMX_TESTS_TMP}/${name})
      file(MAKE_DIRECTORY ${tmp}/new)
      if(use_libfuzzer)
        # new inputs go to ${tmp}/new, never into the seeds; crash files to ${tmp}
        set(cmd $<TARGET_FILE:${name}> -runs=${TPMX_FUZZ_SMOKE_RUNS} -seed=1 -timeout=60 -rss_limit_mb=2048
                -artifact_prefix=${tmp}/ ${tmp}/new ${seeds}/fuzz/${corpus})
      else()
        set(cmd $<TARGET_FILE:${name}> -truncations 4 -mutations 4 -seed 1 ${seeds}/fuzz/${corpus})
      endif()
      add_test(NAME ${name}_smoke COMMAND ${cmd} WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
      set_tests_properties(${name}_smoke PROPERTIES FIXTURES_REQUIRED tpmx_fuzz_seeds LABELS fuzz TIMEOUT 1800)
    endforeach()
  endif()
endif()
