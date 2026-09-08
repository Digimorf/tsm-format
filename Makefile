# TSM - Temporal Signal Medium
# Builds the reference tools on anything with a C99 compiler.
#
# Copyright (c) 2026 Francesco De Simone
# SPDX-License-Identifier: Apache-2.0
#
#   make            build every tool into bin/
#   make check      build, then run each tool with no arguments to see that it
#                   starts and prints its usage
#   make clean      remove bin/ and obj/
#
# On Windows the same thing is in batch/build_v5_2_compact.bat, for people
# without make. Nothing here is Windows-specific: the tools are C99 and the
# only library they need is libm.

CC      ?= gcc
CFLAGS  ?= -std=c99 -O2 -Wall -Wextra
LDLIBS  ?= -lm

SRC     := src
OBJ     := obj
BIN     := bin

# Everything shared between the tools.
COMMON  := $(OBJ)/tsm_v5.o $(OBJ)/wav_io_simple.o $(OBJ)/indexed_common.o

# One program per source with a main() in it.
TOOLS   := \
	wav2tsm_indexed_v5 \
	tsm2wav_v5 \
	tsm_v5_indexed_audit \
	wav_tsm_advisor_v5_indexed \
	wav_gap_scan \
	wav2bits_kcs \
	wav_repair_kcs \
	wav_repair_kcs_framed \
	sc3000_wav_analyzer \
	sc3000_wav_adaptive_repair \
	sc3000_wav_guided_repair

TARGETS := $(addprefix $(BIN)/,$(TOOLS))

.PHONY: all check clean
all: $(TARGETS)

$(OBJ) $(BIN):
	mkdir -p $@

$(OBJ)/%.o: $(SRC)/%.c | $(OBJ)
	$(CC) $(CFLAGS) -c $< -o $@

$(BIN)/%: $(OBJ)/%.o $(COMMON) | $(BIN)
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)

# Each tool prints its usage when told nothing, and exits non-zero doing it.
# That is enough to catch a link that produced something which cannot run.
check: all
	@for t in $(TARGETS); do \
		printf '%-34s ' "$$t"; \
		"$$t" >/dev/null 2>&1; \
		if [ $$? -le 2 ]; then echo "runs"; else echo "FAILED"; exit 1; fi; \
	done

clean:
	rm -rf $(OBJ) $(BIN)
