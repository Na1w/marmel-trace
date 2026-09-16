# Research Note — C Build Tooling & a Minimal Test Harness

**Task:** `t-007` — Research C build tooling & test harness conventions.
**Consumers:** the project `Makefile` (t-015) and `tests/*.c` (t-017, t-018, t-019).
**Scope (deliberately tight):** a minimal portable Makefile pattern for a multi-file C project,
recommended compiler/linker flags, `-lm` placement, a tiny assert-based unit-test harness, and
macOS/clang ↔ Linux/gcc portability notes. **Not covered:** BMP format, rendering algorithms,
math, shading.

Everything below was validated on the local toolchain before writing:
`Apple clang version 17.0.0 (arm64-apple-darwin25.6.0)`, `GNU Make 3.81`. The complete example
in §5 was built and run end-to-end (compile, link, `make test`, failing-test propagation, header
rebuild) and all snippets are copy-pasteable.

---

## 1. A minimal, portable Makefile pattern

### 1.1 Design rules

1. **Use only POSIX-standard make features.** The default `make` on macOS is GNU Make 3.81 and the
   default on Linux is GNU Make 4.x; both support everything below. Avoid GNU-only extensions
   (e.g. `$(shell …)` is fine but unnecessary here, `ifeq` is fine but keep it minimal).
2. **Recursive wildcards are not portable.** Use `$(wildcard src/*.c)`, never `**/*.c`.
3. **Let built-in rules help where harmless**, but define explicit pattern rules so behaviour is
   identical on both platforms.
4. **Keep source (`src/`), objects (`build/`), and binaries (`bin/`) separate** so `clean` is a
   single `rm -rf` and source trees stay pristine.
5. **Automatic header dependency tracking** via `-MMD -MP` + `-include $(DEPS)` — see §1.4.

### 1.2 Variables

```make
CC       ?= cc              # clang on macOS, gcc on Linux — '?=' respects an env override
CFLAGS   ?= -std=c11 -O2 -Wall -Wextra -Wpedantic -Wshadow
LDFLAGS  ?=
LDLIBS   ?= -lm            # math library; MUST come after objects on the link line (see §2.4)

SRCDIR   := src
TESTDIR  := tests
OBJDIR   := build
BINDIR   := bin
BIN      := raytracer
```

Notes:

- `CC ?= cc` (not `CC = cc`) lets a caller run `make CC=clang` or `make CC=gcc-13` without editing
  the file. `?=` only assigns when the variable is still undefined.
- `:=` is *immediate* expansion; `=` is *recursive* (expanded each use). Use `:=` for paths that
  are computed once, `?=` for toolchain knobs.
- Keeping `CFLAGS`/`LDLIBS` overridable means `make CFLAGS="-O0 -g -fsanitize=address,undefined"`
  works for a one-off debug build.

### 1.3 Derived file lists

```make
SRCS     := $(wildcard $(SRCDIR)/*.c)
OBJS     := $(SRCS:$(SRCDIR)/%.c=$(OBJDIR)/%.o)   # substitution reference
MAIN_OBJ := $(OBJDIR)/main.o
LIB_OBJS := $(filter-out $(MAIN_OBJ),$(OBJS))      # objects minus main (for tests)

TEST_SRCS := $(wildcard $(TESTDIR)/*.c)
TEST_BINS := $(patsubst $(TESTDIR)/%.c,$(BINDIR)/%,$(TEST_SRCS))

DEPS     := $(OBJS:.o=.d)                          # build/main.d, build/vec.d, …
```

- `$(SRCS:$(SRCDIR)/%.c=$(OBJDIR)/%.o)` is a **substitution reference**; `$(patsubst …)` is the
  functional equivalent. Both are portable.
