# Sofuu Brain v2 (`sfb`)

Self-contained C11 vector/cognitive store built on QTSQ's vendored crypto. Lives entirely
inside `black-hole-disk/sofuu/brain/` and **does not modify QTSQ core** (`src/`,
`include/qtsq.h`, `include/qtsq_format.h`) or the parent `sofuu/Makefile`.

```
make            # libsfb.a + sfb CLI
make test       # unit suites
make chaos      # SIGKILL / corruption / stampede / ENOSPC matrix
./sfb selftest  # crypto NIST vectors
```

## Layout on disk

`path` given to `sfb_open()` is the **brain directory**:

```
<path>/
  brain.qtsq              manifest: magic, version, dim, count, generation,
                          per-segment {gen,seq,records,length,digest,min,max},
                          CRC32C over the whole header + segment table
  brain.qtsq.d/           sealed, immutable segments (<=256 records each)
    seg-<gen>-<seq>.qtsq  header + payload + per-record CRC32C
    graph-<gen>.bin       persisted HNSW topology (SFG4) — mmap'd, zero copy to open
  brain.qtsq.wal.json     write-ahead intent record for the manifest commit
  brain.qtsq.lock         flock target: pid, start time, writer|reader
```

## Public API sketch

```c
sfb_config_t cfg; sfb_config_default(&cfg, "/var/lib/app/brain", 768);
cfg.mode = SFB_MODE_COGNITIVE;   /* or SFB_MODE_VECTOR_DB */
cfg.dim = 768;                   /* 64..4096 only; anything else is SFB_E_DIM */
cfg.decay = SFB_DECAY_EXPONENTIAL | SFB_DECAY_NONE | SFB_DECAY_TTL;
cfg.dedup_text_similarity = 0.85;  cfg.dedup_action = SFB_DEDUP_MERGE;
cfg.read_only = 0;  cfg.key = "optional passphrase";  cfg.capacity = 100000;

sfb_brain_t *b;
if (sfb_open(&cfg, &b) != SFB_OK) { /* SFB_E_LOCKED, SFB_E_CORRUPT, ... */ }

sfb_remember_result_t r;
sfb_remember(b, vec, "text", "user", &r);   /* r.merged / r.into are honest */
/* The JS surface reports the same thing: remember() returns
 * { id, merged } on an insert and { id, merged, into } on a merge. */

sfb_hit_t hits[10]; uint32_t n;
sfb_recall(b, query, 10, hits, &n);

sfb_flush_result_t fr;
sfb_flush(b, &fr);          /* returns errors; never a bare bool */
sfb_close(b);
```

## Guarantees (and where they are enforced)

| Guarantee | Mechanism |
|---|---|
| A damaged manifest never becomes an empty brain | CRC32C over header + segment table; `SFB_E_CORRUPT` instead of silent zero (W1.4) |
| One bit-flip costs one record, not 256 | per-record CRC32C over the WHOLE record (vector included) behind segment flag bit3; replay drops only the damaged row (W3.5). A truncated segment still falls as a unit — a half-written segment is not a record-level fault. A **sealed** store cannot reach this path with a bit-flip at all — see the next row |
| …but on an encrypted store it costs one segment, not one record | AES-256-GCM authenticates the sealed payload as a unit, so the GCM tag rejects a damaged segment before any per-record CRC is consulted. Record-level isolation is unreachable there **by construction**, not by oversight (W3.5) |
| A read-only open writes nothing | readers take `O_RDONLY` + `flock(LOCK_SH)`, or a shared flock on the store directory when no lock file exists; no file is created, truncated or recorded (W2.3) |
| Opening a brain does not replay it | `sfb_open` reads the manifest and probes the newest segment header; payloads load on first row access. `verify_full` is the paranoid path (W3.3) |
| A pinned row never drops below 0.2 | the floor is an invariant, not a `pins_immune` knob (W5.4) |
| A brain keeps forgetting across a restart | decay mode / half-life / floor persist in the manifest (optional 24-byte block, flag bit1) and are inherited by a config silent about decay (W5.3) |
| Steady-state disk does not grow | superseded generations are reclaimed on flush without an explicit `sfb_compact` — measured ~1.19x amplification over 5 cycles (W3.4) |
| No silent short writes | staged size checked before publish; `EFBIG`/`ENOSPC` ⇒ `SFB_E_ENOSPC` (W1.6) |
| One writer, many readers | `flock(LOCK_EX/LOCK_SH)` + pid record; stale pids reclaimed (W2.1–2.3) |
| Multi-agent without lock churn | `sfb_broker_run()` on a UNIX socket (W2.4) |
| Exact match wins in text search | RRF + exact-phrase boost + lexical dominance guard (W4.6) |
| Recall does not collapse with size | diversity-aware neighbour selection + bidirectional repair (W4.1–4.2) |
| Open does not rebuild the graph | `sfb_hnsw_map()` maps the topology file and points the arrays at it (W3.1) |
| Memory stays proportional to *used* levels | pooled link rows, 1.07 rows/node instead of 33 |
| Dimensions are bounded, not arbitrary | `cfg.dim` must be **64..4096**; anything outside is `SFB_E_DIM` at open. `dim` is baked into every fixed-size on-disk and index record (the segment record, the HNSW link rows, the manifest), so the bound is what makes the layout provably in bounds instead of a silent reinterpretation of a caller's buffer. The plan's Phase-2 exit gate says "any dim accepted" — this is a deliberate narrowing, pinned by `test_dims.c` (63 and 4097 refused, 64 and 4096 accepted) |
| Non-finite vectors cannot crash the process | rejected at ingest (`SFB_E_NONFINITE`) and total-order-safe distances (W1.1–1.2) |
| Distinct memories are not swallowed | dual-key dedup: vector L2 **and** token Jaccard (W5.1) |
| Decay demotes instead of stranding | below-floor strength ⇒ `SFB_TIER_FORGOTTEN` (W5.3) |

