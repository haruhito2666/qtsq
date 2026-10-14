# qtc — QTSQ Custom Compression Library (working codename: *Singularity*)

Standalone compression library built per `../COMPRESSION_LIBRARY_PLAN.md`.

**Phase 3 status:** rANS entropy core + order-1 context model + LZ77.
**Beats zlib-6 by 1.2% on the Silesia corpus** (the standard general-
compression benchmark), within 1.8% of zstd-3, decode at multi-GB/s.
Best-of-all auto-selection: RAW / RAW1 / LZ / STORED — no phase can
regress an earlier one.

## Layout

```
include/qtc.h        public API
src/qtc_internal.h   shared internal definitions (container format, models)
src/qtc_model.c      order-0 model: counts → normalized freq table (M=4096)
src/qtc_ans.c        rANS entropy coder (byte + general-alphabet variants)
src/qtc_order1.c     static order-1 model + rANS encode/decode variants
src/qtc_lz.c         LZ77: hash-chain finder, lazy matching, distance buckets
src/qtc_core.c       container format + qtc_compress / qtc_decompress
tests/test_ans.c     Phase 1–3 test suite + gate checks (202 checks)
bench/qtc_bench.c    benchmark tool (qtc vs zlib on real files)
results/             committed benchmark/phase reports
```

## Build & test

```
make lib     # → libqtc.a
make test    # ASAN+UBSAN correctness suite (no speed gates)
make perf    # -O3 build, runs speed gates (decode ≥ 100 MB/s)
make bench   # builds bin/qtc_bench (qtc vs zlib on real files)
make check   # test + perf
```

## NOT integrated into QTSQ

This directory is intentionally outside the QTSQ build. Integration is Phase 6
and is gated on the numbers in `results/`.
