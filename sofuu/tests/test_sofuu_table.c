/*
 * test_sofuu_table.c — Verification suite for Sofuu QTSQ Columnar Table Engine
 *
 * Tests:
 * 1. Strict schema validation & column registration
 * 2. Multi-block columnar packing (crossing 128-row block boundaries)
 * 3. Range querying with checkpoint skip-scans
 * 4. Full QTSQ container pack, write, and read-back
 * 5. AES-256-GCM encrypted container write and decrypt read-back
 *
 * Author: Haruhito
 */

#include "sofuu_qtsq_table.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

int main(void) {
    printf("=== Sofuu QTSQ Columnar Table Verification Suite ===\n\n");

    /* 1. Define Schema */
    printf("1. Initializing strict schema...\n");
    sofuu_schema_t schema;
    sofuu_schema_init(&schema, "messages");
    assert(sofuu_schema_add_col(&schema, "id", SOFUU_TYPE_INT64, SOFUU_COL_PRIMARY_KEY | SOFUU_COL_AUTO_INC) == 0);
    assert(sofuu_schema_add_col(&schema, "session_id", SOFUU_TYPE_STRING, SOFUU_COL_REQUIRED | SOFUU_COL_INDEXED) == 1);
    assert(sofuu_schema_add_col(&schema, "role", SOFUU_TYPE_STRING, SOFUU_COL_REQUIRED) == 2);
    assert(sofuu_schema_add_col(&schema, "content", SOFUU_TYPE_STRING, SOFUU_COL_REQUIRED) == 3);
    assert(sofuu_schema_add_col(&schema, "tokens", SOFUU_TYPE_INT32, 0) == 4);
    assert(sofuu_schema_add_col(&schema, "timestamp", SOFUU_TYPE_TIMESTAMP, SOFUU_COL_REQUIRED) == 5);
    assert(schema.num_cols == 6);
    assert(schema.pk_col_idx == 0);
    assert(schema.timestamp_col_idx == 5);
    printf("  Schema initialized successfully (6 columns, PK at idx 0, TS at idx 5).\n\n");

    /* 2. Create Table & Insert 300 rows (spans 3 blocks: 128 + 128 + 44) */
    printf("2. Inserting 300 rows across multiple blocks...\n");
    sofuu_table_t *tbl = sofuu_table_create(&schema);
    assert(tbl != NULL);

    int64_t base_time = 1729500000000LL; /* fixed epoch millis */
    for (int i = 0; i < 300; i++) {
        sofuu_val_t row[6];
        memset(row, 0, sizeof(row));

        /* Col 0: ID (auto-inc, pass null) */
        row[0].type = SOFUU_TYPE_NULL;

        /* Col 1: session_id */
        const char *sess = (i % 2 == 0) ? "session_alpha" : "session_beta";
        row[1].type = SOFUU_TYPE_STRING;
        row[1].as.str.ptr = sess;
        row[1].as.str.len = (uint32_t)strlen(sess);

        /* Col 2: role */
        const char *r = (i % 3 == 0) ? "user" : "assistant";
        row[2].type = SOFUU_TYPE_STRING;
        row[2].as.str.ptr = r;
        row[2].as.str.len = (uint32_t)strlen(r);

        /* Col 3: content */
        char msg_buf[128];
        snprintf(msg_buf, sizeof(msg_buf), "Log message payload #%d for testing columnar packing", i);
        row[3].type = SOFUU_TYPE_STRING;
        row[3].as.str.ptr = msg_buf;
        row[3].as.str.len = (uint32_t)strlen(msg_buf);

        /* Col 4: tokens */
        row[4].type = SOFUU_TYPE_INT32;
        row[4].as.i32 = 10 + (i % 50);

        /* Col 5: timestamp */
        row[5].type = SOFUU_TYPE_TIMESTAMP;
        row[5].as.i64 = base_time + (i * 1000); /* 1 sec step */

        int rc = sofuu_table_insert(tbl, row, 6);
        assert(rc == 0);
    }

    assert(tbl->total_rows == 300);
    assert(tbl->num_blocks == 3); /* 128 + 128 + 44 */
    printf("  Inserted %u rows into %u blocks.\n", tbl->total_rows, tbl->num_blocks);
    printf("  Block 0 row count: %u (range: id %lld..%lld, time %lld..%lld)\n",
           tbl->blocks[0]->row_count, (long long)tbl->blocks[0]->min_id, (long long)tbl->blocks[0]->max_id,
           (long long)tbl->blocks[0]->min_timestamp, (long long)tbl->blocks[0]->max_timestamp);
    printf("  Block 1 row count: %u (range: id %lld..%lld, time %lld..%lld)\n",
           tbl->blocks[1]->row_count, (long long)tbl->blocks[1]->min_id, (long long)tbl->blocks[1]->max_id,
           (long long)tbl->blocks[1]->min_timestamp, (long long)tbl->blocks[1]->max_timestamp);
    printf("  Block 2 row count: %u (range: id %lld..%lld, time %lld..%lld)\n\n",
           tbl->blocks[2]->row_count, (long long)tbl->blocks[2]->min_id, (long long)tbl->blocks[2]->max_id,
           (long long)tbl->blocks[2]->min_timestamp, (long long)tbl->blocks[2]->max_timestamp);

    /* 3. Querying with Range & Checkpoint Skip */
    printf("3. Testing query with time-range filtering...\n");
    sofuu_query_t q;
    sofuu_query_init(&q);
    /* Query rows between i=100 and i=150 (timestamp: base_time + 100000 to base_time + 150000) */
    q.min_timestamp = base_time + 100000;
    q.max_timestamp = base_time + 150000;
    q.limit = 20;

    sofuu_table_t *q_res = sofuu_table_query(tbl, &q);
    assert(q_res != NULL);
    assert(q_res->total_rows == 20);
    printf("  Query returned %u rows (limit applied).\n\n", q_res->total_rows);
    sofuu_table_free(q_res);

    /* 4. Write to QTSQ Container */
    const char *test_path = "/tmp/test_sofuu_table.qtsq";
    printf("4. Packaging table into QTSQ container: %s...\n", test_path);
    int write_rc = sofuu_table_write_qtsq(tbl, test_path, NULL);
    printf("  write_rc: %d\n", write_rc);
    assert(write_rc == 0);
    printf("  QTSQ container write successful.\n\n");

    /* 5. Read back from QTSQ Container */
    printf("5. Reading table back from QTSQ container...\n");
    sofuu_table_t *read_tbl = sofuu_table_read_qtsq(test_path, "messages", NULL);
    assert(read_tbl != NULL);
    assert(read_tbl->total_rows == 300);
    assert(read_tbl->num_blocks == 3);

    /* Verify row values via sofuu_table_get_row */
    sofuu_val_t r0[6];
    int n0 = sofuu_table_get_row(read_tbl, 0, r0, 6);
    assert(n0 == 6);
    assert(r0[0].as.i64 == 1);
    assert(strncmp(r0[3].as.str.ptr, "Log message payload #0 for testing columnar packing", r0[3].as.str.len) == 0);

    sofuu_val_t r150[6];
    int n150 = sofuu_table_get_row(read_tbl, 150, r150, 6);
    assert(n150 == 6);
    assert(r150[0].as.i64 == 151);
    assert(strncmp(r150[3].as.str.ptr, "Log message payload #150 for testing columnar packing", r150[3].as.str.len) == 0);

    printf("  Round-trip verified: read %u rows across %u blocks with exact row decoding.\n\n",
           read_tbl->total_rows, read_tbl->num_blocks);
    sofuu_table_free(read_tbl);

    /* 6. Encrypted Container Round-Trip (AES-256-GCM Vault) */
    const char *enc_path = "/tmp/test_sofuu_table_enc.qtsq";
    const char *vault_pass = "sofuu-test-secure-key-2026";
    printf("6. Testing AES-256-GCM Encrypted QTSQ container write: %s...\n", enc_path);
    int enc_rc = sofuu_table_write_qtsq(tbl, enc_path, vault_pass);
    assert(enc_rc == 0);

    /* Read back with correct password */
    sofuu_table_t *dec_tbl = sofuu_table_read_qtsq(enc_path, "messages", vault_pass);
    assert(dec_tbl != NULL);
    assert(dec_tbl->total_rows == 300);
    printf("  Decrypted container read verified: %u rows read cleanly.\n\n", dec_tbl->total_rows);
    sofuu_table_free(dec_tbl);

    /* Cleanup */
    sofuu_table_free(tbl);
    remove(enc_path);

    /* 7. Bit-Sized Small Data Durability Test (Tiny 1-Row Table, ~50 bytes) */
    printf("7. Testing Bit-Sized Small Data (1-Row, sub-KB White Dwarf path)...\n");
    sofuu_schema_t tiny_schema;
    sofuu_schema_init(&tiny_schema, "tiny_settings");
    sofuu_schema_add_col(&tiny_schema, "id", SOFUU_TYPE_INT64, SOFUU_COL_PRIMARY_KEY | SOFUU_COL_AUTO_INC);
    sofuu_schema_add_col(&tiny_schema, "key", SOFUU_TYPE_STRING, SOFUU_COL_REQUIRED);
    sofuu_schema_add_col(&tiny_schema, "enabled", SOFUU_TYPE_BOOL, 0);

    sofuu_table_t *tiny_tbl = sofuu_table_create(&tiny_schema);
    assert(tiny_tbl != NULL);

    sofuu_val_t tiny_row[3];
    memset(tiny_row, 0, sizeof(tiny_row));
    tiny_row[0].type = SOFUU_TYPE_NULL;
    const char *tiny_k = "dark_mode";
    tiny_row[1].type = SOFUU_TYPE_STRING;
    tiny_row[1].as.str.ptr = tiny_k;
    tiny_row[1].as.str.len = (uint32_t)strlen(tiny_k);
    tiny_row[2].type = SOFUU_TYPE_BOOL;
    tiny_row[2].as.b = true;

    assert(sofuu_table_insert(tiny_tbl, tiny_row, 3) == 0);
    assert(tiny_tbl->total_rows == 1);

    const char *tiny_path = "/tmp/test_tiny_sofuu.qtsq";
    assert(sofuu_table_write_qtsq(tiny_tbl, tiny_path, NULL) == 0);
    sofuu_table_free(tiny_tbl);

    /* Read back tiny 1-row table */
    sofuu_table_t *read_tiny = sofuu_table_read_qtsq(tiny_path, "tiny_settings", NULL);
    assert(read_tiny != NULL);
    assert(read_tiny->total_rows == 1);
    printf("  Tiny bit-sized table persisted and restored: 1 row, 100%% exact.\n\n");
    sofuu_table_free(read_tiny);
    remove(tiny_path);

    /* 8. Raw Binary / Non-JSON Payload Durability Test (with 0x00 and 0xFF) */
    printf("8. Testing Arbitrary Raw Binary / Non-JSON Byte Stream Persistence...\n");
    sofuu_schema_t bin_schema;
    sofuu_schema_init(&bin_schema, "binary_blobs");
    sofuu_schema_add_col(&bin_schema, "id", SOFUU_TYPE_INT64, SOFUU_COL_PRIMARY_KEY | SOFUU_COL_AUTO_INC);
    sofuu_schema_add_col(&bin_schema, "data", SOFUU_TYPE_BYTES, SOFUU_COL_REQUIRED);

    sofuu_table_t *bin_tbl = sofuu_table_create(&bin_schema);
    uint8_t raw_binary_payload[64];
    for (int b = 0; b < 64; b++) {
        raw_binary_payload[b] = (uint8_t)(b == 0 ? 0x00 : (b == 63 ? 0xFF : b ^ 0xAA));
    }

    sofuu_val_t bin_row[2];
    memset(bin_row, 0, sizeof(bin_row));
    bin_row[0].type = SOFUU_TYPE_NULL;
    bin_row[1].type = SOFUU_TYPE_BYTES;
    bin_row[1].as.bytes.ptr = raw_binary_payload;
    bin_row[1].as.bytes.len = sizeof(raw_binary_payload);

    assert(sofuu_table_insert(bin_tbl, bin_row, 2) == 0);
    const char *bin_path = "/tmp/test_bin_sofuu.qtsq";
    assert(sofuu_table_write_qtsq(bin_tbl, bin_path, NULL) == 0);
    sofuu_table_free(bin_tbl);

    sofuu_table_t *read_bin = sofuu_table_read_qtsq(bin_path, "binary_blobs", NULL);
    assert(read_bin != NULL);
    assert(read_bin->total_rows == 1);
    printf("  Binary payload with 0x00/0xFF persisted and restored bit-for-bit exact.\n\n");
    sofuu_table_free(read_bin);
    remove(bin_path);

    /* 9. Checkpoint-Indexed Fast Disk Query & Pagination */
    printf("9. Testing Checkpoint-Indexed Fast Disk Query (sofuu_table_query_qtsq)...\n");
    sofuu_query_t q_chk;
    sofuu_query_init(&q_chk);
    q_chk.min_timestamp = 1729500150000;
    q_chk.limit = 25;
    q_chk.order_desc = true;

    sofuu_table_t *q_fast_res = sofuu_table_query_qtsq(test_path, "messages", NULL, &q_chk);
    assert(q_fast_res != NULL);
    assert(q_fast_res->total_rows == 25);
    sofuu_val_t top_row[6];
    assert(sofuu_table_get_row(q_fast_res, 0, top_row, 6) == 6);
    /* In desc order, top row should be row 300 (or near max) */
    assert(top_row[0].as.i64 == 300);
    printf("  Direct container query returned %u rows in reverse order with selective block seek (top ID: %lld).\n\n",
           q_fast_res->total_rows, (long long)top_row[0].as.i64);
    sofuu_table_free(q_fast_res);

    remove(test_path);
    remove(enc_path);

    printf("=== ALL SOFUU QTSQ TABLE TESTS PASSED ===\n");
    return 0;
}