- `LIB_OBJS` is the set of translation units shared with the test binaries (everything except the
  program's `main.c`). Tests link against these objects rather than recompiling the world.

### 1.4 Pattern rules and automatic variables

```make
$(OBJDIR)/%.o: $(SRCDIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I$(SRCDIR) -MMD -MP -c $< -o $@

$(BINDIR)/$(BIN): $(OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@
```

Automatic variables — the core of portable rules:

| Var  | Meaning                                                        |
|------|----------------------------------------------------------------|
| `$@` | The **target** (file being built), e.g. `build/vec.o`.          |
| `$<` | The **first prerequisite**, e.g. `src/vec.c`.                   |
| `$^` | **All prerequisites**, deduplicated, space-separated (`a.o b.o`).|
| `$*` | The **stem** matched by `%` (here, `vec`).                      |
| `$?` | Prerequisites newer than the target.                            |

- The **pattern rule** `%.o: %.c` is the idiomatic "compile one C file to one object" rule.
  Combined with `$(OBJDIR)/%.o: $(SRCDIR)/%.c`, make can derive `build/vec.o` from `src/vec.c`
  automatically for any source file — no per-file rules needed.
- The **recipe lines must be tab-indented**, not spaces. This is the single most common Makefile
  error; editors that expand tabs silently break the build.
- `@mkdir -p $(dir $@)` creates `build/` (or `bin/`) on demand and stays silent. `$(dir …)` yields
  the directory portion with a trailing slash. This avoids an `order-only` prerequisite dance.
- `-MMD -MP` on the **compile** line emits a `.d` sidecar (e.g. `build/vec.d`) listing every
  header the object depends on. `-MP` adds phony targets for the headers so that deleting a header
  does not make make complain about a missing prerequisite.
- `-include $(DEPS)` (with a leading dash, so it is a no-op when the `.d` files do not exist yet)
  pulls those dependencies back in on the next run. **This is the automatic header-dependency
  mechanism**: touch `src/vec.h` and every object that includes it is rebuilt.

### 1.5 Phony targets

```make
.DEFAULT_GOAL := all
.PHONY: all clean test run
```

- A **phony target** is a target that is *not* a real file. Declaring `.PHONY` prevents a stray
  file named `clean` or `test` in the working directory from making make think the target is
  already up to date, and it lets `make clean` always run.
- `.DEFAULT_GOAL := all` makes a bare `make` build the program even though the first target in the
  file is a pattern rule (which cannot be a default goal). Setting it explicitly is more robust
  than relying on ordering.

### 1.6 `all`, `clean`, `test`

```make
all: $(BINDIR)/$(BIN)

run: all
	./$(BINDIR)/$(BIN)

clean:
	rm -rf $(OBJDIR) $(BINDIR)

-include $(DEPS)      # keep near the end so DEPS is fully defined
```

`test` is shown in full in §3.3, because it depends on the harness convention.

---

## 2. Recommended compiler flags

### 2.1 The baseline

```make
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wpedantic -Wshadow
```

| Flag            | Why                                                                                   |
|-----------------|---------------------------------------------------------------------------------------|
| `-std=c11`      | Pin the language standard so clang and gcc agree. `-std=c11` is supported by both.    |
| `-O2`           | Good, safe optimisation; the right default for a renderer. `-O3` rarely helps enough to justify its compile-time cost. |
| `-Wall -Wextra` | The practical warning baseline. Both compilers support both.                          |
| `-Wpedantic`    | Rejects compiler-specific extensions; keeps the code portable across clang/gcc.       |
| `-Wshadow`      | Catches a local shadowing an outer variable — a real bug source in nested loops.      |

### 2.2 Optional but useful

- `-Wconversion` — warns on implicit narrowing conversions (e.g. `double` → `float`, `size_t` →
  `int`). **Very noisy** in numeric code that mixes `float` and `double`; enable it deliberately
  for a cleanup pass rather than in the default `CFLAGS`. It is supported by both clang and gcc.
- `-Wdouble-promotion` — flags implicit `float` → `double` promotion; useful in a `float`-heavy
  renderer, but again noisy. Optional.
- `-g` — emit debug symbols. Add for debug builds: `make CFLAGS="-std=c11 -O0 -g -Wall -Wextra"`.
  `-g` composes fine with `-O2` if you want optimised-but-debuggable.
- `-fsanitize=address,undefined` — AddressSanitizer + UndefinedBehaviorSanitizer. **Best used in
  the test build**, not the release build (it slows execution and inflates memory). See §3.4.

### 2.3 Suggested presets

```make
# Release (default)
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wpedantic -Wshadow
# Debug
#   make CFLAGS="-std=c11 -O0 -g -Wall -Wextra -Wpedantic -Wshadow"
# Sanitized test build
#   make test CFLAGS="-std=c11 -O1 -g -fsanitize=address,undefined -Wall -Wextra"
```

A cleaner arrangement is to keep a separate variable for the sanitizer so both the compile and
link steps see it:

```make
SANFLAGS ?=
# usage: make test SANFLAGS="-fsanitize=address,undefined"
$(CC) $(CFLAGS) $(SANFLAGS) … -c …
$(CC) $(LDFLAGS) $(SANFLAGS) $^ $(LDLIBS) -o $@
```

`-fsanitize=address,undefined` **must appear on the link line as well as the compile line**;
otherwise the runtime support library is not linked in. Both clang and gcc accept it (gcc ≥ 4.9,
clang ≥ 3.x).

### 2.4 Linking the math library: `-lm`

- The C standard library does **not** include math functions like `sqrt`, `sin`, `pow`, `fmod`.
  On Linux/glibc these live in `libm`, so you must link with `-lm`.
- **Order matters.** The linker processes inputs left to right and resolves symbols from objects
  already seen. A library must therefore appear **after** the objects that reference it:

  ```make
  # CORRECT — -lm after the objects
  $(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@     # expands to: cc … build/main.o build/vec.o -lm -o bin/raytracer

  # WRONG — -lm before the objects; symbols are not yet known, link fails
  $(CC) $(LDLIBS) $^ -o $@
  ```

  This is why `LDLIBS` (not `LDFLAGS`) is the right home for `-lm` and why it is placed at the end
  of the command. `LDFLAGS` is conventionally for linker *options* (`-L`, `-fsanitize=…`), while
  `LDLIBS` holds *libraries*.
- **macOS note:** on macOS, `libm` is folded into `libSystem` and there is no standalone
  `libm.dylib` (confirmed: `ls /usr/lib/libm.dylib` → not present). `-lm` is accepted and simply
  a no-op there. Keeping `-lm` in the Makefile is therefore harmless on macOS and **required** on
  Linux — do not remove it just because the macOS build links without it.

---

## 3. A minimal assert-based unit-test harness

### 3.1 Option A — the C standard `assert`

The simplest possible harness. `<assert.h>`'s `assert(cond)` aborts the process with a diagnostic
(filename, line, expression) when `cond` is false, and returns a nonzero exit status.

```c
/* tests/test_vec.c */
#include <assert.h>
#include <math.h>
#include "vec.h"

int main(void)
{
    assert(fabs(vlen2(3.0, 4.0) - 25.0) < 1e-9);
    assert(vlen2(0.0, 0.0) == 0.0);
    return 0;              /* reached only if every assert held */
}
```

- **Caveat:** `assert` is compiled out when `NDEBUG` is defined. Never define `NDEBUG` for test
  builds; if you do, the tests silently become no-ops that always pass. This is a real trap.
- The process aborts on the *first* failure, so you get one failure per run rather than a report.

### 3.2 Option B — a tiny `CHECK` macro (recommended)

A five-line macro gives a running pass/fail count, continues past failures, and still yields a
nonzero exit code. No external framework required.

```c
/* tests/test_vec.c */
#include <stdio.h>
#include <math.h>
#include "vec.h"

static int g_checks = 0;
static int g_fails  = 0;

#define CHECK(cond, msg)                                                  \
    do {                                                                  \
        ++g_checks;                                                       \
        if (!(cond)) {                                                    \
            ++g_fails;                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); \
        }                                                                 \
    } while (0)

int main(void)
{
    CHECK(fabs(vlen2(3.0, 4.0) - 25.0) < 1e-9, "3-4-5 triangle length squared");
    CHECK(vlen2(0.0, 0.0) == 0.0,              "zero vector length squared");

    printf("%d/%d checks passed\n", g_checks - g_fails, g_checks);
    return g_fails ? 1 : 0;      /* 0 == success, nonzero == failure */
}
```

Key points:

- The `do { … } while (0)` wrapper makes the macro a single statement, so it is safe in `if`/`else`
  bodies without braces and requires no trailing semicolon quirks.
- `__FILE__` / `__LINE__` pinpoint the failing check.
- **`main()` returns 0 on success and nonzero on failure.** This is the contract the Makefile's
  `test` target relies on. `return g_fails ? 1 : 0;` is the whole convention.
- For floating-point comparisons, always test against a tolerance (`fabs(a - b) < 1e-9`), never
  `==`. A `CHECK_NEAR(a, b, eps)` helper macro is a natural extension:

  ```c
  #define CHECK_NEAR(a, b, eps) CHECK(fabs((a) - (b)) < (eps), "values not within tolerance")
  ```

- If several test files need the macro, factor it into `tests/test_util.h` and `#include` it in
  each. The macro definition (not a function) is what keeps `__FILE__`/`__LINE__` meaningful.

### 3.3 Wiring tests into the Makefile

Give every `tests/*.c` its own binary and run them all from the `test` target:

```make
$(BINDIR)/%: $(TESTDIR)/%.c $(LIB_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(SANFLAGS) -I$(SRCDIR) $(LDFLAGS) $< $(LIB_OBJS) $(LDLIBS) -o $@

test: $(TEST_BINS)
	@set -e; for t in $(TEST_BINS); do \
		echo "== $$t =="; \
		./$$t; \
	done; \
	echo "ALL TESTS PASSED"
```

- The pattern rule `$(BINDIR)/%: $(TESTDIR)/%.c $(LIB_OBJS)` turns `tests/test_vec.c` into
  `bin/test_vec`, linking the shared (non-`main`) objects. The program's own `main.c` is excluded
  via `LIB_OBJS` so there is no duplicate `main` symbol.
- `@set -e; for t in …; do ./$$t; done` runs each binary in turn and **aborts the shell loop on
  the first nonzero exit** (`set -e`), which makes `make test` itself fail with a nonzero status.
  The `@` suppresses echoing of the shell line; `$$t` is make-escaped `$t`.
- **Verified behaviour:** with a deliberately failing test, `make test` stops at that test and
  exits nonzero (`make: *** [test] Error 1`, process exit code 2). With all tests passing it prints
  `ALL TESTS PASSED` and exits 0. This is exactly the CI-friendly contract we want.
- A per-test rule is the alternative to the loop (`test: bin/test_vec bin/test_geometry`, each
  listed as a prerequisite), but the loop scales better and gives a clear per-test banner.

### 3.4 Running tests under sanitizers

```bash
make clean
make test CFLAGS="-std=c11 -O1 -g -fsanitize=address,undefined -Wall -Wextra" \
          SANFLAGS="-fsanitize=address,undefined"
```

ASan catches out-of-bounds reads/writes and use-after-free; UBSan catches signed overflow,
misaligned access, and bad shifts. Both are invaluable in vector/geometry code. Keep them **out**
of the release `CFLAGS` — they cost time and memory and are not needed for the shipped binary.

---

## 4. Portability notes (macOS/clang vs Linux/gcc)

1. **`cc` resolves correctly on both.** On macOS `cc` → Apple clang; on Linux `cc` → gcc (via the
   `alternatives`/`cc` symlink). Using `CC ?= cc` means the same Makefile works on both without
   edits, and `make CC=gcc` / `make CC=clang` overrides it explicitly.
2. **Prefer standard C11.** `-std=c11` is the common denominator. Avoid `-std=gnu11` if you want
   `-Wpedantic` to stay meaningful, and avoid C23/C2x features that older gcc/clang lack.
3. **Avoid GNU-only make extensions** so the file is not tied to GNU Make's richer dialect where
   possible: `$(wildcard)`, `$(patsubst)`, `$(filter-out)`, substitution references, and pattern
   rules are all portable and used here. (`make` on both target platforms *is* GNU Make, so these
   are safe; the rule of thumb is simply not to reach for `$(eval)`/`$(call)` machinery unless
   needed.)
4. **No `libm` on macOS.** As noted in §2.4, `-lm` is a no-op on macOS but required on Linux.
   Keep it unconditionally.
5. **`-fsanitize=address,undefined`** works on both Apple clang and modern gcc; older gcc may lack
   some UBSan checks, so treat sanitizer output as advisory.
6. **Threads / `-pthread`:** if the renderer is later parallelised, add `-pthread` to both compile
   and link lines. It is portable across clang and gcc. (Not needed for the current design.)
7. **Integer/type sizes:** use `<stdint.h>` (`uint8_t`, `uint32_t`, `int32_t`) for anything
   serialised (e.g. BMP bytes) rather than `char`/`int`, whose widths are not fixed by the
   standard. This keeps output byte-identical across platforms.
8. **Line endings and tabs:** the Makefile recipe lines must use real tab characters on both
   platforms; `.gitattributes`/editor settings that convert tabs to spaces will break the build.
9. **Run binaries from the project root.** The program writes `output/scene.bmp` using a
   **relative** path, so `./bin/raytracer` must be invoked from the repository root (which is what
   the `run` target does). Running from another directory would resolve the relative path
   elsewhere. Either document "run from the project root", or have `main()` create the `output/`
   directory (`mkdir`/`mkdirat`) before opening the file, or accept an output path as a CLI
   argument. The simplest robust convention: **relative output paths are interpreted from the
   project root**, and `make run` enforces that.
10. **Create `output/` in the Makefile** if the program does not: add `@mkdir -p output` to the
    `run` (or `all`) target so the first run does not fail on a missing directory.

---

## 5. Complete example — Makefile skeleton for the raytracer

This is the full file, adapted to the project layout in `.marmel/execution_plan.md`
(`src/*.c` → `raytracer`, `tests/*.c` → separate test binaries, image at `output/scene.bmp`).
It was built and exercised end-to-end.

```make
# ---------------------------------------------------------------------------
# Raytracer — portable Makefile (macOS/clang and Linux/gcc)
# ---------------------------------------------------------------------------

CC       ?= cc
CFLAGS   ?= -std=c11 -O2 -Wall -Wextra -Wpedantic -Wshadow
LDFLAGS  ?=
LDLIBS   ?= -lm            # math library, placed AFTER objects on the link line
SANFLAGS ?=                # e.g. -fsanitize=address,undefined for test builds

SRCDIR   := src
TESTDIR  := tests
OBJDIR   := build
BINDIR   := bin
OUTDIR   := output
BIN      := raytracer

SRCS      := $(wildcard $(SRCDIR)/*.c)
OBJS      := $(SRCS:$(SRCDIR)/%.c=$(OBJDIR)/%.o)
MAIN_OBJ  := $(OBJDIR)/main.o
LIB_OBJS  := $(filter-out $(MAIN_OBJ),$(OBJS))

TEST_SRCS := $(wildcard $(TESTDIR)/*.c)
TEST_BINS := $(patsubst $(TESTDIR)/%.c,$(BINDIR)/%,$(TEST_SRCS))

DEPS      := $(OBJS:.o=.d)

.DEFAULT_GOAL := all
.PHONY: all clean test run

# --- default: build the program -------------------------------------------
all: $(BINDIR)/$(BIN)

# --- compile: one object per source, with header deps ---------------------
$(OBJDIR)/%.o: $(SRCDIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(SANFLAGS) -I$(SRCDIR) -MMD -MP -c $< -o $@

# --- link the program -----------------------------------------------------
$(BINDIR)/$(BIN): $(OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) $(SANFLAGS) $^ $(LDLIBS) -o $@

# --- build one test binary per tests/*.c ----------------------------------
$(BINDIR)/%: $(TESTDIR)/%.c $(LIB_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(SANFLAGS) -I$(SRCDIR) $(LDFLAGS) $< $(LIB_OBJS) $(LDLIBS) -o $@

# --- run the renderer from the project root -------------------------------
run: all
	@mkdir -p $(OUTDIR)
	./$(BINDIR)/$(BIN)

# --- build and run all tests; fail the build on any failure ---------------
test: $(TEST_BINS)
	@set -e; for t in $(TEST_BINS); do \
		echo "== $$t =="; \
		./$$t; \
	done; \
	echo "ALL TESTS PASSED"

# --- housekeeping ---------------------------------------------------------
clean:
	rm -rf $(OBJDIR) $(BINDIR) $(OUTDIR)

# --- automatic header dependency tracking (no-op on first build) ----------
-include $(DEPS)
```

Usage:

```bash
make                     # build bin/raytracer
make run                 # build, then run from project root -> output/scene.bmp
make test                # build + run all tests/*.c; nonzero exit on failure
make clean               # remove build/, bin/, output/
make CC=clang            # explicit compiler
make test SANFLAGS="-fsanitize=address,undefined" \
          CFLAGS="-std=c11 -O1 -g -Wall -Wextra"   # sanitized test run
```

Minimal companion test file:

```c
/* tests/test_vec.c */
#include <stdio.h>
#include <math.h>
#include "vec.h"

static int g_checks = 0;
static int g_fails  = 0;

#define CHECK(cond, msg)                                                  \
    do {                                                                  \
        ++g_checks;                                                       \
        if (!(cond)) {                                                    \
            ++g_fails;                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); \
        }                                                                 \
    } while (0)

int main(void)
{
    CHECK(fabs(vlen2(3.0, 4.0) - 25.0) < 1e-9, "3-4-5 length squared");
    CHECK(vlen2(0.0, 0.0) == 0.0,              "zero vector");
    printf("%d/%d checks passed\n", g_checks - g_fails, g_checks);
    return g_fails ? 1 : 0;
}
```

---

## 6. Common pitfalls

- **Spaces instead of tabs in recipes.** Make requires a literal tab at the start of every recipe
  line. This is the #1 cause of `missing separator` errors. Configure your editor to keep tabs in
  `Makefile`.
- **`-lm` before the objects.** On Linux the link fails with undefined references to `sqrt`/`sin`.
  Put `$(LDLIBS)` (containing `-lm`) *after* `$^`.
- **Missing `-I$(SRCDIR)`.** Headers included as `#include "vec.h"` are found relative to the
  including file, but if any source lives outside `src/` (or you use `#include <vec.h>`), the
  compile fails. Add `-I$(SRCDIR)` to every compile and test line.
- **Forgetting `-MMD -MP` or `-include $(DEPS)`.** Without them, editing a header does **not**
  rebuild the objects that include it, and you silently link stale code. Verified fix: touching
  `src/vec.h` triggers recompilation of every dependent object.
- **Defining `NDEBUG` in test builds.** It compiles out `assert`, turning failing tests into
  no-ops that "pass". Never set `NDEBUG` for tests.
- **Test binary colliding with the program's `main`.** Linking `tests/*.c` together with
  `src/main.o` yields a duplicate-`main` error. Link tests against `LIB_OBJS` (all objects except
  `main.o`), as the example does.
- **`set -e` swallowed by a pipeline or subshell.** `set -e` only aborts on a failing *command*,
  not inside `if`/`&&` contexts in some shells; keep the loop body a plain `./$$t;` so the first
  failure stops the run.
- **Running the binary from the wrong directory.** Relative output `output/scene.bmp` resolves
  against the current directory, not the Makefile. Use `make run` (which runs from the root) or
  create `output/` in `main()`.
- **Stale objects after a flag change.** Make does not track `CFLAGS`; switching from `-O2` to
  sanitizers without `make clean` can mix incompatible objects. `make clean` before sanitized runs.
- **`$(wildcard)` evaluated when the tree is empty.** On the very first run `build/` does not
  exist, but that is fine — `wildcard` is only used for `src/`/`tests/`, and `-include` tolerates
  missing `.d` files.
- **Editing `LDLIBS` vs `LDFLAGS`.** Libraries belong in `LDLIBS`; linker *options* in `LDFLAGS`.
  Mixing them up usually still links, but breaks the conventional override story.

---

## 7. Sources & verification

All facts above were checked against the local toolchain and standard documentation:

- Local toolchain (verified by `run_command`):
  - `Apple clang version 17.0.0 (clang-1700.6.4.2)`, target `arm64-apple-darwin25.6.0`.
  - `GNU Make 3.81`.
  - `ls /usr/lib/libm.dylib` → absent (macOS folds `libm` into `libSystem`).
- Empirical checks performed in a temporary multi-file project:
  - `-MMD -MP` produced `build/vec.d` containing `build/vec.o: src/vec.c src/vec.h` plus a phony
    `src/vec.h:` target (the `-MP` effect).
  - `-fsanitize=address,undefined` compiles and links cleanly on Apple clang 17.
  - `-Wpedantic -Wshadow -Wconversion` all accepted by Apple clang 17.
  - `make` built `bin/raytracer`; `make test` ran every `bin/test_*` binary; a deliberately failing
    test aborted the loop and made `make` exit nonzero (`Error 1`, process code 2); touching a
    header triggered rebuild of all dependent objects.
- Reference documentation:
  - GNU Make manual — automatic variables, pattern rules, `.PHONY`, `include`:
    https://www.gnu.org/software/make/manual/make.html
  - GCC warning options: https://gcc.gnu.org/onlinedocs/gcc/Warning-Options.html
  - Clang diagnostics reference: https://clang.llvm.org/docs/DiagnosticsReference.html
  - Clang `-MMD`/`-MP` dependency generation:
    https://clang.llvm.org/docs/ClangCommandLineReference.html#dependency-file-generation
  - AddressSanitizer: https://clang.llvm.org/docs/AddressSanitizer.html
  - UndefinedBehaviorSanitizer: https://clang.llvm.org/docs/UndefinedBehaviorSanitizer.html
  - POSIX `assert.h`: https://pubs.opengroup.org/onlinepubs/9699919799/basedefs/assert.h.html
