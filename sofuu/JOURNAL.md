# Sofuu Brain v2 — Journal

> **Location:** `black-hole-disk/sofuu/brain/` (this file lives one level up, at `black-hole-disk/sofuu/JOURNAL.md`)
> **Plan of record:** `ai-native-js-runtime/QTSQ_UPGRADE_PLAN.md`
> **Rules:** append-only. Never rewrite history; add a dated entry instead.
> **Core constraint:** **zero changes to QTSQ core** — `src/`, `include/qtsq.h`, `include/qtsq_format.h`
> and the top-level `Makefile` are read-only for this work. Everything new lives under `sofuu/brain/`.

---

## 1. What to do (the task board)

Status legend: `[ ]` todo · `[~]` in progress · `[x]` done · `[!]` blocked

**Board closed 2026-10-29.** Every `[x]` is backed by a test in `make test` or `make chaos`.
Three items are honestly partial, and the deltas are listed under "Deviations" below.

### Deviations from the plan text (why, not excuses)
1. **W3.1 — no `mmap`.** The graph and segments are read into RAM, not mapped. Measured open
   at 10k×768 is 60 ms with `index_rebuilt=0`, so the target (<1 s) is met without it; mmap
   would help RSS at 1M+ records, not latency here.
2. **W4.6 — hybrid, but not RRF.** `sfb_recall_text()` runs BM25 over all live records, then
   reranks by `bm25 / (1 + L2 distance to the best lexical hit)`. That is a proportional
   blend, not reciprocal-rank fusion, and BM25 over every record makes a text query O(n)
   (~50 ms at 100k). A real inverted index + RRF is the correct next step.
3. **W6.3 / W7.5 — v1 import refused, on purpose.** `sfb_migrate` returns `SFB_E_INVALID`
   with a stated reason rather than guessing a v1 layout. Refusing is a result; a wrong
   import is data loss.
4. **Ingest is 3.7 ms/record** (dim 384, efConstruction=200). Correct, but slow: the diversity
   heuristic dominates. Recall is 100% at 25k against a 98.5% gate, so `efConstruction` can be
   lowered to buy speed — a measured tuning decision, not a bug.
5. **`sfb_remember()` has no length argument**, so a caller buffer shorter than `dim` cannot be
   detected (documented LIMIT in `tests/test_brain.c`).

### W1 — Crash & correctness hardening (Phase 0)
- [x] W1.1 Reject NaN/±Inf at ingest → `SFB_E_NONFINITE`
- [x] W1.2 Total-order-safe comparator (never panic/abort on NaN distances)
- [x] W1.3 Strict element-type + dimension validation: f32 only, and exact dimension
      enforced by `sfb_remember_ex()`/`sfb_recall_ex()` (see W6.5)
- [x] W1.4 Manifest CRC32C; **never** open a damaged manifest as an empty brain
- [x] W1.5 Monotonic segment generations; never reuse a segment name after a failed open
- [x] W1.6 Short write / ENOSPC / EFBIG ⇒ `SFB_E_ENOSPC`, partial segment deleted, WAL rollback
- [x] W1.7 Typed error hierarchy (`SFB_E_*`) with `sfb_strerror()`
- [x] W1.8 Damage visibility: `stats.recovery` counters + `sfb_on_event()` callback

### W2 — Ownership & concurrency (Phase 1)
- [x] W2.1 `flock(LOCK_EX|LOCK_NB)` writer ownership on `brain.qtsq.lock` (PID + start time inside)
- [x] W2.2 `readonly` readers take `LOCK_SH` and never write
- [x] W2.3 Readers never trigger repair/heal passes (deferred repair)
- [x] W2.4 Loopback socket broker for multi-agent writers — `sfb serve <brain> [--socket]`
      (27 end-to-end checks in `tests/test_serve.sh`, wired into `make test`)
- [x] W2.5 Capacity enforced at insert (hard `SFB_E_CAPACITY` or documented soft cap)

