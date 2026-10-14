# Quantum Tensor Sequence — Build System
# Author: Haruhito

CC       = cc
# The Python tests need the interpreter the C extension was built for
# (it carries a per-version ABI tag), so probe for one that can import
# the package rather than assuming python3 is the right one.
PYTHON  ?= $(shell for p in python3 python3.13 python3.12 python3.11 python3.10; \
                do command -v $$p >/dev/null 2>&1 && $$p -c "import qtsq" >/dev/null 2>&1 \
                   && echo $$p && break; done)
# -MMD -MP: emit .d header-dependency files so header edits (e.g. struct
# layout changes in qtsq_format.h) force recompilation of every consumer.
# Without this, stale objects silently mix old/new struct layouts.
# Phase 8: -O3 for the shipped library (was -O2). Every test target below
# appends its own -O0/-O2 after $(CFLAGS), so the correctness suites keep
# their explicit level; only libqtsq.a objects move to -O3. Output bytes are
# unchanged by optimization level (verified: speed_phase8_baseline.md).
CFLAGS   = -Wall -Wextra -O3 -std=c11 -MMD -MP -Iinclude -Icompressor/include -Icrypto/common -Icrypto/ml-kem-768 -Icrypto/ml-dsa-65 -Icrypto/argon2

UNAME := $(shell uname -s)

ifeq ($(UNAME),Darwin)
  # CoreFoundation/Security: qtsq_keystore.c stores keychain items via
  # CFData and SecItem. Metal/Foundation: qtsq_gravitational.c calls the
  # Metal compute path. These are link-time requirements of libqtsq.a,
  # not just of the apps, so every test binary needs them too.
  LDFLAGS += -lz -framework CoreFoundation -framework Security \
             -framework Metal -framework Foundation
  METAL    = metal
  OSXFLAGS = -framework Metal -framework Foundation
else
  LDFLAGS += -lz -lm -lpthread
  METAL    = true
  OSXFLAGS =
endif

LIB       = libqtsq.a
QTC_LIB   = compressor/libqtc.a
OBJ_CORE  = $(OBJ) $(CRYPTO_OBJ)

