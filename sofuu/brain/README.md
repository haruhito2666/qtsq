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
cfg.dimensions 64..4096;
cfg.decay = SFB_DECAY_EXPONENTIAL | SFB_DECAY_NONE | SFB_DECAY_TTL;
cfg.dedup_text_similarity = 0.85;  cfg.dedup_action = SFB_DEDUP_MERGE;
cfg.read_only = 0;  cfg.key = "optional passphrase";  cfg.capacity = 100000;

sfb_brain_t *b;
if (sfb_open(&cfg, &b) != SFB_OK) { /* SFB_E_LOCKED, SFB_E_CORRUPT, ... */ }

sfb_remember_result_t r;
sfb_remember(b, vec, "text", "user", &r);   /* r.merged / r.into are honest */

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
| One bit-flip costs one record, not 256 | per-record CRC32C in the segment payload (W3.5) |
| No silent short writes | staged size checked before publish; `EFBIG`/`ENOSPC` ⇒ `SFB_E_ENOSPC` (W1.6) |
| One writer, many readers | `flock(LOCK_EX/LOCK_SH)` + pid record; stale pids reclaimed (W2.1–2.3) |
| Multi-agent without lock churn | `sfb_broker_run()` on a UNIX socket (W2.4) |
| Exact match wins in text search | RRF + exact-phrase boost + lexical dominance guard (W4.6) |
| Recall does not collapse with size | diversity-aware neighbour selection + bidirectional repair (W4.1–4.2) |
| Open does not rebuild the graph | `sfb_hnsw_map()` maps the topology file and points the arrays at it (W3.1) |
| Memory stays proportional to *used* levels | pooled link rows, 1.07 rows/node instead of 33 |
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
| INT8 footprint (persisted) | **3.36x smaller** whole store (3.02x sealed) | 4x payload |
| exact dimension enforcement | wrong-length write/query -> `SFB_E_DIM`, no OOB read | refused |
| unit checks | **~13,250 across 12 suites** + 27 socket checks + 7 chaos cases | all pass |

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
- INT8 stores int8 codes plus a per-segment scale table. It is a footprint/write
  optimisation, not a recall gain: quantised vectors are dequantised for the graph,
  so search quality is unchanged but a quantised brain is not bit-identical to a
  float32 one at large scale.
- `sfb serve` handles one client at a time and was exercised only on Darwin;
  `flock` semantics over NFS/SMB and power-loss (as opposed to process-crash)
  durability are still untested.

## Migration from a v1 brain

`sfb migrate <v1.qtsq> <brain-dir> [--key <passphrase>]` reads the container, imports
the `memories` stream, writes a `.v1backup` snapshot and refuses a non-empty
destination. The happy path is proven end-to-end in `test_migrate` (50x128 v1 built
with `qtsq_container_pack_write()`, 50/50 records, 10/10 self-recall), not just the
refusal paths; encrypted v1 brains require `--key` and a wrong key is refused.