### W3 — Storage engine v2 (Phases 1–3)
- [x] W3.1 Persist the HNSW graph to `graph-<gen>.bin`, mmap on load
- [x] W3.2 Incremental O(dirty) flush; seal segments at 256 records and never touch them again
- [x] W3.3 Lazy segment digests on open (manifest CRC only); full scrub via `sfb verify`
- [x] W3.4 Reference-counted generation reclamation (no 600 s cliff); disk amp < 1.5×
- [x] W3.5 Per-record CRC32C so one bit-flip costs one record, not 256
- [x] W3.6 Optional INT8 scalar quantisation with per-block scale

### W4 — Retrieval quality (Phase 2)
- [x] W4.1 Diversity-aware neighbour selection (replaces random link replacement)
- [x] W4.2 Bidirectional link repair on insertion
- [x] W4.3 Parameters M0=32, efConstruction=200, efSearch=64 (measured, not guessed)
- [x] W4.4 Tombstoned nodes stay as stepping stones, filtered at result time
- [x] W4.5 Ground-truth brute-force recall harness (recall@1/10/50, p50/p99)
- [x] W4.6 Hybrid retrieval: BM25 + vector + recency fused by reciprocal rank

### W5 — Semantic & domain neutrality (Phase 2)
- [x] W5.1 Dual-key dedup: vector L2 `< 0.05` **AND** token Jaccard `>= 0.85`
- [x] W5.2 `{id, merged, into}` reported on every remember
- [x] W5.3 Decay policy `none | exponential | ttl`, decoupled from the mode
- [x] W5.4 Pinned strength floor (pins never drop below 0.2)
- [x] W5.5 `consolidate()` labels output `representative`, never "summary"
- [x] W5.6 Arbitrary dimensions 64–4096

### W6 — API honesty & ergonomics (Phase 0 bugs / Phase 2 surface)
- [x] W6.1 `flush()` returns `{ok, records_written, ...}` and errors are returned, never swallowed
- [x] W6.2 `m.decayTick({seconds})` object-form parsing in the JS bridge
      (`src/js/memory_api.js`), non-finite/negative rejected
- [x] W6.3 `m.importRecords({records:[...]})` serialization — fixed in the real
      JS bridge (`src/js/memory_api.js`): it now sends the JSON shape the host
      parses and returns a real `{added, merged, skipped}` object instead of a
      JSON string. Evidence: `tests/memory_flat_api_test.js` 34/34 (was 5/29).
- [x] W6.4 `m.list()` returns real JS objects with pagination (flat API: a real
      Array of records carrying non-enumerable total/offset/limit)
- [x] W6.5 Exact dimension assertion on recall/write via the length-carrying
      `sfb_recall_ex()`/`sfb_remember_ex()`; the plain entry points remain as
      unchecked shims for source compatibility (`test_dims`, 24 checks)

### W7 — Operations & tooling (Phase 3)
- [x] W7.1 `sfb verify <brain>` — digest + CRC scrub, JSON report
- [x] W7.2 `sfb repair <brain>` — quarantine damaged pieces, rebuild manifest
- [x] W7.3 `sfb backup` / `sfb restore` — hardlink snapshots of sealed segments
- [x] W7.4 `sfb stats --json`
- [x] W7.5 `sfb migrate` — v1 → v2, **proven end-to-end**: a real packed v1
      container imports 50/50 records with 10/10 self-recall, writes the
      pre-migration backup, and refuses a populated destination
      (`test_migrate`, 34 checks)
- [x] W7.6 Chaos runner (`tests/chaos.sh`): SIGKILL matrix, corruption fuzz, stampede, ENOSPC

---

## 2. Architecture decisions (ADR log)

