# TPMExtractor fork - plain GNU Makefile (no CMake needed). Copyright 2026 the TPMExtractor fork
# contributors. Licensed under the Apache License, Version 2.0 (see LICENSE). New file in the
# fork (see CHANGES.md). Builds the same targets as CMakeLists.txt.
#
#   make                      build/<target-triple>/tpmextractor
#   make SANITIZE=1           ASan + UBSan build (in build/<target-triple>-asan)
#   make WERROR=1             warnings are errors
#   make tests                build the test programs only (e.g. in a cross build)
#   make check                build and run every tests/unit_*.cpp (exit 0 pass, 77 skip), then
#                             the end-to-end tests tests/run_tests.py (skipped without Python 3)
#   make CXX=x86_64-w64-mingw32-g++ CC=x86_64-w64-mingw32-gcc     Windows .exe (static)
#   make check-project-files  every source is listed in TPMExtractor.vcxproj(.filters)
#   make clean                remove this configuration's build directory
#
# Variables: BUILD (output directory), OPT (default -O2), CXXFLAGS/CFLAGS/LDFLAGS (appended),
# PYTHON (default python3). The real-data tests run when TPM_DISC (and TPM_REF_MANIFEST for the
# --legacy identity test) are set in the environment; see tests/run_tests.py.

CC  ?= cc
CXX ?= g++
OPT ?= -O2
PYTHON ?= python3

TARGET_TRIPLE := $(shell $(CXX) -dumpmachine 2>/dev/null)
ifeq ($(TARGET_TRIPLE),)
  TARGET_TRIPLE := unknown
endif

ifneq ($(SANITIZE),)
  BUILD ?= build/$(TARGET_TRIPLE)-asan
  OPT := -O1 -g
  SANFLAGS := -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer
  SANLINK := -fsanitize=address,undefined
else
  BUILD ?= build/$(TARGET_TRIPLE)
endif

# A prefixed cross compiler (x86_64-w64-mingw32-g++) gets the matching archiver.
ifeq ($(origin AR),default)
  ifneq ($(filter %-g++,$(notdir $(CXX))),)
    AR := $(patsubst %-g++,%-ar,$(CXX))
  endif
endif

EXE :=
PLATFORM_LDFLAGS :=
ifneq ($(findstring mingw,$(TARGET_TRIPLE)),)
  EXE := .exe
  PLATFORM_LDFLAGS += -static
endif
# 32-bit x86: SSE2 single-precision arithmetic like MSVC's /arch:SSE2, not x87 extended precision.
ARCHFLAGS :=
ifneq ($(filter i386% i486% i586% i686%,$(TARGET_TRIPLE)),)
  ARCHFLAGS := -msse2 -mfpmath=sse
endif

WARN := -Wall -Wextra
ifneq ($(WERROR),)
  WARN += -Werror
endif

# -ffp-contract=off: no fused multiply-add, so float results (and the %f text written from them)
# are the same on every compiler and CPU, as with MSVC's default /fp:precise.
COMMON := $(OPT) -ffp-contract=off $(ARCHFLAGS) $(SANFLAGS) -MMD -MP
ALL_CXXFLAGS = -std=c++17 $(WARN) $(COMMON) -I. $(CXXFLAGS)
ALL_CFLAGS = $(WARN) $(COMMON) -I. $(CFLAGS)
ALL_LDFLAGS = $(SANLINK) $(PLATFORM_LDFLAGS) $(LDFLAGS)

BIN := $(BUILD)/tpmextractor$(EXE)

