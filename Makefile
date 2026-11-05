# msiqs: native C factoring with optional pthreads.
# Run from the repository root.
CC ?= cc
CPPFLAGS ?=
# 32-byte loop alignment avoids placement-sensitive GCC/x86 sieve slowdowns.
CFLAGS ?= -O3 -march=native -falign-loops=32
LDFLAGS ?=
LDLIBS ?= -lgmp -lm
PERL ?= perl
PYTHON ?= python3
EXTRA_FLAGS ?=
CHECK_ARGS ?=
TUNE_ARGS ?=

SOURCES = msiqs.c siqs.c lanczos.c pr32_iter.c tinyecm64.c
HEADERS = siqs.h lanczos.h siqs_choose_inc.c siqs_sieve_kernel_inc.c siqs_journal_inc.c \
	psiqs_inc.c planczos_inc.c ptypes.h mont64.h pr32_iter.h \
	tinyecm64.h
BUILD = $(PERL) tools/build-siqs.pl --cc "$(CC)" \
	--cppflags "$(CPPFLAGS)" --cflags "$(CFLAGS) $(EXTRA_FLAGS)" \
	--ldflags "$(LDFLAGS)" --ldlibs "$(LDLIBS)"
BUILD_DEPS = tools/build-siqs.pl tools/pthread-probe.c
BENCH_DEPS = $(SOURCES) $(HEADERS)

.PHONY: all siqs serial tuned tuned8 check test check-pr32 check-journal \
	check-autotune check-embedded clean

all: msiqs
siqs: msiqs

# Probe pthread support by linking, never running.
msiqs: $(SOURCES) $(HEADERS) $(BUILD_DEPS) Makefile
	@$(BUILD) --output $@ $(SOURCES)

# Explicit overrides rebuild even an otherwise up-to-date executable.
serial: $(SOURCES) $(HEADERS) $(BUILD_DEPS)
	@$(BUILD) --output msiqs --serial $(SOURCES)

tuned: bench-sieve tools/siqs-autotune.pl
	@tuned_flags=`$(PERL) tools/siqs-autotune.pl --flags $(TUNE_ARGS)` && \
	$(BUILD) --cflags "$(CFLAGS) $(EXTRA_FLAGS) $$tuned_flags" --output msiqs $(SOURCES)

tuned8:
	$(MAKE) tuned TUNE_ARGS='--threads 8'

bench-lanczos: tools/lanczos-bench.c lanczos.c lanczos.h planczos_inc.c ptypes.h $(BUILD_DEPS) Makefile
	@$(BUILD) --ldlibs "-lm" --output $@ tools/lanczos-bench.c

bench-solvers: tools/siqs-la-bench.c $(BENCH_DEPS) $(BUILD_DEPS) Makefile
	@$(BUILD) --serial --output $@ tools/siqs-la-bench.c siqs.c pr32_iter.c tinyecm64.c

bench-sieve: tools/siqs-sieve-bench.c tools/siqs-autotune.inc.c $(BENCH_DEPS) $(BUILD_DEPS) Makefile
	@$(BUILD) --output $@ tools/siqs-sieve-bench.c lanczos.c pr32_iter.c tinyecm64.c

# The main and embedded runners manage their temporary-directory cleanup.
# To force serial checks: make check CHECK_ARGS=--serial
check:
	$(PERL) tests/siqs-check.pl --cc "$(CC)" --cflags "$(CPPFLAGS) $(CFLAGS) $(EXTRA_FLAGS)" --ldflags "$(LDFLAGS) $(LDLIBS)" $(CHECK_ARGS)

test: check

# Install cleanup before compiling, and preserve a failing command's status.
check-pr32: tests/pr32-iter-check.c pr32_iter.c pr32_iter.h Makefile
	@trap 'rm -f check-pr32' 0; \
	$(CC) $(CPPFLAGS) $(CFLAGS) $(EXTRA_FLAGS) -o check-pr32 tests/pr32-iter-check.c $(LDFLAGS) && \
	./check-pr32

check-journal:
	@journal_dir="$${SIQS_JOURNAL_TEST_DIR:-}"; \
	if [ -z "$$journal_dir" ]; then \
	  journal_dir=$$(mktemp -d "$${TMPDIR:-/tmp}/msiqs-journal-check.XXXXXXXX") || exit $$?; \
	fi; \
	trap 'for journal_bin in msiqs msiqs-serial journal-seeds journal-attempts journal-graph; do rm -f "$$journal_dir/$$journal_bin"; done' 0; \
	SIQS_JOURNAL_TEST_DIR="$$journal_dir" $(PYTHON) tests/journal/run-all.py

check-autotune: tests/siqs-autotune-check.c tools/siqs-sieve-bench.c tools/siqs-autotune.inc.c $(BENCH_DEPS) $(BUILD_DEPS) Makefile
	@trap 'rm -f check-autotune' 0; \
	$(BUILD) --output check-autotune tests/siqs-autotune-check.c lanczos.c pr32_iter.c tinyecm64.c && \
	./check-autotune && \
	$(PERL) tests/siqs-autotune.t

check-embedded:
	$(PYTHON) tests/check-embedded.py

clean:
	rm -f msiqs bench-lanczos bench-solvers bench-sieve check-pr32 check-autotune