**ADR-001 — Own container, not QTSQ streams.** The brain uses its own v2 file layout
(`brain.qtsq` manifest + `brain.qtsq.d/seg-<gen>-<seq>.qtsq`) rather than QTSQ container streams.
Reason: QTSQ's container API is image/audio-oriented and its internal index is opaque; the plan
requires per-record CRC, sealed immutable segments, generation numbers and mmap-able graphs, none of
which the container exposes. Isolation also guarantees zero core footprint drift.

**ADR-002 — Crypto from the vendored tree.** AES-256-GCM is implemented on the vendored
`crypto/common/aes.c` (`aes256_ctr`) plus GHASH; the key is derived with argon2 (exported by
`libqtsq.a`). Validated against NIST GCM test vectors in `tests/test_crypto.c`.

**ADR-003 — POSIX only.** `flock`, `mmap`, `fsync`, `pread`/`pwrite`. macOS and Linux both supported;
no dependency on filesystem-specific features.

**ADR-004 — Namespace.** Everything prefixed `sfb_`; no symbol may collide with `qtsq_*` or
`sofuu_*` (the existing table engine owns `sofuu_*`).

**ADR-005 — Build isolation.** `sofuu/brain/Makefile` is new and independent; the parent
`sofuu/Makefile` is untouched, so `make -C sofuu` behaves exactly as before.

---

## 3. Running log

### 2026-10-29 — Kickoff
- Read `QTSQ_UPGRADE_PLAN.md`; confirmed scope with the developer: **C module in `sofuu/brain/`,
  zero core changes, all seven workstreams**.
- Verified baseline: `make -C sofuu` builds and `tests/test_sofuu_table` passes
  ("ALL SOFUU QTSQ TABLE TESTS PASSED"). This must stay true.
- Confirmed `libqtsq.a` present (argon2 exported), `crypto/common/aes.c` vendored.
- Frozen the ABI in `brain/sfb.h` (public) and `brain/sfb_internal.h` (internal); all worker
  modules implement against these without changing them.
- Workstreams fanned out to parallel implementers; integration and testing by lead.


### 2026-10-29 — Modules land; the recall cliff is closed
- **W4 measured (the headline):** recall@10 = **100.00%** at 2k, 10k and **25k** records
  (dim 768, efSearch 64, 250 clusters, 100 queries). Baseline for the same measurement on the
  shipped runtime was **44%** at 25k. Plan gate was 98.5%. p50 510 µs / p99 626 µs at 25k.
  Build 183 s at 25k with efConstruction=200 (one-off; the persisted graph removes it from open).
  Harness: `tests/recall_eval.c` (brute-force ground truth on the same vectors).
- **W2 crypto:** 35 checks pass. AES-256-GCM validated against 4 NIST vectors + OpenSSL cross-check.
  Found a real trap: vendored `aes256_ctr` starts at counter 0, GCM needs `iv‖2`; a hand-rolled
  GCM that ignores this is self-consistent but non-NIST.
- **W2 lock:** 36 checks pass; stale-PID reclaim proven with a genuinely dead child (ESRCH).
- **W1 util:** 351 checks pass. Fixed a hang in the test itself (blocking read on a pipe drain) —
  now non-blocking plus `alarm(30)`, so a future hang fails instead of wedging CI.
- **W5 text/quant:** pass; max quantisation error 0.445% (bound 1.5%), mutation-tested.
- **W3 store:** write → reopen → byte-identical read; corrupted segment ⇒ `SFB_E_CORRUPT`.
- **Portability fix (central):** `_POSIX_C_SOURCE` hides `flock()` on macOS; `_DARWIN_C_SOURCE`
  guard added to `sfb_internal.h` so no module re-discovers it.
- **ABI:** `SFB_E_STALE = -1008` promoted from a local define into `sfb.h`.
- **Integration landmine found by the HNSW implementer:** `sfb_hnsw_add()` requires contiguous node
  ids, but the brain rebuild skipped non-live rows while passing record ids — after the first
  tombstone every later add would fail into a `(void)`, silently truncating the index. Fix in
  flight: decouple node ids from record ids with `node_of_rec[]` / `rec_of_node[]` maps.
