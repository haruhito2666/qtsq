/* demo_wormhole.c — Inter-file deduplication demo */
#include <stdio.h>
#include <string.h>
#include "qtsq.h"
#include "../tests/test_util.h"

int main(void) {
    printf("=== QTSQ Demo: Wormholes (Inter-file Links) ===\n\n");

    /* Create a source file */
    qtsq_context_t source;
    qtsq_init(&source);

    const char *shared_data = "This data is shared across multiple files via wormholes.";
    qtsq_compress_text(&source, shared_data, strlen(shared_data));
    qtsq_test_encrypt_for_write(&source);
    qtsq_write(&source, "/tmp/wormhole_source.qtsq");
    printf("Source file: /tmp/wormhole_source.qtsq (%u bytes)\n", source.singularity_size);

    /* Create a file that links to the source */
    qtsq_context_t linked;
    qtsq_init(&linked);

    /* Compress some local data */
    const char *local = "Local data unique to this file.";
    qtsq_compress_text(&linked, local, strlen(local));
    qtsq_test_encrypt_for_write(&linked);
    printf("Linked file: %u bytes local data\n\n", linked.singularity_size);

    /* Add wormhole reference */
    int r = qtsq_add_wormhole(&linked, "/tmp/wormhole_source.qtsq", 0, source.singularity_size);
    if (r != QTSQ_OK) { printf("ERROR: %s\n", qtsq_error_string(r)); return 1; }

    printf("Wormhole added: ✅\n");
    printf("  Target: /tmp/wormhole_source.qtsq\n");
    printf("  Offset: 0, Size: %u bytes\n", source.singularity_size);
    printf("  Wormholes: %u\n", linked.num_wormholes);
    printf("  Flags: 0x%04X %s\n", linked.header.flags,
           (linked.header.flags & QTSQ_FLAG_HAS_WORMHOLES) ? "[WORMHOLES]" : "");

    /* Cleanup */
    remove("/tmp/wormhole_source.qtsq");
    qtsq_free(&source);
    qtsq_free(&linked);
    return 0;
}
