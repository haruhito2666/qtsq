# qtc — QTSQ Custom Compression Library
# Standalone build with its own -Werror gate; since Phase 6a the parent
# Makefile links compressor/libqtc.a into QTSQ (opt-in codec, see
# COMPRESSION_LIBRARY_PLAN.md Phase 6).

CC      = cc
CFLAGS  = -Wall -Wextra -Werror -std=c11 -Iinclude -Isrc -pthread
LDFLAGS = -lm -pthread

# Linux: strict -std=c11 hides M_PI (math.h guards it behind
# _DEFAULT_SOURCE); Darwin's math.h exposes it unconditionally.
ifeq ($(shell uname -s),Linux)
    CFLAGS += -D_DEFAULT_SOURCE
endif

SRC = src/qtc_model.c src/qtc_ans.c src/qtc_order1.c src/qtc_lz.c src/qtc_image.c src/qtc_audio.c src/qtc_detect.c src/qtc_mdct.c src/qtc_audio_mdct.c src/qtc_audio_mdct2.c src/qtc_audio_float.c src/qtc_delaunay.c src/qtc_image_mesh.c src/qtc_accretion.c src/qtc_image_dct.c src/qtc_tensor.c src/qtc_video.c src/qtc_core.c \
      src/qtc_text.c \
      src/qtc_table.c
OBJ = $(SRC:.c=.o)
LIB = libqtc.a

.PHONY: all lib test perf bench speed bench-image bench-image-rd bench-audio bench-audio-rd cli check clean

all: lib

lib: $(LIB)

$(LIB): $(OBJ)
	ar rcs $@ $^
	@echo "  libqtc.a built"

src/%.o: src/%.c include/qtc.h src/qtc_internal.h
	$(CC) $(CFLAGS) -O3 -c $< -o $@

# Correctness suite: ASAN + UBSAN, no optimization.
test: $(SRC) tests/test_ans.c
	@mkdir -p bin
	$(CC) $(CFLAGS) -O0 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
		$(SRC) tests/test_ans.c $(LDFLAGS) -lz -o bin/test_ans_asan
	./bin/test_ans_asan

# Performance build: -O3, runs the speed gates.
perf: $(SRC) tests/test_ans.c
	@mkdir -p bin
	$(CC) $(CFLAGS) -O3 -DNDEBUG $(SRC) tests/test_ans.c $(LDFLAGS) -lz -o bin/test_ans_perf
	./bin/test_ans_perf --speed

# Benchmark tool (Phase 2+): qtc vs zlib on real files.
bench: $(SRC) bench/qtc_bench.c
	@mkdir -p bin
	$(CC) $(CFLAGS) -O3 -DNDEBUG $(SRC) bench/qtc_bench.c $(LDFLAGS) -lz -o bin/qtc_bench
	@echo "  bench built → bin/qtc_bench"

# Phase 8 realistic speed benchmark: per-lens encode/decode MB/s on real
# corpora. SPEED_OPT is overridable so we can compare codegen levels
# (e.g. `make speed SPEED_OPT="-O2"` vs the default -O3).
SPEED_OPT ?= -O3 -DNDEBUG
speed: $(SRC) bench/qtc_speed.c
	@mkdir -p bin
	$(CC) $(CFLAGS) $(SPEED_OPT) $(SRC) bench/qtc_speed.c $(LDFLAGS) -o bin/qtc_speed
	@echo "  speed bench built → bin/qtc_speed (run: ./bin/qtc_speed)"

# Image-lens benchmark (Phase 4): qtc-image vs zstd-3/zlib on real PNGs.
# -Wno-unused-function: stb_image leaves unused statics under STBI_ONLY_PNG.
bench-image: $(SRC) bench/qtc_bench_image.c
	@mkdir -p bin
	$(CC) $(CFLAGS) -Wno-unused-function -O3 -DNDEBUG -I/opt/homebrew/include $(SRC) bench/qtc_bench_image.c $(LDFLAGS) -lz -L/opt/homebrew/lib -lzstd -o bin/qtc_bench_image
	@echo "  image bench built → bin/qtc_bench_image"

# Image rate-distortion benchmark (Phase 5): lossy mesh vs JPEG/WebP (PSNR+SSIM).
bench-image-rd: $(SRC) bench/qtc_bench_image_rd.c
	@mkdir -p bin
	$(CC) $(CFLAGS) -Wno-unused-function -O3 -DNDEBUG -I/opt/homebrew/include $(SRC) bench/qtc_bench_image_rd.c $(LDFLAGS) -L/opt/homebrew/lib -ljpeg -lwebp -o bin/qtc_bench_image_rd
	@echo "  image RD bench built → bin/qtc_bench_image_rd"

# Audio-lens benchmark (Phase 4): qtc-audio vs zstd-3/zlib on synthesized PCM.
bench-audio: $(SRC) bench/qtc_bench_audio.c
	@mkdir -p bin
	$(CC) $(CFLAGS) -O3 -DNDEBUG -I/opt/homebrew/include $(SRC) bench/qtc_bench_audio.c $(LDFLAGS) -lz -L/opt/homebrew/lib -lzstd -o bin/qtc_bench_audio
	@echo "  audio bench built → bin/qtc_bench_audio"

# Audio rate-distortion benchmark (Phase 5): lossy MDCT quality sweep + Opus.
bench-audio-rd: $(SRC) bench/qtc_bench_audio_rd.c
	@mkdir -p bin
	$(CC) $(CFLAGS) -O3 -DNDEBUG -DQTC_HAVE_OPUS -I/opt/homebrew/include -I/opt/homebrew/include/opus $(SRC) bench/qtc_bench_audio_rd.c $(LDFLAGS) -L/opt/homebrew/lib -lopus -o bin/qtc_bench_audio_rd
	@echo "  audio RD bench built → bin/qtc_bench_audio_rd"

# Manual-testing CLI (Phase 5.5b): compress/decompress files + photos,
# native image window (Cocoa) + CoreAudio playback. Compiled as Objective-C
# so the Cocoa renderer links; -Wno-deprecated-declarations silences the
# AppKit activation API used to focus the window when launched from a shell.
cli: $(SRC) tools/qtc_cli.c
	@mkdir -p bin
	$(CC) $(CFLAGS) -x objective-c -Wno-unused-function -Wno-deprecated-declarations -O2 -DNDEBUG -I/opt/homebrew/include $(SRC) tools/qtc_cli.c $(LDFLAGS) -framework Cocoa -framework AudioToolbox -o bin/qtc
	@echo "  cli built → bin/qtc"

check: test perf

clean:
	rm -f src/*.o $(LIB)
	rm -rf bin