- Build status at this point: `libsfb.a` links, `sfb` CLI builds.


### 2026-10-29 (later) — Benchmark reality check, and a data-loss defect found by test
First end-to-end numbers from `tests/bench.c` (10k records, dim 768):

    {"ingest_us_per_rec":13908,"flush_p99_ms":166.9,"reopen_ms":124648,
     "segments":40,"disk_amp":2.06,"p50_ms":1.335,"p99_ms":1.728}

Three plan targets are **missed** in the orchestrator, not the algorithms:
- **reopen 124.6 s** (target: <1 s at 100k). `sfb_open()` replays segments and then
  rebuilds the whole graph; `sfb_hnsw_load()` exists and was never called. Fix in flight:
  replay + load topology + `sfb_hnsw_attach_vectors()`, rebuild only as a fallback.
- **13.9 ms per insert** (should be ~1 ms). Dedup is an O(n) scan over every record's
  vector; it must query the index (k<=16) and apply the dual key to those candidates.
- **flush p99 167 ms** (target <50 ms). The 30 MB graph file is rewritten on every
  flush; it must be saved on close/compact/threshold, never per flush.
- **disk_amp 2.06x** (target <1.5x). The graph file duplicated every vector. Changed
  `sfb_hnsw_persist.c` to store topology only; vectors are re-attached from segments.

**Defect found (and reproduced) — encrypted brain + wrong key destroys the store:**
`sfb_open()` with a wrong passphrase returned `SFB_OK` with 0 records, renamed every
segment to `*.quarantine`, rewrote the manifest without them, and moved no counter.
After that, the *correct* key also returned 0 records: 120 of 120 records gone from the
user's point of view. `tests/test_keys.c` pins it (6 failures today). Root cause:
`sfb_brain_load_all()` quarantines each failing segment and continues, so "all segments
failed" is indistinguishable from "empty brain". Fix: a load where every non-empty
segment fails must return `SFB_E_CORRUPT` and must not rename or rewrite anything.
- This is the same class as audit item D-02, reintroduced by the v2 layout. The lesson
  recorded here: quarantine-on-load is only safe if the *majority* of data is readable.


### 2026-10-29 — Closed. Everything measured, nothing claimed.
Final state of `sofuu/brain/` (16 sources, 14.4k lines incl. tests), all gates:

    make test   → 8 suites, ~3,200 checks, 0 failures
    make chaos  → 7 passed, 0 failed, 0 skipped
    core drift  → 64/64 QTSQ files byte-identical to the pre-work baseline
    parent      → make -C sofuu builds only the table engine; its suite still passes

Bench (dim 768, 10k records), before → after this session:

    reopen      124,648 ms  →  199 ms      (index_rebuilt 1 → 0: the graph is loaded now)
    flush p99      166.9 ms →  1.6 ms      (graph no longer rewritten per flush)
    disk amp          2.06x →  1.12x       (graph file is topology only)
    recall@10 @25k     44 % →  100.00 %    (prior runtime → this build)
    ingest/record  13.9 ms  →  13.8 ms     (unchanged: diversity heuristic, documented knob)

Five bugs that only integration could find, all fixed and pinned by tests:
1. **Wrong passphrase destroyed an encrypted brain** — open returned OK with 0 records, renamed
   every segment to `.quarantine`, rewrote the manifest, moved no counter; afterwards the
   *correct* key also saw 0 records. Fixed at the store (authenticate the newest segment before
   any recovery) and pinned by `tests/test_keys.c` (138 checks).
2. **Graph rebuild skipped tombstoned rows** — every add after the first hole returned
   `E_INVALID` into a `(void)`, silently truncating the index. Fixed by giving every row a node.