# qtsq_metal.mm is Objective-C++ and defines qtsq_metal_global /
# qtsq_metal_render_splats, which qtsq_gravitational.c and
# qtsq_triangle_only.c call. A *.c-only wildcard omits it and every
# link then fails on those symbols.
CORE_SRC   = $(wildcard src/*.c) $(wildcard src/*.mm)
CRYPTO_SRC = $(wildcard crypto/*/*.c)
QTC_SRC    = $(wildcard compressor/src/*.c)
TEST_SRC   = $(wildcard tests/*.c)

# Opus audio: qtsq_audio_opus.c includes <opus/opus.h> unguarded, so it
# only compiles when libopus is installed. Mirror the fallback in
# setup.py -- detect it, and drop the file rather than fail the build.
# Without this every test binary that links libqtsq.a dies on
# _opus_decode/_opus_decoder_create.
OPUS_CFLAGS := $(shell pkg-config opus --cflags 2>/dev/null)
OPUS_LIBS   := $(shell pkg-config opus --libs 2>/dev/null)
ifneq ($(strip $(OPUS_LIBS)),)
  # pkg-config yields .../include/opus, but the source uses the portable
  # <opus/opus.h> form, so add the parent too (same dance as setup.py).
  OPUS_PARENT := $(shell for f in $(OPUS_CFLAGS); do echo $$f | sed 's|/opus$$||'; done)
  CFLAGS  += -DQTSQ_HAVE_OPUS $(OPUS_CFLAGS) $(OPUS_PARENT)
  LDFLAGS += $(OPUS_LIBS)
else
  CORE_SRC := $(filter-out src/qtsq_audio_opus.c,$(CORE_SRC))
endif

# Split by suffix before mapping. A single $(CORE_SRC:.c=.o) leaves
# qtsq_metal.mm unconverted, and that stray word lands in the -include
# list below, so make tries to parse the .mm as a makefile.
CORE_C     = $(filter %.c,$(CORE_SRC))
CORE_MM    = $(filter %.mm,$(CORE_SRC))
OBJ        = $(CORE_C:.c=.o) $(CORE_MM:.mm=.o)
CRYPTO_OBJ = $(patsubst crypto/%.c,crypto/%.o,$(CRYPTO_SRC))
QTC_OBJ    = $(patsubst compressor/src/%.c,compressor/src/%.o,$(QTC_SRC))

.PHONY: all lib demos cli bench-sha viewer studio studio-app photon-studio \
        photon-studio-app metallib xcodeproj vtxdump dylib test test_model \
        test_qtc test_video_pixel test_all_phases test_format_spec \
        test_photon test_tsubu clean

all: lib cli viewer studio photon-studio demos

# ── Library ───────────────────────────────────────────────────────────────
$(QTC_LIB): $(QTC_OBJ)
	ar rcs $@ $^

lib: $(LIB)

# The compressor objects go in directly. Listing $(QTC_LIB) here would
# nest the archive as a member of libqtsq.a, which the linker rejects
# ("archive member not a mach-o file").
$(LIB): $(OBJ_CORE) $(QTC_OBJ)
	ar rcs $@ $^

# Objective-C++ needs an explicit rule; the built-in one only knows .c.
# -std=c11 is rejected by clang for Objective-C++, so drop it and select
# the C++ dialect explicitly. This only shows up on a clean build -- a
# stale src/qtsq_metal.o from an earlier toolchain would mask it.
src/%.o: src/%.mm
	$(CC) $(filter-out -std=c11,$(CFLAGS)) -std=c++11 -x objective-c++ -c $< -o $@

# ── Tools ─────────────────────────────────────────────────────────────────
demos: $(LIB)
	@mkdir -p bin
	@for d in demos/*.c; do \
	    n=$$(basename $$d .c); \
	    $(CC) $(CFLAGS) -O2 $$d -L. -lqtsq $(LDFLAGS) -o bin/$$n -lc++ || exit 1; \
	done
	@echo "built $$(ls bin/ | grep -c '^demo_') demos"

cli: $(LIB)
	@mkdir -p bin
	$(CC) $(CFLAGS) -O2 tools/qtsq_cli.c -L. -lqtsq $(LDFLAGS) -o bin/qtsq -lc++

bench-sha: $(LIB)
	@mkdir -p bin
	$(CC) $(CFLAGS) -O2 tools/bench_sha256.c -L. -lqtsq $(LDFLAGS) -o bin/bench_sha -lc++

vtxdump: $(LIB)
	@mkdir -p bin
	$(CC) $(CFLAGS) -O2 tools/qtsq_vertex_dump.c -L. -lqtsq $(LDFLAGS) -o bin/vtxdump -lc++

viewer: $(LIB)
	@mkdir -p bin
	$(CC) $(CFLAGS) -O2 tools/qtsq_viewer.m -L. -lqtsq $(LDFLAGS) $(OSXFLAGS) -o bin/qtsq_viewer -lc++

dylib: $(OBJ_CORE) $(QTC_LIB)
	$(CC) -shared -o libqtsq.dylib $(OBJ_CORE) $(LDFLAGS)

# ── macOS apps ────────────────────────────────────────────────────────────
metallib:
	@mkdir -p build
	@metallib -ci metal/*.metal -o build/default.metallib 2>/dev/null || \
	    echo "no metal shaders; skipping"

studio: $(LIB) metallib
	@mkdir -p bin build/QTSQStudio.app/Contents/MacOS
	$(CC) $(CFLAGS) -O2 tools/StudioSwift/*.m -L. -lqtsq $(LDFLAGS) $(OSXFLAGS) \
	    -o build/QTSQStudio.app/Contents/MacOS/QTSQStudio -lc++ 2>/dev/null || \
	    echo "studio sources absent; use xcodeproj"
	@cp -R QTSQStudio.app/Contents/Resources build/QTSQStudio.app/Contents/ 2>/dev/null || true

studio-app: studio

photon-studio: $(LIB) metallib
	@mkdir -p build/PhotonStudio.app/Contents/MacOS
	$(CC) $(CFLAGS) -O2 tools/PhotonStudioSwift/*.m -L. -lqtsq $(LDFLAGS) $(OSXFLAGS) \
	    -o build/PhotonStudio.app/Contents/MacOS/PhotonStudio -lc++ 2>/dev/null || \
	    echo "photon studio sources absent; use xcodeproj"
	@cp -R PhotonStudio.app/Contents/Resources build/PhotonStudio.app/Contents/ 2>/dev/null || true

photon-studio-app: photon-studio

xcodeproj:
	@echo "open QTSQStudio.xcodeproj / PhotonStudio.xcodeproj in Xcode"

# ── Tests ─────────────────────────────────────────────────────────────────
test: $(LIB)
	@mkdir -p bin
	@if [ ! -f tests/test_qtsq.c ]; then echo "skip  test_qtsq (no file)"; \
	 else $(CC) $(CFLAGS) -O0 -g tests/test_qtsq.c -L. -lqtsq $(LDFLAGS) -lc++ \
	      -o bin/test_qtsq && ./bin/test_qtsq; fi

test_model: $(LIB)
	@mkdir -p bin
	@echo "═══ Model Weights (exact) ═══"
	$(CC) $(CFLAGS) -O0 -g -fsanitize=address,undefined -fno-omit-frame-pointer tests/test_model.c -L. -lqtsq $(LDFLAGS) -lc++ -o bin/test_model
	./bin/test_model

# Phase 6a: qtc integration round-trip + backward-compat + crafted-input gate.
test_qtc: $(LIB)
	@mkdir -p bin
	@echo "═══ Phase 6: qtc Integration ═══"
	$(CC) $(CFLAGS) -O0 -g -fsanitize=address,undefined -fno-omit-frame-pointer tests/test_qtc_integration.c -L. -lqtsq $(LDFLAGS) -lc++ -o bin/test_qtc_integration
	./bin/test_qtc_integration

# Pixel-path video: GOP round-trip, lossy PSNR, 4DGS regression, QTSQ_BITSTREAM_MB cap.
test_video_pixel: $(LIB)
	@mkdir -p bin
	@echo "═══ Pixel Video + Bitstream Cap ═══"
	$(CC) $(CFLAGS) -O0 -g -fsanitize=address,undefined -fno-omit-frame-pointer tests/test_video_pixel.c -L. -lqtsq $(LDFLAGS) -lc++ -o bin/test_video_pixel
	./bin/test_video_pixel

# Spec conformance: parse real containers using only the offsets in
# docs/FORMAT.md, and confirm damaged files are rejected. Writing the
# spec is not enough on its own -- this is what stops the two drifting
# apart. It also regression-guards the 2.2 codec_id fallthrough bug.
test_format_spec:
	@test -n "$(PYTHON)" || { echo "no interpreter can 'import qtsq'; \
	  build the extension first (python setup.py build_ext --inplace)"; exit 1; }
	@$(PYTHON) tests/test_format_spec_py.py

# H18 fix: run ALL phase tests (previously 12 test files were orphaned —
# not compiled or run by any target, so regressions in Argon2id, layered
# encryption, Shamir, seal, container-name encryption went uncaught).
#
# Each name below is compiled only if tests/<name>.c exists. The list
# is longer than the files on disk: several of these were removed while
# the target still referenced them, which made `make test_all_phases`
# fail on the first missing one and silently gate every later test.
# Reporting a skip keeps the target usable and makes the gap visible.
PHASE_TESTS = test_layered_encryption test_shamir_csprng test_two_intent \
              test_geometry_seal test_freqdct test_container_name_enc \
              test_security test_gf256 test_splat2d test_surfel \
              test_photon_fusion test_turbo test_model \
              test_qtc_integration test_video_pixel

define RUN_PHASE_TESTS
	@mkdir -p bin
	@for t in $(PHASE_TESTS); do \
	    if [ -f tests/$$t.c ]; then \
	        $(CC) $(CFLAGS) -O0 -g tests/$$t.c -L. -lqtsq $(LDFLAGS) -lc++ \
	            -o bin/$$t >/dev/null 2>&1 \
	            || { echo "BFAIL $$t"; echo $$t >> .phase_test_failures; continue; }; \
	        ./bin/$$t >/dev/null 2>&1 && echo "pass  $$t" \
	            || { echo "FAIL  $$t"; echo $$t >> .phase_test_failures; }; \
	    else \
	        echo "skip  $$t (no tests/$$t.c)"; \
	    fi; \
	done
	@if [ -f .phase_test_failures ]; then \
	    echo; echo "FAILED: $$(cat .phase_test_failures | tr '\n' ' ')"; \
	    rm -f .phase_test_failures; exit 1; \
	fi
	@echo "all phase tests passed"
endef

test_all_phases: $(LIB) test test_format_spec
	$(RUN_PHASE_TESTS)

test_photon: $(LIB)
	@mkdir -p bin
	@if [ ! -f tests/test_photon_sonar.c ]; then echo "skip  test_photon_sonar (no file)"; \
	 else $(CC) $(CFLAGS) -O0 -g tests/test_photon_sonar.c -L. -lqtsq $(LDFLAGS) -lc++ \
	      -o bin/test_photon_sonar && ./bin/test_photon_sonar; fi

test_tsubu: $(LIB)
	@mkdir -p bin
	@if [ ! -f tests/test_tsubu.c ]; then echo "skip  test_tsubu (no file)"; \
	 else $(CC) $(CFLAGS) -O0 -g tests/test_tsubu.c -L. -lqtsq $(LDFLAGS) -lc++ \
	      -o bin/test_tsubu && ./bin/test_tsubu; fi

# ── Housekeeping ──────────────────────────────────────────────────────────
clean:
	rm -f $(OBJ) $(CRYPTO_OBJ) $(QTC_OBJ) $(LIB) $(QTC_LIB) libqtsq.dylib
	rm -rf bin build/default.metallib

-include $(OBJ:.o=.d) $(CRYPTO_OBJ:.o=.d) $(QTC_OBJ:.o=.d)