# Sofuu Subsystem for Quantum Tensor Sequence (QTSQ)

> **Subsystem:** `black-hole-disk/sofuu/`  
> **Author:** Haruhito  
> **Target:** Sofuu AI-Native JavaScript Runtime (`ai-native-js-runtime`)  
> **Purpose:** High-performance, zero-dependency, encrypted structured columnar table storage tailored specifically for Sofuu's session logs, agent traces, and application databases.

---

## 1. Overview & Non-Conflicting Design

This folder houses the dedicated, optimized structured table engine designed specifically for Sofuu.
It builds upon QTSQ's container format without modifying or conflicting with core QTSQ image/audio/splat pipelines.

### Key Architectural Invariants
1. **Isolated Namespace**: All functions and structs use the `sofuu_*` prefix (`sofuu_schema_t`, `sofuu_block_t`, `sofuu_table_t`).
2. **Zero Core Footprint Drift**: Core QTSQ headers (`include/qtsq.h`, `include/qtsq_format.h`) remain clean and unmodified.
3. **Weekly Rotation Support**: Native support for segmented weekly containers (`storage_YYYY_Www.qtsq`), avoiding multi-gigabyte monolithic databases.
4. **Hardware AES-256-GCM Encryption**: Built-in support for encrypted containers via QTSQ's authenticated password vault.

---

## 2. Core Optimizations for Sofuu

### A. Strict Schema Columnar Memory Layout
Traditional databases format rows as JSON text (`{"id": 1, "session_id": "s-1", ...}`), repeating column keys on every row and incurring high serialization overhead.
Sofuu QTSQ Tables store rows in **columnar chunks**:
- **Primitive Fixed-Width Columns** (`INT64`, `INT32`, `FLOAT64`, `FLOAT32`, `BOOL`, `TIMESTAMP`):  
  Stored as contiguous unpadded C arrays (`col_data[col_idx]`). This allows vectorized SIMD scans and compresses with extreme ratios (90%+) via delta encoding.
- **Variable-Width Columns** (`STRING`, `BYTES`, `JSON`):  
  Stored as Arrow-style contiguous byte heaps + 32-bit offset arrays (`str_offsets`), eliminating heap fragmentation and maximizing QTC compressor dictionary matches.

### B. Bounded Block Chunking (128 Rows per Block)
Rows are partitioned into fixed-size blocks (default: `SOFUU_ROWS_PER_BLOCK = 128`):
- Each block maintains boundary statistics:
  - `min_id` and `max_id`
  - `min_timestamp` and `max_timestamp`
  - `row_count`
- In multi-stream QTSQ containers, each block is serialized as an independent sub-stream:
  `table/<table_name>/blk_000000`, `table/<table_name>/blk_000001`, ...

### C. Checkpoint Indexing (Sub-Millisecond Range Scans)
The table index (`sofuu_block_checkpoint_t`) acts as a skip-list:
- When querying `WHERE timestamp >= T0 AND timestamp <= T1`:
  - The query engine scans only the 48-byte checkpoint headers.
  - If a block's `max_timestamp < T0` or `min_timestamp > T1`, the block is **skipped completely**.
  - In a 50,000-message history, querying the last 50 messages decompresses **only 1 block (< 128 rows)** instead of all 50,000 rows.

### D. Dual-Strategy Adaptive Compression & Zero-Error Fallback Ladder
Compressing tiny byte/KB payloads through heavy entropy/dictionary codecs often causes framing expansion, dictionary token rejection, or bit-size corruption. Sofuu implements an adaptive dual-strategy compressor:
1. **Sub-KB & Bit-Sized Data (< 1,024 bytes)**:
   - Completely bypasses dictionary/entropy models.
   - Stored directly as **White Dwarf (RAW `QTSQ_STRATEGY_NONE`)** with 32-byte SHA-256 integrity verification.
   - Eliminates framing expansion, guaranteeing 100% bit-exact durability for tiny 1-row tables, key-value settings, and small metadata.
2. **Larger Columnar Blocks ($\ge 1,024$ bytes)**:
   - Evaluated by QTSQ's **QTC compressor** (`qtc_compress`).
   - If QTC succeeds and achieves real compression (`compressed_len < raw_len`), the compressed block is saved under `QTSQ_CODEC_QTC`.
   - **Automatic Fallback**: If QTC returns any non-zero error OR fails to beat raw size, it instantly and silently falls back to White Dwarf RAW with **0.00% error rate**.