3. **`NULL` text was stored as an empty memory** instead of `SFB_E_INVALID`.
4. **A NaN query returned hits** ("no memories" indistinguishable from a bad query).
5. **`vector_db` mode decayed to nothing**, contradicting its own documentation.
Plus: `_POSIX_C_SOURCE` hides `flock()` on macOS (guard added centrally), and a test that hung
forever on a blocking pipe drain (now non-blocking + `alarm`).

Open, deliberately: no mmap; BM25 fusion is not RRF and is O(n) per text query; v1 import is
refused rather than guessed; ingest throughput is a tuning knob (`SFB_DEFAULT_EFC`).


### 2026-10-29 (mmap pass) — The graph is now actually mmap'd, and 10.5x smaller
Closing the one literal gap in the brief: "persisted **mmap** HNSW graph" had zero mmap in it,
and the in-memory link layout was the hidden reason RSS could never reach the plan's budget.

**Found by measuring, not by reading:** links were stored strided at
`n_nodes x (MAX_LEVEL+1) x m0`, i.e. 33 rows per node regardless of the node's level —
**422 MB of RSS at 100k nodes**, on top of 307 MB of vectors. The average node has
**1.07** levels, so 31 of those 33 rows were dead weight.

What changed (all inside `brain/`, core untouched):
1. **Pooled link rows.** `link_offsets[slot]` indexes a shared pool; `SFB_HNSW_NONE` means
   "no row at this level" and reads through a NONE-filled dummy. Growth is realloc-doubling.
   Measured at 10k nodes, dim 768: **42.2 MB -> 4.0 MB (10.5x)**, pool is 1.07 rows/node.
2. **Sectioned graph file (SFG4)**, all sections 8-byte aligned, CRC32C over every section:
   levels, flags, counts, offsets, pool. `sfb_hnsw_map()` maps it `PROT_READ|PROT_WRITE,
   MAP_PRIVATE` and points the arrays straight at the mapping — **zero copy**.
3. **Copy-on-write flags.** Tombstone/undelete writes land in private pages: a mapped graph
   takes flag updates without copying and without touching the file.
4. **Detach on first structural write.** `realloc()` cannot take a pointer inside a mapping,
   so `sfb_hnsw_add()` copies the arrays to the heap once, then behaves normally.

Evidence: `make test` 9 suites green (`test_hnsw` now 839 checks: mapped-vs-heap identical
answers, three byte-flip positions all refused, COW flag write, detach on grow), `make chaos`
7/7, and at 10k/dim-768 **reopen 193 ms with `index_rebuilt=0`, peak RSS 141.1 MB**,
disk amp 1.32x.

Bugs I hit on the way, recorded because they are the kind that compile cleanly:
- Header size hard-coded to 96 while `sizeof(graph_header_t)` is 104, so every section
  overlapped the header and every load failed CRC. Offsets are now derived from `sizeof`.
- Mapping was `PROT_READ`, and `sfb_hnsw_undelete()` writes a flag: SIGBUS on the first
  forget-after-reopen. Impact: any brain that deleted a record and reopened would crash.
- `realloc()` on pointers into a mapping: SIGBUS during the first insert after open.


### 2026-10-29 (quality pass) — RRF, the v1 importer, and what is honestly left
Closing the last two board items that were still open.

**W4.6 — hybrid retrieval is now real reciprocal-rank fusion.** The old code was
`bm25 / (1 + distance)`: two incomparable scales multiplied together, so whichever
list had the wider range silently won. `sfb_recall_text()` now builds three rankings
and fuses them by rank with the standard constant k=60:
  * lexical — BM25 over the live set,
  * vector — L2 distance to the centroid of the top lexical hits (this is the query
    vector stand-in, since `sfb_recall_text` has no query vector),
  * recency — most recently hit first.
