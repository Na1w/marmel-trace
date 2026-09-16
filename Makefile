# ---------------------------------------------------------------------------
# Raytracer - portable Makefile (macOS/clang and Linux/gcc)
#
# Targets:
#   make            build ./raytracer (DEFAULT: threaded, -DUSE_PTHREADS -pthread)
#   make serial     rebuild SERIAL (no pthread); alias: make no-threads
#   make no-threads alias for the serial opt-out build
#   make threads    rebuild threaded (backward-compatible alias for the default)
#   make run        build, then run ./raytracer with default arguments
#   make test       build + run every tests/*.c (nonzero exit on any failure)
#   make clean      remove objects, deps, the binary and bin/
#
# Threading is ON by default; override with THREADS=0 (e.g. `make all THREADS=0`)
# or use the `serial` / `no-threads` convenience targets.
# ---------------------------------------------------------------------------

CC        ?= cc

# Threading is the DEFAULT. Keep CFLAGS/LDFLAGS user-overridable by appending
# the thread flags to them rather than baking them into the `?=` defaults.
THREADS ?= 1
ifeq ($(THREADS),0)
THREAD_CFLAGS  :=
THREAD_LDFLAGS :=
else
THREAD_CFLAGS  := -DUSE_PTHREADS -pthread
THREAD_LDFLAGS := -pthread
endif

CFLAGS    ?= -std=c11 -O2 -Wall -Wextra
CFLAGS    += $(THREAD_CFLAGS)
CPPFLAGS  += -Isrc
LDFLAGS   ?=
LDFLAGS   += $(THREAD_LDFLAGS)
LDLIBS    += -lm            # MUST come AFTER objects on the link line

SRCS = src/vec3.c src/camera.c src/bmp.c src/noise.c src/geometry.c src/bvh.c src/material.c src/texture.c src/sampling.c src/pathtrace.c src/scene.c src/scene_desc.c src/scene_desc_write.c src/render.c
OBJS = $(SRCS:.c=.o) src/main.o
BIN  = raytracer

DEPS     = $(OBJS:.o=.d)
LIB_OBJS = $(filter-out src/main.o,$(OBJS))   # shared objects for tests (no main)

TEST_SRCS := $(wildcard tests/*.c)
TEST_BINS := $(patsubst tests/%.c,bin/%,$(TEST_SRCS))

.DEFAULT_GOAL := all
.PHONY: all threads serial no-threads run test clean

# --- default: build the program -------------------------------------------
all: $(BIN)

# --- link the program -----------------------------------------------------
$(BIN): $(OBJS)
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

# --- compile: one object per source, with header deps ---------------------
src/%.o: src/%.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -MMD -MP -c $< -o $@

# --- build one test binary per tests/*.c ----------------------------------
bin/%: tests/%.c $(LIB_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) $< $(LIB_OBJS) $(LDLIBS) -o $@

# --- threaded build (now the default; kept for backward compatibility) -----
threads:
	$(MAKE) clean
	$(MAKE) all THREADS=1

# --- serial opt-out build (no pthread) ------------------------------------
serial:
	$(MAKE) clean
	$(MAKE) all THREADS=0

no-threads:
	$(MAKE) clean
	$(MAKE) all THREADS=0

# --- run the renderer with default arguments ------------------------------
run: all
	./$(BIN)

# --- build and run all tests; fail the build on any failure ---------------
test: $(TEST_BINS)
	@if [ -z "$(strip $(TEST_SRCS))" ]; then \
		echo "no tests found"; \
	else \
		set -e; for t in $(TEST_BINS); do \
			echo "== $$t =="; \
			./$$t; \
		done; \
		echo "ALL TESTS PASSED"; \
	fi

# --- housekeeping ---------------------------------------------------------
clean:
	rm -f $(OBJS) $(DEPS) $(BIN)
	rm -rf bin

# --- automatic header dependency tracking (no-op on first build) ----------
-include $(DEPS)
