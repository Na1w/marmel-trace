/*
 * tests/test_water_units.c - UNIT test for the ENERGY-CONSERVATION property of
 *                            the legacy lobe weighting in src/pathtrace.c.
 *
 * WHY THIS FILE EXISTS
 * --------------------
 * `pt_sample_legacy` (src/pathtrace.c) builds a stochastic mix of mirror /
 * dielectric / Lambertian lobes. When `reflectivity + transparency > 1` (the
 * water preset is 1.0 + 0.85 = 1.85) the SELECTION probabilities are
 * renormalised by `s = r_mir + r_die`, but the historical code left the lobe
 * WEIGHTS at `r/p = s`, injecting a spurious `x s` energy gain per bounce (see
 * docs/diag_water_absorption.md, docs/diag_water_sky_sun.md). The fix divides
 * both lobe weights by `s` (a `norm` factor), so:
 *
 *     reflectivity + transparency > 1   ->   w_lobe = r_lobe / (p_lobe * s)
 *     reflectivity + transparency <= 1  ->   w_lobe = r_lobe /  p_lobe   (norm = 1)
 *
 * This file locks that contract.
 *
 * APPROACH (a): TRANSLATION-UNIT INCLUSION
 * ----------------------------------------
 * `pt_sample_legacy` is `static`, so it is not reachable through a public
 * header. Rather than weaken production code (adding a non-static wrapper or a
 * test-only hook), this test #includes the translation unit
 * (`../src/pathtrace.c`) directly and calls the real static function. The three
 * public entry points defined by that TU are #defined to unique names first so
 * the test binary (which also links src/pathtrace.o from the Makefile's
 * LIB_OBJS) has no duplicate symbols; the static kernel under test is unchanged.
 * No new symbol is added to src/, and src/render.{h,c} is untouched.
 *
 * DETERMINISM
 * -----------
 * Every uniform is drawn from the kernel's OWN hash PRNG (`pt_rand01`, also a
 * file-static of the included TU) with a fixed `seed_key`/channel, so the test
 * is a pure function of the seed. All comparisons are EXACT (`==`, bit-for-bit)
 * — there are NO floating tolerances to go flaky.
 *
 * C11, -Wall -Wextra clean, no rand(), no clock, no I/O beyond a summary line.
 */

/*
 * Rename the TU's three public entry points so including the .c file does not
 * collide with the identical symbols that the Makefile links in from
 * src/pathtrace.o. Only file-static helpers (pt_sample_legacy / pt_rand01 /
 * the PT_CH_* channel constants) are actually used by this test.
 */
#define pathtrace_radiance tu_pathtrace_radiance
#define pathtrace_to_byte   tu_pathtrace_to_byte
#define pathtrace_render    tu_pathtrace_render
#include "../src/pathtrace.c"