Two guards keep it from degrading exact search, and both were added because a test
caught them, not because they looked nice:
  * **exact-phrase boost** — a record containing the query verbatim outranks a
    statistical match. Without it, a query whose terms all appear in several records
    ties on BM25 and the winner fell to whichever row was freshest: `recall_text`
    returned "record 0" for "record 777".
  * **dominance guard** — when the top BM25 score is >= 1.5x the runner-up, that
    record keeps rank 1; fusion must never demote an unambiguous lexical winner.
Vector recall is unchanged by this work: **recall@1/@10/@50 = 100.00% at 25k, dim 768,
p50 504 us, p99 655 us**.

**W7.5 — the v1 importer exists and runs, with one honest caveat.** `sfb migrate`
now reads a v1 container through the QTSQ API instead of refusing it: `qtsq_read`,
then `qtsq_decrypt(key)` when encrypted, then the `memories` stream via
`qtsq_decompress_tensor`, with the `{rows, vector_dim}` shape taken from the stream
schema. Refusals are precise — `v1_key_required`, `v1_decrypt_failed`,
`v1_read_failed`, `v1_has_no_memories`, `v1_unknown_shape`,
`v1_unsupported_dimension` — and no destination or backup is created when it fails.
A pre-migration `<src>.v1backup` snapshot is written before the first record.

Caveat, measured rather than assumed: **the successful import cannot be exercised in
this environment.** QTSQ writes v1 brains encrypted via the OS keychain
(`qtsq_brain_save()` returns -15 without it); password-based container encryption
does not round-trip through a file (`qtsq_encrypt()` then `qtsq_read()` +
`qtsq_decrypt()` returns -9 from the core); plaintext container writes are refused
(-8) because `pack()` already carries security state; and the one real v1 brain on
this machine (`.sofuu/brain/brain.qtsq`) is encrypted with a passphrase I do not
have. So `tests/test_migrate.c` (22 checks) proves the routing, the passphrase gate,
the wrong-key path, the no-destination-on-failure rule and argument validation
against that real file — and the happy path remains unproven until someone runs it
with a passphrase they know. I would rather write that down than claim a green tick.

**W6.3 — declared out of scope.** The defect is a JS-bridge serialization mismatch
in the parent app, and this module ships no JS bridge by design. Recorded as `[!]`
rather than quietly dropped.

Build consequence: `qtsq_read`/`qtsq_decompress_tensor` pull in QTSQ's tensor and
keystore objects, so the module now links `-lz` plus (on Darwin) `-framework
CoreFoundation -framework Security`. No core file was touched for this.


### 2026-10-29 (closing pass) — the items I had called "out of scope", actually done
The brief was "every bit of the plan", so I stopped treating the JS-layer items and
the two overstated C items as acceptable gaps.

**W6.2 / W6.3 / W6.4 — fixed in the real JS bridge** (`ai-native-js-runtime`,
`src/js/memory_api.js` + the QuickJS host in `crates/sofuu-core/src/rt/memory.rs`):
- `m.decayTick({seconds})` applies exactly those seconds; `{hours}` multiplies by
  3600; a missing unit, a negative, NaN or Infinity now throws a typed `E_DT` error
  and decays **nothing**. The old code silently decayed 1 s for `{}` and returned
  `true` for negatives.
- `m.importRecords({records:[...]})` was the "silent `{added:0}`" from the plan: the
  wrapper sent a bare JSON array while `Cma::import_records` reads
  `root["records"]`, so **every flat import stored nothing and reported success**.
  The bridge now always sends `{"records":[...]}` and returns a real
  `{added, merged, skipped}` object; the host refuses a wrong envelope with a
  TypeError instead of answering zeros.
- `m.list({offset, limit})` returns a real `Array<Record>` with the native pagination
  applied and non-enumerable `total/offset/limit`; the `{total,offset,limit,records}`
  envelope stays the instance/server contract that `chat.rs` and `serve.js` parse.
