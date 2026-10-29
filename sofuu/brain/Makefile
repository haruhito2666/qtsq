# Makefile — Sofuu Brain v2 (sfb)
# Author: Haruhito
#
# Standalone: does not touch ../Makefile (the QTSQ subsystem table engine).
# Build:  make            → libsfb.a + sfb CLI
#         make test       → unit tests
#         make chaos      → SIGKILL / corruption / stampede runner
#         make clean

CC      ?= cc
ROOT    := ../..
CFLAGS  ?= -Wall -Wextra -O2 -g -std=c11 -D_GNU_SOURCE
CFLAGS  += -I. -I$(ROOT)/include -I$(ROOT)/crypto/common -I$(ROOT)/crypto/argon2
# -lz: qtsq_brain_load pulls in QTSQ's tensor/zlib paths
LDFLAGS += -L$(ROOT) -lqtsq -lz -lpthread -lm

UNAME := $(shell uname -s)
ifeq ($(UNAME),Darwin)
  LDFLAGS += -lc++
  # The v1 brain reader (qtsq_brain_load) pulls in QTSQ's keystore object, which
  # uses CoreFoundation/Security on macOS.
  LDFLAGS += -framework CoreFoundation -framework Security
endif

SRC = sfb_util.c sfb_crypto.c sfb_store.c sfb_hnsw.c sfb_lock.c \
      sfb_quant.c sfb_text.c sfb_brain.c sfb_ops.c sfb_broker.c sfb_hnsw_persist.c
OBJ = $(SRC:.c=.o)
LIB = libsfb.a
BIN = sfb

TESTS = test_util test_crypto test_store test_hnsw test_lock test_text_quant \
        test_brain test_ops test_keys test_migrate test_dims test_gates test_int8
TESTBINS = $(addprefix tests/,$(TESTS))

all: $(LIB) $(BIN)

$(LIB): $(OBJ)
	ar rcs $@ $(OBJ)

$(BIN): sfb_cli.c $(LIB)
	$(CC) $(CFLAGS) sfb_cli.c -L. -lsfb $(LDFLAGS) -o $@

%.o: %.c sfb.h sfb_internal.h sfb_broker.h
	$(CC) $(CFLAGS) -c $< -o $@

tests/%: tests/%.c $(LIB)
	$(CC) -DSFB_TEST_HAVE_CRYPTO $(CFLAGS) $< -L. -lsfb $(LDFLAGS) -o $@

tests/chaos_worker: tests/chaos_worker.c $(LIB)
	$(CC) $(CFLAGS) $< -L. -lsfb $(LDFLAGS) -o $@

test: $(TESTBINS) $(BIN) tests/chaos_worker
	@for t in $(TESTBINS); do echo "── $$t"; ./$$t || exit 1; done
	@chmod +x tests/test_serve.sh
	@echo "── tests/test_serve.sh"
	@BIN=./$(BIN) WORKER=./tests/chaos_worker bash tests/test_serve.sh || exit 1
	@echo "=== ALL SFB UNIT TESTS PASSED ==="

chaos: $(BIN) tests/chaos_worker
	@chmod +x tests/chaos.sh
	@BIN=./$(BIN) WORKER=./tests/chaos_worker bash tests/chaos.sh

clean:
	rm -f $(OBJ) $(LIB) $(BIN) $(TESTBINS) tests/chaos_worker tests/*.o
	rm -rf tests/tmp

.PHONY: all test chaos clean