#include <math.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/* Minimal harness (same convention as tests/test_water.c)             */
/* ------------------------------------------------------------------ */

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, msg) do { if (cond) { g_pass++; } else { g_fail++; \
    fprintf(stderr, "FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); } } while (0)

/* Exact (bit-for-bit) double comparison — the task forbids flaky tolerances. */
#define CHECK_EQ(a, b, msg) do { \
    double tu__a = (a), tu__b = (b); \
    if (tu__a == tu__b) { g_pass++; } else { g_fail++; \
        fprintf(stderr, "FAIL: %s (%s:%d): %.17g != %.17g\n", \
                (msg), __FILE__, __LINE__, tu__a, tu__b); } } while (0)

/* ------------------------------------------------------------------ */
/* Test fixtures                                                       */
/* ------------------------------------------------------------------ */

/*
 * Build a fully-initialised material. Every field is set explicitly so the
 * test never reads an uninitialised value, and the legacy (non-PBR) path is
 * selected (`pbr = 0`).
 */
static Material tu_make_material(double reflectivity, double transparency, double ior)
{
    Material m;
    m.albedo          = vec3(0.10, 0.20, 0.30);
    m.specular        = vec3(1.0, 1.0, 1.0);
    m.shininess       = 64.0;
    m.reflectivity    = reflectivity;
    m.transparency    = transparency;
    m.ior             = ior;
    m.is_water        = 1;
    m.absorption      = vec3(0.45, 0.12, 0.06);
    m.deep_color      = vec3(0.02, 0.10, 0.16);
    m.beer_lambert    = 0;
    m.metallic        = 0.0;
    m.roughness       = 0.0;
    m.emissive        = vec3(0.0, 0.0, 0.0);
    m.pbr             = 0;
    m.texture_kind    = TEXTURE_NONE;
    m.texture_scale   = 1.0;
    m.texture_color_a = vec3(1.0, 1.0, 1.0);
    m.texture_color_b = vec3(0.0, 0.0, 0.0);
    return m;
}

/*
 * Independent ORACLE for the documented lobe-selection contract. This is a
 * verbatim restatement of the spec (NOT a call into the code under test), so a
 * mismatch means the implementation drifted from the contract.
 */
typedef struct {
    double p_mir;  /* selection probability of the mirror lobe            */
    double p_die;  /* selection probability of the dielectric lobe        */
    double p_dif;  /* selection probability of the diffuse lobe           */
    double r_mir;  /* renormalised mirror reflectance numerator           */
    double r_die;  /* renormalised dielectric reflectance numerator       */
    double r_dif;  /* renormalised diffuse reflectance numerator          */
    double norm;   /* 1.0, or s = r + t when s > 1                        */
} TuLobes;

static TuLobes tu_oracle(double reflectivity, double transparency)
{
    TuLobes o;
    double r_mir = reflectivity;
    double r_die = transparency;

    o.r_mir = r_mir;
    o.r_die = r_die;
    o.r_dif = 1.0 - r_mir - r_die;
    if (o.r_dif < 0.0) o.r_dif = 0.0;

    o.p_mir = r_mir;
    o.p_die = r_die;
    o.p_dif = 1.0 - o.p_mir - o.p_die;
    o.norm  = 1.0;
    if (o.p_dif < 0.0) {
        double s = o.p_mir + o.p_die;
        if (s > 0.0) {
            o.p_mir /= s;
            o.p_die /= s;
            if (s > 1.0) o.norm = s; /* the energy-normalisation factor */
        }
        o.p_dif = 0.0;
    }
    return o;
}

/* Expected mirror weight per the fixed contract: r_mir / (p_mir * norm). */
static double tu_expected_mirror_weight(const TuLobes *o)
{
    return o->r_mir / (o->p_mir * o->norm);
}

/* Expected dielectric base weight per the fixed contract: r_die / (p_die * norm). */
static double tu_expected_dielectric_weight(const TuLobes *o)
{
    return o->r_die / (o->p_die * o->norm);
}

/*
 * Drive the mirror lobe: u_lobe = 0 selects the mirror branch whenever
 * p_mir > 0. Returns the scalar weight in *w_out (and 1 on success).
 */
static int tu_sample_mirror(const Material *m, double *w_out)
{
    Vec3 wi, w;
    int rf = -1, sp = -1;
    /* N=+Y, d=-Y (so reflect(d,N) = +Y, a valid outgoing direction). */
    int ok = pt_sample_legacy(m, vec3(0.0, 1.0, 0.0), vec3(0.0, -1.0, 0.0),
                              vec3(0.0, 1.0, 0.0), 1,
                              0.3, 0.7, /* u1, u2 */
                              0.0,      /* u_lobe -> mirror window [0, p_mir) */
                              0.5,      /* u_glass (unused by mirror) */
                              &wi, &w, &rf, &sp);
    *w_out = w.x;
    return ok;
}

/*
 * Drive the dielectric lobe in its TOTAL-INTERNAL-REFLECTION sub-branch, where
 * the returned weight equals the base lobe weight exactly:
 *
 *   F >= 1  ->  w = lobe = r_die / (p_die * norm)
 *
 * TIR is forced with front_face = 0 (leaving a denser medium) and cos_i = 0
 * (V perpendicular to N) so eta = ior > 1 and sin^2_t = ior^2 >= 1. `u_lobe`
 * is placed at the centre of the dielectric window [p_mir, p_mir + p_die).
 */
static int tu_sample_dielectric_tir(const Material *m, const TuLobes *o, double *w_out)
{
    Vec3 wi, w;
    int rf = -1, sp = -1;
    double u_lobe = o->p_mir + 0.5 * o->p_die;
    int ok = pt_sample_legacy(m, vec3(0.0, 1.0, 0.0), vec3(0.0, -1.0, 0.0),
                              vec3(1.0, 0.0, 0.0), /* V _|_ N -> cos_i = 0   */
                              0,                    /* front_face = 0 -> eta = ior > 1 */
                              0.3, 0.7, u_lobe, 0.5,
                              &wi, &w, &rf, &sp);
    *w_out = w.x;
    return ok;
}

/* Drive the Lambertian diffuse lobe: u_lobe placed inside [p_mir+p_die, 1). */
static int tu_sample_diffuse(const Material *m, const TuLobes *o, Vec3 *w_out)
{
    Vec3 wi, w;
    int rf = -1, sp = -1;
    double u_lobe = o->p_mir + o->p_die + 0.5 * o->p_dif;
    int ok = pt_sample_legacy(m, vec3(0.0, 1.0, 0.0), vec3(0.0, -1.0, 0.0),
                              vec3(0.0, 1.0, 0.0), 1,
                              0.3, 0.7, u_lobe, 0.5,
                              &wi, &w, &rf, &sp);
    *w_out = w;
    return ok;
}

/*
 * The probability-weighted energy carried by the two DELTA lobes:
 *
 *     E_delta = p_mir * w_mir + p_die * w_die
 *
 * This is the expected throughput multiplier contributed by the mirror +
 * dielectric branches, i.e. the physically meaningful "sum of the two lobe
 * weights". Before the fix E_delta = r_mir + r_die = s (> 1 for water); after
 * the fix E_delta = (r_mir + r_die) / norm <= 1.0.
 */
static double tu_delta_energy(const TuLobes *o, double w_mir, double w_die)
{
    return o->p_mir * w_mir + o->p_die * w_die;
}

/* ------------------------------------------------------------------ */
/* 1. s > 1: the water case (1.0 + 0.85) is energy-normalised          */
/* ------------------------------------------------------------------ */

static void test_water_normalised(void)
{
    const double r = 1.0, t = 0.85; /* s = 1.85 > 1 (the water preset) */
    Material m = tu_make_material(r, t, 1.33);
    TuLobes o = tu_oracle(r, t);

    /* Precondition: the material really is in the renormalised regime. */
    CHECK(r + t > 1.0, "water: reflectivity + transparency > 1 (renormalised)");
    CHECK_EQ(o.norm, r + t, "water: norm == s == r + t");
    /* Precondition on the ORACLE's own probabilities (a sanity check, not the
     * property under test): p_mir + p_die == 1 up to the ~1 ulp rounding of the
     * two divisions. The energy assertion below is the exact one. */
    CHECK(fabs((o.p_mir + o.p_die) - 1.0) < 1e-15,
          "water: selection probabilities sum to 1 (within 1 ulp)");
    CHECK_EQ(o.p_dif, 0.0, "water: no diffuse energy remains");

    double w_mir = -1.0;
    CHECK(tu_sample_mirror(&m, &w_mir) == 1, "water: mirror lobe sampled");
    CHECK_EQ(w_mir, tu_expected_mirror_weight(&o),
             "water: mirror weight == r_mir / (p_mir * s)");

    double w_die = -1.0;
    CHECK(tu_sample_dielectric_tir(&m, &o, &w_die) == 1,
          "water: dielectric (TIR) lobe sampled");
    CHECK_EQ(w_die, tu_expected_dielectric_weight(&o),
             "water: dielectric weight == r_die / (p_die * s)");

    /* Energy conservation: the two delta lobes carry EXACTLY unit energy. */
    double E = tu_delta_energy(&o, w_mir, w_die);
    CHECK_EQ(E, 1.0, "water: p_mir*w_mir + p_die*w_die == 1 (energy conserved)");
    CHECK(E <= 1.0, "water: delta-lobe energy <= 1");

    /* The property has teeth: the UN-normalised (buggy) weighting would give
     * r/p = s = 1.85 for both lobes, i.e. a spurious x1.85 gain. `r/p` is not
     * bit-exact after the division (1.8500000000000003 vs the literal 1.85),
     * so this demonstration check uses a 1 ulp tolerance; the energy assertion
     * above remains exact. */
    double buggy_mirror = o.r_mir / o.p_mir;   /* == s == 1.85 (to ~1 ulp) */
    double buggy_energy = tu_delta_energy(&o, buggy_mirror, buggy_mirror);
    CHECK(fabs(buggy_mirror - 1.85) < 1e-15,
          "water: un-normalised weight would be s == 1.85 (within 1 ulp)");
    CHECK(buggy_energy > 1.0,
          "water: un-normalised weighting would VIOLATE energy conservation");
    CHECK(w_mir < buggy_mirror,
          "water: fixed weight is strictly below the buggy weight");
}

/* ------------------------------------------------------------------ */
/* 2. s == 2 (reflectivity = transparency = 1): fully normalised       */
/* ------------------------------------------------------------------ */

static void test_over_one_equal_split(void)
{
    const double r = 1.0, t = 1.0; /* s = 2 */
    Material m = tu_make_material(r, t, 1.33);
    TuLobes o = tu_oracle(r, t);

    CHECK_EQ(o.norm, 2.0, "s=2: norm == 2");
    CHECK_EQ(o.p_mir, 0.5, "s=2: p_mir == 0.5");
    CHECK_EQ(o.p_die, 0.5, "s=2: p_die == 0.5");

    double w_mir = -1.0, w_die = -1.0;
    CHECK(tu_sample_mirror(&m, &w_mir) == 1, "s=2: mirror lobe sampled");
    CHECK(tu_sample_dielectric_tir(&m, &o, &w_die) == 1, "s=2: dielectric lobe sampled");

    CHECK_EQ(w_mir, 1.0, "s=2: mirror weight == r/(p*s) == 1");
    CHECK_EQ(w_die, 1.0, "s=2: dielectric weight == r/(p*s) == 1");
    CHECK_EQ(tu_delta_energy(&o, w_mir, w_die), 1.0, "s=2: delta energy == 1");
}

/* ------------------------------------------------------------------ */
/* 3. s <= 1: weights are UNCHANGED from the un-normalised formula     */
/* ------------------------------------------------------------------ */

static void test_unchanged_when_le_one(void)
{
    /* (reflectivity, transparency) pairs with r + t <= 1, including the
     * exact boundary r + t == 1 and a strictly-diffuse-capable pair. */
    static const double cases[][2] = {
        { 0.50, 0.30 },  /* s = 0.80, diffuse present */
        { 0.50, 0.50 },  /* s = 1.00, boundary        */
        { 0.40, 0.60 },  /* s = 1.00, boundary        */
        { 0.20, 0.30 },  /* s = 0.50, diffuse present */
        { 0.70, 0.25 },  /* s = 0.95                  */
        { 0.00, 0.00 },  /* s = 0.00, pure diffuse    */
    };
    const size_t n = sizeof(cases) / sizeof(cases[0]);

    for (size_t i = 0; i < n; ++i) {
        double r = cases[i][0], t = cases[i][1];
        Material m = tu_make_material(r, t, 1.33);
        TuLobes o = tu_oracle(r, t);

        CHECK(r + t <= 1.0, "le1: fixture satisfies r + t <= 1");
        CHECK_EQ(o.norm, 1.0, "le1: norm stays exactly 1 (no renormalisation)");
        CHECK_EQ(o.p_mir, r, "le1: p_mir == r (selection unchanged)");
        CHECK_EQ(o.p_die, t, "le1: p_die == t (selection unchanged)");

        /* Mirror weight: unchanged == r_mir / p_mir (norm == 1). */
        if (r > 0.0) {
            double w_mir = -1.0;
            CHECK(tu_sample_mirror(&m, &w_mir) == 1, "le1: mirror lobe sampled");
            CHECK_EQ(w_mir, r / r, "le1: mirror weight unchanged == r/p");
            CHECK_EQ(w_mir, tu_expected_mirror_weight(&o),
                     "le1: mirror weight == r_mir / (p_mir * 1)");
        }

        /* Dielectric base weight: unchanged == r_die / p_die. */
        if (t > 0.0) {
            double w_die = -1.0;
            CHECK(tu_sample_dielectric_tir(&m, &o, &w_die) == 1,
                  "le1: dielectric (TIR) lobe sampled");
            CHECK_EQ(w_die, t / t, "le1: dielectric weight unchanged == r/p");
            CHECK_EQ(w_die, tu_expected_dielectric_weight(&o),
                     "le1: dielectric weight == r_die / (p_die * 1)");
        }

        /* Diffuse weight: unchanged == albedo * (r_dif / p_dif). */
        if (o.p_dif > 0.0) {
            Vec3 w;
            CHECK(tu_sample_diffuse(&m, &o, &w) == 1, "le1: diffuse lobe sampled");
            CHECK_EQ(w.x, m.albedo.x * (o.r_dif / o.p_dif), "le1: diffuse R unchanged");
            CHECK_EQ(w.y, m.albedo.y * (o.r_dif / o.p_dif), "le1: diffuse G unchanged");
            CHECK_EQ(w.z, m.albedo.z * (o.r_dif / o.p_dif), "le1: diffuse B unchanged");
        }

        /* The two delta lobes carry s = r + t <= 1 of energy (unchanged). */
        double w_mir = (r > 0.0) ? 1.0 : 0.0;
        double w_die = (t > 0.0) ? 1.0 : 0.0;
        double E = tu_delta_energy(&o, w_mir, w_die);
        CHECK_EQ(E, r + t, "le1: delta energy == r + t (unchanged)");
        CHECK(E <= 1.0, "le1: delta-lobe energy <= 1");
    }
}

/* ------------------------------------------------------------------ */
/* 4. Determinism: seed the kernel's OWN hash PRNG, assert exact values */
/* ------------------------------------------------------------------ */

static void test_prng_determinism_and_exact_weights(void)
{
    /*
     * For each seed the uniforms come from the kernel's private `pt_rand01`
     * (the SAME function the path tracer uses), so this exercises the real
     * deterministic stream rather than ad-hoc numbers.
     */
    const double r = 1.0, t = 0.85; /* water: s = 1.85 */
    Material m = tu_make_material(r, t, 1.33);
    TuLobes o = tu_oracle(r, t);

    int mirror_hits = 0, diel_hits = 0;

    for (unsigned seed_key = 0; seed_key < 64u; ++seed_key) {
        const unsigned bounce = 0u;

        double u_lobe = pt_rand01(seed_key, bounce, PT_CH_LOBE);
        double u1     = pt_rand01(seed_key, bounce, PT_CH_BSDF_A);
        double u2     = pt_rand01(seed_key, bounce, PT_CH_BSDF_B);
        double u_glass = pt_rand01(seed_key, bounce, PT_CH_GLASS);

        /* Same seed -> identical uniforms (PRNG determinism). */
        CHECK_EQ(u_lobe, pt_rand01(seed_key, bounce, PT_CH_LOBE),
                 "prng: u_lobe reproducible for a fixed seed");

        Vec3 wi, w;
        int rf = -1, sp = -1;
        int ok = pt_sample_legacy(&m, vec3(0.0, 1.0, 0.0), vec3(0.0, -1.0, 0.0),
                                  vec3(1.0, 0.0, 0.0), 0, /* front_face=0 -> TIR setup */
                                  u1, u2, u_lobe, u_glass,
                                  &wi, &w, &rf, &sp);
        CHECK(ok == 1, "prng: every water sample returns a valid lobe");

        /* Expected exact weight from the contract, based on which delta lobe
         * the deterministic `u_lobe` selects. For water p_mir + p_die == 1, so
         * every draw lands in one of the two delta lobes. */
        double expected;
        if (u_lobe < o.p_mir) {
            expected = tu_expected_mirror_weight(&o);
            mirror_hits++;
        } else {
            expected = tu_expected_dielectric_weight(&o);
            diel_hits++;
        }
        CHECK_EQ(w.x, expected, "prng: exact lobe weight matches contract");
        CHECK_EQ(w.y, w.x, "prng: delta weight is achromatic (Y)");
        CHECK_EQ(w.z, w.x, "prng: delta weight is achromatic (Z)");

        /* Re-running with identical arguments is bit-identical. */
        Vec3 wi2, w2;
        int rf2 = -1, sp2 = -1;
        int ok2 = pt_sample_legacy(&m, vec3(0.0, 1.0, 0.0), vec3(0.0, -1.0, 0.0),
                                   vec3(1.0, 0.0, 0.0), 0,
                                   u1, u2, u_lobe, u_glass,
                                   &wi2, &w2, &rf2, &sp2);
        CHECK(ok2 == ok && w2.x == w.x && w2.y == w.y && w2.z == w.z,
              "prng: identical inputs -> bit-identical weight");
    }

    /* The sweep must actually have exercised both delta lobes. */
    CHECK(mirror_hits > 0, "prng: sweep hit the mirror lobe");
    CHECK(diel_hits > 0, "prng: sweep hit the dielectric lobe");
    CHECK(mirror_hits + diel_hits == 64, "prng: every draw was a delta lobe");
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(void)
{
    test_water_normalised();
    test_over_one_equal_split();
    test_unchanged_when_le_one();
    test_prng_determinism_and_exact_weights();

    printf("tests/test_water_units: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
