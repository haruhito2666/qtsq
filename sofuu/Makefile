# Makefile — Sofuu Subsystem for QTSQ
# Author: Haruhito

CC       = cc
CFLAGS   = -Wall -Wextra -O3 -std=c11 -I. -I../include -I../compressor/include -I../crypto/common -I../crypto/ml-kem-768 -I../crypto/ml-dsa-65 -I../crypto/argon2
LDFLAGS  = -L.. -lqtsq -L../compressor -lqtc -lz -lm -lpthread

OPUS_LIBS := $(shell pkg-config opus --libs 2>/dev/null)

UNAME := $(shell uname -s)
ifeq ($(UNAME),Darwin)
  LDFLAGS += $(OPUS_LIBS) -lc++ -framework CoreFoundation -framework Security \
             -framework Metal -framework Foundation \
             -framework AVFoundation -framework CoreBluetooth \
             -framework CoreAudio -framework CoreMedia -framework AudioToolbox
else
  LDFLAGS += $(OPUS_LIBS) -lc++
endif

OBJ = sofuu_qtsq_table.o
LIB = libsofuu_qtsq.a
TEST = tests/test_sofuu_table

all: $(LIB) $(TEST)

$(LIB): $(OBJ)
	ar rcs $@ $(OBJ)

sofuu_qtsq_table.o: sofuu_qtsq_table.c sofuu_qtsq_table.h
	$(CC) $(CFLAGS) -c sofuu_qtsq_table.c -o $@

$(TEST): tests/test_sofuu_table.c $(LIB) ../libqtsq.a
	$(CC) $(CFLAGS) tests/test_sofuu_table.c -L. -lsofuu_qtsq $(LDFLAGS) -o $@

test: $(TEST)
	./$(TEST)

clean:
	rm -f $(OBJ) $(LIB) $(TEST) tests/*.o

.PHONY: all test clean