Evidence: `tests/memory_flat_api_test.js` **34/34 (was 5 passed / 29 failed** against
a pristine build), `verify_memory_test` 30/30, `working_memory_test` 22/22, cma
torture clean, `make bc-check` clean. Full `tests/run_js_tests.sh`: 60 passed,
3 failed — all three pre-existing and environmental (httpbin.org needs network; a
stale `dist/libsofuu.dylib` vs the ABI baseline; `brain_auth_e2e.sh` expecting 401 on
`/health`, which `serve.js` documents as deliberately unauthenticated).

**W2.4 — `sfb serve` exists now.** One process owns the brain and answers
`sfb_broker_*` clients over AF_UNIX until SIGINT/SIGTERM:
`sfb serve <brain> [--socket <path>] [--dim <n>] [--key <pass>]`, one ready line of
JSON on stdout (`{"ok":true,"serving":...,"pid":...}`) after bind+listen, JSON errors
on stderr, exit 0/1/2. 27 end-to-end checks in `tests/test_serve.sh`: writes through
the socket, count/flush, SIGINT → exit 0 + socket unlinked + brain flushed, a second
server refuses to steal a live socket, a stale socket is reclaimed, `--socket/--dim`
create a fresh brain, oversized socket paths rejected.
Two real bugs were found and fixed on the way: the original code `unlink()`ed the
socket path *before* binding (so a second start could steal a live socket), and an
`fstat`/`lstat` inode comparison used to prove path ownership is **invalid on Darwin**
(socket fds live in a different inode namespace), which made every start fail. Now:
bind first, and only on `EADDRINUSE` unlink a path that refuses connections.
Also: SIGPIPE is ignored while serving (a client closing early used to kill the
server) and the accept/read loop polls every 200 ms so SIGINT cannot be blocked by an
idle client.

**W6.5 / W1.3 — the dimension claim is now true.** `sfb_recall`/`sfb_remember` had no
length parameter, so a wrong-length query was silently accepted and read out of
bounds (measured: an 8-float query against a 128-dim brain returned `SFB_OK` with 3
hits). Added `sfb_recall_ex()` / `sfb_remember_ex()` that take `vec_len` and reject
anything but exactly `cfg.dim` with `SFB_E_DIM`; the plain entry points remain as
unchecked shims for source compatibility. `test_dims` (24 checks) pins it: short,
long, zero and `SIZE_MAX` rejected on write and on query, rejected writes create no
records, NaN/Inf still refused through the checked path, and the 64..4096 bounds hold.

**W7.5 — the v1 import is proven end-to-end.** The blocker was that this environment
cannot produce a v1 container it can decrypt: `qtsq_brain_save()` needs the OS
keystore (unavailable headlessly, `init` returns -3), password-encrypted containers do
not round-trip through a file (core returns -9 on read+decrypt), and `pack()` marks
security state so `qtsq_write_plaintext` refuses. The working recipe is
`qtsq_container_pack_write()`, the public plaintext streaming writer, which builds a
genuine v1 container **without** `pack()`: `test_migrate` now builds a 50x128 v1 brain
with it, migrates it, and asserts **50/50 records imported, 10/10 self-recall**, the
pre-migration `.v1backup` snapshot written, and the encrypted-brain refusals
(`v1_key_required`, `v1_decrypt_failed`) still exact. A second migration into a
populated store is refused (`dest_not_empty`) — without that guard, re-running migrate
silently duplicated every record.

**Phase gates measured** (`tests/test_gates.c`): 10k distinct texts → 10k records with
zero merges (Phase 2), and disk amplification on a 8k x 128 corpus.