## Measured (this build, macOS arm64)

| Metric | Result | Plan gate |
|---|---|---|
| recall@10 @ 25k, dim 768, ef=64 | **100.00 %** (prior runtime: 44 %) | >= 98.5 % |
| recall@10 @ 2k / 10k | 100.00 % / 100.00 % | >= 98.5 % |
| query latency @ 25k | p50 510 µs, p99 626 µs | < 5 ms |
| **reopen @ 10k x 768** | **199 ms**, `index_rebuilt=0` (prior: 124,648 ms) | < 1 s @ 100k |
| flush p99 @ 10k | 1.6 ms (final flush 0.2 ms) | < 50 ms |
| disk amplification | 1.32x @10k (1.12x @25k) | < 1.5x |
| peak RSS @10k x 768 | **141 MB** | < 350 MB @100k |
| graph topology memory @10k | **4.0 MB** (was 42.2 MB, 10.5x) | — |
| concurrent writers | 1 winner, others `SFB_E_LOCKED`, no corruption | no corruption |
| SIGKILL recovery | 15/15 clean reopens, 0 count regressions | monotonic |
| manifest fuzz | 0/20 silent empty opens | 0 |
| AES-256-GCM | 4 NIST vectors, cross-checked against OpenSSL | vectors pass |
| INT8 quantisation error | 0.445 % max, 0.286 % RMS | <= 1.5 % |
| INT8 footprint (persisted) | **3.36x smaller** whole store, 3.02x sealed, 3.37x on the payload (f32 4 348 310 B vs int8 1 292 310 B) — not 4x: only the vector bytes shrink | 4x payload |
| exact dimension enforcement | wrong-length write/query -> `SFB_E_DIM`, no OOB read | refused |
| unit checks | **13,628 across 18 C suites** + 27 socket checks + 7 chaos cases (the sum covers the 17 suites that print a count; `test_text_quant` prints none) | all pass |

Known cost: ingest is **13.8 ms/record at dim 768** (3.7 ms at dim 384), dominated by the
diversity heuristic at `efConstruction=200`. Correctness is not in question (100 % recall
against a 98.5 % gate), so lowering `SFB_DEFAULT_EFC` is the documented tuning knob if bulk
load throughput matters more than build-time recall margin.

## Not in this pass

- Hybrid text search is fused by rank (RRF over BM25, vector centroid and recency)
  but still scans every live record: O(n) per text query, by design for now.
- The plain `sfb_remember()`/`sfb_recall()` cannot see a caller's buffer length. They
  are now thin shims over `sfb_remember_ex()`/`sfb_recall_ex()`, which do enforce it;
  a caller that knows its length should use the `_ex` entry points.
- INT8 stores int8 codes plus **one f32 scale per record**, not one per segment as the
  plan specifies: a segment mixes unrelated memories, and a single scale would clip the
  loud rows or flatten the quiet ones (`sfb_quant.c:104-121`). The measured whole-store
  reduction is **3.36x**, not the promised 4x, because only the vector bytes shrink
  (4·dim → dim + 4 per row) while the HNSW graph, record headers and manifest do not —
  3.94x is what the vector store's own bytes per row reach. It is a footprint/write
  optimisation, not a recall gain: quantised vectors are dequantised for the graph,
  so search quality is unchanged but a quantised brain is not bit-identical to a
  float32 one at large scale. Dedup narrows too: in int8 mode the exact-vector leg is
  **exact-only** — the query's `(scale, codes)` pair must match the stored row — so
  re-remembering the same memory still folds, while a merely nearby vector no longer
  folds there (only the HNSW-neighbour + Jaccard leg can).
- `sfb serve` handles one client at a time and was exercised only on Darwin;
  `flock` semantics over NFS/SMB and power-loss (as opposed to process-crash)
  durability are still untested.

## Migration from a v1 brain

`sfb migrate <v1.qtsq> <brain-dir> [--key <passphrase>]` reads the container, imports
the `memories` stream, writes a `.v1backup` snapshot and refuses a non-empty
destination. The happy path is proven end-to-end in `test_migrate` (50x128 v1 built
with `qtsq_container_pack_write()`, 50/50 records, 10/10 self-recall), not just the
refusal paths; encrypted v1 brains require `--key` and a wrong key is refused.