CORE_SRCS := $(filter-out main.cpp,$(sort $(wildcard *.cpp)))
UNSHIELD_SRCS := unshieldv3/ISArchiveV3.cpp unshieldv3/unshieldv3.cpp unshieldv3/blast.c
TEST_SRCS := $(sort $(wildcard tests/unit_*.cpp))
TEST_SUPPORT_SRCS := $(sort $(wildcard tests/support/*.cpp))

obj = $(addprefix $(BUILD)/obj/,$(addsuffix .o,$(basename $(1))))
CORE_OBJS := $(call obj,$(CORE_SRCS))
UNSHIELD_OBJS := $(call obj,$(UNSHIELD_SRCS))
MAIN_OBJ := $(call obj,main.cpp)
TEST_SUPPORT_OBJS := $(call obj,$(TEST_SUPPORT_SRCS))
TEST_OBJS := $(call obj,$(TEST_SRCS))
TEST_BINS := $(patsubst tests/%.cpp,$(BUILD)/tests/%$(EXE),$(TEST_SRCS))

CORE_LIB := $(BUILD)/libtpmx_core.a
UNSHIELD_LIB := $(BUILD)/libunshieldv3.a

.PHONY: all tests check clean check-project-files
.DELETE_ON_ERROR:
# The test objects are only reached through a pattern rule; keep them between runs.
.SECONDARY: $(TEST_OBJS)

all: $(BIN)

# Builds the test programs without running them (for example in a cross build).
tests: $(BIN) $(TEST_BINS)

$(BIN): $(MAIN_OBJ) $(CORE_LIB) $(UNSHIELD_LIB)
	$(CXX) -o $@ $(MAIN_OBJ) $(CORE_LIB) $(UNSHIELD_LIB) $(ALL_LDFLAGS)

$(CORE_LIB): $(CORE_OBJS)
	@rm -f $@
	$(AR) rcs $@ $^

$(UNSHIELD_LIB): $(UNSHIELD_OBJS)
	@rm -f $@
	$(AR) rcs $@ $^

# The vendored code is kept as upstream wrote it (std::tm aggregate initialisation). Its header
# checks are reported errors in the fork, not assert()s, so NDEBUG builds are fine.
$(UNSHIELD_OBJS): WARN += -Wno-missing-field-initializers

$(BUILD)/obj/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(ALL_CXXFLAGS) -c $< -o $@

$(BUILD)/obj/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(ALL_CFLAGS) -c $< -o $@

$(BUILD)/tests/%$(EXE): $(BUILD)/obj/tests/%.o $(TEST_SUPPORT_OBJS) $(CORE_LIB) $(UNSHIELD_LIB)
	@mkdir -p $(dir $@)
	$(CXX) -o $@ $< $(TEST_SUPPORT_OBJS) $(CORE_LIB) $(UNSHIELD_LIB) $(ALL_LDFLAGS)

# Runs every test program from the repository root, then the end-to-end runner (which uses
# unit_b3d_decompress as its level-decoder helper). Exit code 0 = pass, 77 = skipped.
check: $(BIN) $(TEST_BINS)
	@pass=0; skip=0; fail=0; \
	for t in $(TEST_BINS); do \
	  name=$$(basename $$t $(EXE)); tmp="$(abspath $(BUILD))/tests/tmp/$$name"; \
	  rm -rf "$$tmp"; mkdir -p "$$tmp"; \
	  echo "== $$name"; \
	  TPMX_EXE="$(abspath $(BIN))" TPMX_SOURCE_DIR="$(CURDIR)" TPMX_TEST_TMP="$$tmp" "$$t"; rc=$$?; \
	  if [ $$rc -eq 0 ]; then pass=$$((pass+1)); \
	  elif [ $$rc -eq 77 ]; then skip=$$((skip+1)); echo "SKIP $$name"; \
	  else fail=$$((fail+1)); echo "FAIL $$name (exit $$rc)"; fi; \
	done; \
	tmp="$(abspath $(BUILD))/tests/tmp/run_tests"; rm -rf "$$tmp"; mkdir -p "$$tmp"; \
	echo "== run_tests.py"; \
	if command -v $(PYTHON) >/dev/null 2>&1; then \
	  TPMX_EXE="$(abspath $(BIN))" TPMX_SOURCE_DIR="$(CURDIR)" TPMX_TEST_TMP="$$tmp" PYTHONDONTWRITEBYTECODE=1 \
	    $(PYTHON) -I tests/run_tests.py --helper "$(abspath $(BUILD))/tests/unit_b3d_decompress$(EXE)"; rc=$$?; \
	else echo "no $(PYTHON): the end-to-end tests are skipped"; rc=77; fi; \
	if [ $$rc -eq 0 ]; then pass=$$((pass+1)); elif [ $$rc -eq 77 ]; then skip=$$((skip+1)); echo "SKIP run_tests.py"; \
	else fail=$$((fail+1)); echo "FAIL run_tests.py (exit $$rc)"; fi; \
	echo "tests: $$pass passed, $$skip skipped, $$fail failed"; \
	[ $$fail -eq 0 ]

# Every source file must also be in the Visual Studio project and its filters file.
check-project-files:
	@missing=0; \
	for f in $(sort $(wildcard *.cpp *.h unshieldv3/*.cpp unshieldv3/*.c unshieldv3/*.h)); do \
	  w=$$(printf '%s' "$$f" | tr '/' '\\'); \
	  for p in TPMExtractor.vcxproj TPMExtractor.vcxproj.filters; do \
	    grep -qF "Include=\"$$w\"" $$p || { echo "missing from $$p: $$f"; missing=1; }; \
	  done; \
	done; \
	[ $$missing -eq 0 ] && echo "project files list every source"

clean:
	rm -rf $(BUILD)

-include $(CORE_OBJS:.o=.d) $(UNSHIELD_OBJS:.o=.d) $(MAIN_OBJ:.o=.d) $(TEST_SUPPORT_OBJS:.o=.d) \
         $(patsubst tests/%.cpp,$(BUILD)/obj/tests/%.d,$(TEST_SRCS))