**W3.6 — INT8 now actually reduces the footprint.** It did not before: with
`quantize_int8 = 1` the segments still stored float32 and the vector store held
*both* a float32 array and the int8 codes, so quantisation cost more RAM and saved
nothing on disk (measured: identical bytes, ratio 1.00x — my Phase-3 gate caught it).
Now an int8 segment sets flag bit 1 (`SFB_SEG_FLAG_INT8`) and stores int8 codes with
per-segment scale factors; readers dispatch per segment on that flag, so mixed
stores stay readable and an older build rejects such a segment loudly instead of
misparsing it. Measured on the same corpus written twice: **f32 4 348 286 B vs int8
1 292 286 B (3.36x whole store, 3.02x when sealed; 3.37x on the payload alone and
3.94x on the bytes each row actually holds in RAM)**, with the quantised store
reopening with every record and near-perfect self-recall. `tests/test_int8.c` (151
checks) also proves a corrupted int8 payload is caught by `verify` and dropped by
`repair`, and that a float32 store still opens.

### Incident: I deleted the test suite and rebuilt it from the transcripts
While clearing stale test binaries I ran `rm -f tests/test_*`, which matched the
**sources** as well as the binaries — every `tests/test_*.c` and `tests/test_serve.sh`
was destroyed (15 files; `sofuu/` is untracked, there were no snapshots, and `rm` does
not use the Trash). Nothing in the library or the CLI was affected.
Recovery, in order of what actually worked:
1. The harness keeps a compressed transcript per agent under
   `~/.dsh/sessions/<workspace>/<agent-id>/session.v4.jsonl.zstd`. Every `write` call
   carries the full file text and every `edit` call its old/new strings, so replaying
   all of them in timestamp order rebuilt all 15 files byte-for-byte in the parts that
   went through those tools.
2. Several files had later changes made through *bash* (python/sed one-liners). Those
   are also in the transcripts; I replayed the twelve that touched test files, in the
   same timestamp order, which restored the genuinely-current versions (e.g.
   `test_hnsw.c` came back at 839 checks only after its two patches — one of which
   attaches vectors before comparing answers, which is why the pre-patch version
   compared garbage).
3. Three files needed hand repair where an older API shape survived: `test_keys.c`
   (`sfb_count()` grew an out-parameter), `test_dims.c` (`sfb_remember_result_t` has
   `merged`, not `ok`) and `test_ops.c` (two assertions still expected a stub's "not
   implemented in this pass" message; the v1 path is real now).
Lesson recorded: delete build artefacts by explicit list or via the Makefile's
`clean` target — never a glob that also matches sources.

### Entry-level verification after recovery
`make test` is green end to end — util 350, crypto 35, store 1440, hnsw 839, lock 36,
text+quant, brain 116, ops 70, keys 140, migrate 34, dims 24, gates 10 017, int8 151,
plus `serve` 27 and `chaos` 7/7. Core drift re-checked: 0 mismatches against the
pre-session baseline, so nothing outside `sofuu/` moved.

### A durability bug the INT8 work uncovered (pre-existing, now fixed)
Chasing a failing INT8 retention check found a real defect in the f32 path, present
before any of this work: `serialize_dirty()` persists `r->flags` **verbatim**, so
`SFB_F_DIRTY` went to disk with every record and replay applied it back. Every
replayed row was then already dirty, `brain_mark_dirty()` early-returns for those,
`sfb_flush()` only serialises when the dirty list is non-empty, and so:
- `sfb_retain()` wrote nothing after a reopen and still returned `SFB_OK` — the
  tombstones came back on the next open (silent data resurrection), and
- the first real flush after any reopen re-wrote the whole corpus: **201 records /
  113 kB for one new row**, a 200x write amplification that no correctness test saw.

Fix is one format-neutral line in `sfb_brain_replay_segment_ex()` — replay drops the
flag, because on-disk is the truth, not RAM:
`flags &= ~(uint32_t)SFB_F_DIRTY;`
`tests/test_gates.c` now pins the visible invariant ("one write after a reopen writes
one record") and I mutation-tested it: with the mask disabled the gate fails with
`wrote 201 records`, with it restored it passes. The persisted bit itself was left
alone so old builds read the same bytes.

### Template for new entries

```
### YYYY-MM-DD — <short title>
- What changed (files)
- Why
- Test evidence (command → result)
- Follow-ups
```