3. **Decompression Fallback Ladder**:
   - RAW streams are decoded via direct zero-decode memory copy.
   - QTC streams decode via `qtsq_codec_decompress`.
   - If any stream fails, it cascades through `qtsq_decompress` $\to$ `qtsq_decompress_horizon` $\to$ raw singularity copy.
   - **Zero data loss under any circumstance.**
4. **Binary & Non-JSON Data Safety**:
   - Binary blobs (containing `0x00`, `0xFF`, negative numbers, arbitrary byte streams) are preserved without passing through text-only `json_minify` cleaners.

---

## 3. Container Stream Layout

A Sofuu structured table persists as a standard QTSQ multi-stream container:

```text
storage_YYYY_Www.qtsq (Single AES-256-GCM Encrypted Container)
├── stream: "meta/schema"                 <-- Table schema, column definitions, row counts
├── stream: "table/messages/blk_000000"   <-- Rows 0..127 (columnar compressed)
├── stream: "table/messages/blk_000001"   <-- Rows 128..255 (columnar compressed)
├── stream: "table/messages/blk_000002"   <-- Rows 256..299 (active block)
└── stream: "kv/settings"                 <-- High-speed key-value document dictionary
```

---

## 4. API Reference

### Schema Initialization
```c
sofuu_schema_t schema;
sofuu_schema_init(&schema, "messages");
sofuu_schema_add_col(&schema, "id", SOFUU_TYPE_INT64, SOFUU_COL_PRIMARY_KEY | SOFUU_COL_AUTO_INC);
sofuu_schema_add_col(&schema, "session_id", SOFUU_TYPE_STRING, SOFUU_COL_REQUIRED | SOFUU_COL_INDEXED);
sofuu_schema_add_col(&schema, "role", SOFUU_TYPE_STRING, SOFUU_COL_REQUIRED);
sofuu_schema_add_col(&schema, "content", SOFUU_TYPE_STRING, SOFUU_COL_REQUIRED);
sofuu_schema_add_col(&schema, "timestamp", SOFUU_TYPE_TIMESTAMP, SOFUU_COL_REQUIRED);
```

### Table Insertion & In-Memory Working Buffer
```c
sofuu_table_t *tbl = sofuu_table_create(&schema);

sofuu_val_t row[5];
// fill row values...
sofuu_table_insert(tbl, row, 5);
```

### Checkpoint-Accelerated Query
```c
sofuu_query_t q;
sofuu_query_init(&q);
q.min_timestamp = 1729500000000LL;
q.max_timestamp = 1729500100000LL;
q.limit = 20;

sofuu_table_t *results = sofuu_table_query(tbl, &q);
// process results...
sofuu_table_free(results);
```

### Checkpoint Index & Selective Seek Query
```c
// Direct on-disk selective query without loading full table:
sofuu_query_t q;
sofuu_query_init(&q);
q.min_timestamp = 1729500000000;
q.limit = 50;
q.order_desc = true; // Sub-1.5ms reverse pagination

// Reads ONLY the matching columnar blocks via index/checkpoints:
sofuu_table_t *recent = sofuu_table_query_qtsq("storage.qtsq", "messages", "my-vault-pass", &q);
sofuu_table_free(recent);
```

### Container Packaging & Encryption
```c
// Plaintext write:
sofuu_table_write_qtsq(tbl, "storage.qtsq", NULL);

// Encrypted write (AES-256-GCM authenticated vault):
sofuu_table_write_qtsq(tbl, "storage.qtsq", "my-vault-pass");

// Decrypted read-back:
sofuu_table_t *loaded = sofuu_table_read_qtsq("storage.qtsq", "messages", "my-vault-pass");
```

---

## 5. Build, Test & Export Workflow

### Running Unit Tests
```bash
cd sofuu
make test
```

### Exporting to Sofuu Runtime
Run the automated export script:
```bash
./export_to_sofuu.sh
```
This syncs:
1. `sofuu_qtsq_table.h` $\to$ `ai-native-js-runtime/crates/sofuu-ffi/include/`
2. `libsofuu_qtsq.a` / `sofuu_qtsq_table.c` $\to$ `ai-native-js-runtime/crates/sofuu-ffi/`

When updating QTSQ optimizations in the future, iterate inside this folder, run `make test`, and execute `export_to_sofuu.sh`.
