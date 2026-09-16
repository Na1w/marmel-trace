/*
 * main.c - Command-line entry point for the raytracer.
 *
 * Responsibilities:
 *   1. Parse CLI options (both `--opt value` and `--opt=value`).
 *   2. Obtain the scene description: --scene FILE, or the embedded default
 *      scene TEXT (DEFAULT_SCENE_TEXT, from src/default_scene_text.h), and
 *      build the scene from it via scene_build_from_desc() (the single
 *      scene-construction path). Both sources go through the parser:
 *      scene_desc_load() for a file, scene_desc_load_string() for the
 *      embedded text.
 *   3. Build the framing camera with the ACTUAL output aspect ratio.
 *   4. Allocate the RGB buffer and render.
 *   5. Create the output directory (if needed) and write the image: BMP, or
 *      PPM (P6) when the --out path ends with ".ppm" (case-insensitive).
 *   6. Print a concise summary and clean up (no leaks on any exit path).
 *
 * C11, -Wall -Wextra clean. Uses fprintf(stderr, ...) for errors and
 * printf for the success summary.
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "vec3.h"
#include "camera.h"
#include "scene.h"
#include "scene_desc.h"
#include "default_scene_text.h"   /* DEFAULT_SCENE_TEXT: the embedded default scene */
#include "render.h"
#include "pathtrace.h"
#include "bmp.h"

#define DEFAULT_WIDTH    1280
#define DEFAULT_HEIGHT   720
#define DEFAULT_SAMPLES  16
#define DEFAULT_DEPTH    6
#define DEFAULT_SEED     1337
#define DEFAULT_OUT      "output/scene.bmp"

typedef struct {
    int         width;
    int         height;
    int         samples;
    int         depth;
    unsigned    seed;
    const char *out;
    const char *scene_path;       /* --scene FILE, or NULL for the embedded default */
    const char *write_scene_path; /* --write-scene FILE, or NULL */
    int         want_threads;     /* --threads (default ON; opt out --no-threads) */
    int         adaptive;         /* --adaptive (default ON; opt out --no-adaptive) */
    int         pathtrace;        /* --pathtrace (default ON; opt out --no-pathtrace) */
    int         adaptive_max;     /* --adaptive-max N (0 = default 4*samples) */
    double      adaptive_tau;     /* --adaptive-tau T (0 = default 0.02) */
    int         no_progress;      /* --no-progress (also RAYTRACER_NO_PROGRESS) */
} Options;

static void usage(FILE *f, const char *prog)
{
    fprintf(f,
        "Usage: %s [options]\n"
        "\n"
        "Options:\n"
        "  --width N      image width in pixels   (default %d, must be > 0)\n"
        "  --height N     image height in pixels  (default %d, must be > 0)\n"
        "  --samples N    samples per pixel       (default %d, must be >= 1)\n"
        "  --depth N      max recursion depth     (default %d, must be >= 0)\n"
        "  --seed N       scene seed              (default %d)\n"
        "  --out PATH     output image path; BMP, or PPM if PATH ends in .ppm\n"
        "                 (default \"%s\")\n"
        "  --scene PATH   load a scene description file (default: embedded scene)\n"
        "  --write-scene PATH\n"
        "                 write the default scene description to PATH and exit\n"
        "  --threads / --no-threads (alias --single-threaded)\n"
        "                 multithreading is ON by default (threaded build);\n"
        "                 --no-threads forces single-threaded rendering\n"
        "  --adaptive / --no-adaptive\n"
        "                 adaptive sampling is ON by default (Whitted renderer\n"
        "                 only); spends more samples on noisy pixels, fewer on\n"
        "                 flat ones; --no-adaptive disables it\n"
        "  --pathtrace / --no-pathtrace\n"
        "                 unbiased path tracer (global illumination) is ON by\n"
        "                 default; --no-pathtrace selects the legacy Whitted\n"
        "                 renderer\n"
        "  --adaptive-max N\n"
        "                 max samples per pixel in adaptive mode (default 4*N)\n"
        "  --adaptive-tau T\n"
        "                 relative-error tolerance in adaptive mode (default 0.02)\n"
        "  --no-progress  silence the stderr render progress meter\n"
        "                 (same as setting RAYTRACER_NO_PROGRESS)\n"
        "  --help, -h     print this usage and exit\n"
        "\n"
        "The scene description format is documented in docs/scene_format.md.\n"
        "Both `--opt value` and `--opt=value` forms are accepted.\n",
        prog, DEFAULT_WIDTH, DEFAULT_HEIGHT, DEFAULT_SAMPLES,
        DEFAULT_DEPTH, DEFAULT_SEED, DEFAULT_OUT);
}

/* Parse a base-10 integer strictly; returns 0 on success, -1 otherwise. */
static int parse_long(const char *s, long *out)
{
    char *end = NULL;
    long v;

    if (s == NULL || *s == '\0')
        return -1;

    errno = 0;
    v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0')
        return -1;

    *out = v;
    return 0;
}

/* Extract the value for an option: either after '=' or the next argv entry. */
static int take_value(const char *eq, int argc, char **argv, int *i,
                      const char **out_val)
{
    if (eq != NULL) {
        *out_val = eq + 1;
        return 0;
    }
    if (*i + 1 >= argc) {
        *out_val = NULL;
        return -1;
    }
    *out_val = argv[++(*i)];
    return 0;
}

static int parse_args(int argc, char **argv, Options *opt)
{
    int i;

    opt->width        = DEFAULT_WIDTH;
    opt->height       = DEFAULT_HEIGHT;
    opt->samples      = DEFAULT_SAMPLES;
    opt->depth        = DEFAULT_DEPTH;
    opt->seed         = (unsigned)DEFAULT_SEED;
    opt->out          = DEFAULT_OUT;
    opt->scene_path   = NULL;
    opt->write_scene_path = NULL;
    opt->want_threads = 1;   /* --threads: ON by default (opt out --no-threads) */
    opt->adaptive     = 1;   /* --adaptive: ON by default (opt out --no-adaptive) */
    opt->pathtrace    = 1;   /* --pathtrace: ON by default (opt out --no-pathtrace) */
    opt->adaptive_max = 0;
    opt->adaptive_tau = 0.0;
    opt->no_progress  = 0;

    for (i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        const char *eq;
        const char *name;
        char        namebuf[64];
        size_t      namelen;

        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            usage(stdout, argv[0]);
            exit(0);
        }

        if (strncmp(arg, "--", 2) != 0) {
            fprintf(stderr, "error: unexpected argument '%s'\n\n", arg);
            usage(stderr, argv[0]);
            return -1;
        }

        eq = strchr(arg, '=');
        if (eq != NULL) {
            namelen = (size_t)(eq - arg);
            if (namelen >= sizeof(namebuf)) {
                fprintf(stderr, "error: unknown option '%s'\n\n", arg);
                usage(stderr, argv[0]);
                return -1;
            }
            memcpy(namebuf, arg, namelen);
            namebuf[namelen] = '\0';
            name = namebuf;
        } else {
            name = arg;
        }

        if (strcmp(name, "--threads") == 0) {
            if (eq != NULL) {
                fprintf(stderr, "error: option '--threads' takes no value\n\n");
                usage(stderr, argv[0]);
                return -1;
            }
            opt->want_threads = 1;
            continue;
        }

        if (strcmp(name, "--adaptive") == 0) {
            if (eq != NULL) {
                fprintf(stderr, "error: option '--adaptive' takes no value\n\n");
                usage(stderr, argv[0]);
                return -1;
            }
            opt->adaptive = 1;
            continue;
        }

        if (strcmp(name, "--pathtrace") == 0) {
            if (eq != NULL) {
                fprintf(stderr, "error: option '--pathtrace' takes no value\n\n");
                usage(stderr, argv[0]);
                return -1;
            }
            opt->pathtrace = 1;
            continue;
        }

        if (strcmp(name, "--no-pathtrace") == 0) {
            if (eq != NULL) {
                fprintf(stderr, "error: option '--no-pathtrace' takes no value\n\n");
                usage(stderr, argv[0]);
                return -1;
            }
            opt->pathtrace = 0;
            continue;
        }

        if (strcmp(name, "--no-adaptive") == 0) {
            if (eq != NULL) {
                fprintf(stderr, "error: option '--no-adaptive' takes no value\n\n");
                usage(stderr, argv[0]);
                return -1;
            }
            opt->adaptive = 0;
            continue;
        }

        if (strcmp(name, "--no-threads") == 0 ||
            strcmp(name, "--single-threaded") == 0) {
            if (eq != NULL) {
                fprintf(stderr, "error: option '%s' takes no value\n\n", name);
                usage(stderr, argv[0]);
                return -1;
            }
            opt->want_threads = 0;
            continue;
        }

        if (strcmp(name, "--no-progress") == 0) {
            if (eq != NULL) {
                fprintf(stderr, "error: option '--no-progress' takes no value\n\n");
                usage(stderr, argv[0]);
                return -1;
            }
            opt->no_progress = 1;
            continue;
        }

        {
            const char *val = NULL;
            long        num = 0;

            if (strcmp(name, "--width") == 0 ||
                strcmp(name, "--height") == 0 ||
                strcmp(name, "--samples") == 0 ||
                strcmp(name, "--depth") == 0 ||
                strcmp(name, "--seed") == 0 ||
                strcmp(name, "--out") == 0 ||
                strcmp(name, "--scene") == 0 ||
                strcmp(name, "--adaptive-max") == 0 ||
                strcmp(name, "--adaptive-tau") == 0 ||
                strcmp(name, "--write-scene") == 0) {
                if (take_value(eq, argc, argv, &i, &val) != 0) {
                    fprintf(stderr, "error: option '%s' requires a value\n\n", name);
                    usage(stderr, argv[0]);
                    return -1;
                }
            } else {
                fprintf(stderr, "error: unknown option '%s'\n\n", arg);
                usage(stderr, argv[0]);
                return -1;
            }

            if (strcmp(name, "--out") == 0) {
                if (val[0] == '\0') {
                    fprintf(stderr, "error: --out path must not be empty\n\n");
                    usage(stderr, argv[0]);
                    return -1;
                }
                opt->out = val;
                continue;
            }

            if (strcmp(name, "--scene") == 0) {
                if (val[0] == '\0') {
                    fprintf(stderr, "error: --scene path must not be empty\n\n");
                    usage(stderr, argv[0]);
                    return -1;
                }
                opt->scene_path = val;
                continue;
            }

            if (strcmp(name, "--write-scene") == 0) {
                if (val[0] == '\0') {
                    fprintf(stderr, "error: --write-scene path must not be empty\n\n");
                    usage(stderr, argv[0]);
                    return -1;
                }
                opt->write_scene_path = val;
                continue;
            }

            if (strcmp(name, "--adaptive-tau") == 0) {
                char *end = NULL;
                double t;

                errno = 0;
                t = strtod(val, &end);
                if (errno != 0 || end == val || *end != '\0' || !(t > 0.0)) {
                    fprintf(stderr,
                            "error: --adaptive-tau expects a positive number, got '%s'\n\n",
                            val);
                    usage(stderr, argv[0]);
                    return -1;
                }
                opt->adaptive_tau = t;
                continue;
            }

            if (parse_long(val, &num) != 0) {
                fprintf(stderr, "error: option '%s' expects an integer, got '%s'\n\n",
                        name, val);
                usage(stderr, argv[0]);
                return -1;
            }

            if (strcmp(name, "--width") == 0) {
                if (num <= 0) {
                    fprintf(stderr, "error: --width must be > 0 (got %ld)\n\n", num);
                    usage(stderr, argv[0]);
                    return -1;
                }
                opt->width = (int)num;
            } else if (strcmp(name, "--height") == 0) {
                if (num <= 0) {
                    fprintf(stderr, "error: --height must be > 0 (got %ld)\n\n", num);
                    usage(stderr, argv[0]);
                    return -1;
                }
                opt->height = (int)num;
            } else if (strcmp(name, "--samples") == 0) {
                if (num < 1) {
                    fprintf(stderr, "error: --samples must be >= 1 (got %ld)\n\n", num);
                    usage(stderr, argv[0]);
                    return -1;
                }
                opt->samples = (int)num;
            } else if (strcmp(name, "--depth") == 0) {
                if (num < 0) {
                    fprintf(stderr, "error: --depth must be >= 0 (got %ld)\n\n", num);
                    usage(stderr, argv[0]);
                    return -1;
                }
                opt->depth = (int)num;
            } else if (strcmp(name, "--seed") == 0) {
                if (num < 0) {
                    fprintf(stderr, "error: --seed must be >= 0 (got %ld)\n\n", num);
                    usage(stderr, argv[0]);
                    return -1;
                }
                opt->seed = (unsigned)num;
            } else if (strcmp(name, "--adaptive-max") == 0) {
                if (num < 1) {
                    fprintf(stderr, "error: --adaptive-max must be >= 1 (got %ld)\n\n",
                            num);
                    usage(stderr, argv[0]);
                    return -1;
                }
                opt->adaptive_max = (int)num;
            }
        }
    }

    return 0;
}

/*
 * Ensure the parent directory of `path` exists. Parses up to the last '/'
 * (no '/' means the current directory, which always exists). Ignores EEXIST.
 * Returns 0 on success, -1 on failure (with a message on stderr).
 */
static int ensure_parent_dir(const char *path)
{
    const char *slash;
    size_t      len;
    char       *dir;

    slash = strrchr(path, '/');
    if (slash == NULL)
        return 0; /* write into the current directory */

    len = (size_t)(slash - path);
    if (len == 0)
        return 0; /* path like "/file.bmp": parent is the root, which exists */

    dir = (char *)malloc(len + 1);
    if (dir == NULL) {
        fprintf(stderr, "error: out of memory creating output directory\n");
        return -1;
    }
    memcpy(dir, path, len);
    dir[len] = '\0';

    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "error: cannot create directory '%s': %s\n",
                dir, strerror(errno));
        free(dir);
        return -1;
    }

    free(dir);
    return 0;
}

/*
 * Case-insensitive test for whether `path` ends with `ext` (e.g. ".ppm").
 * Returns 1 if the suffix matches, 0 otherwise (including NULL inputs or a
 * path shorter than the extension).
 */
static int has_suffix(const char *path, const char *ext)
{
    size_t plen, elen, i;

    if (path == NULL || ext == NULL)
        return 0;

    plen = strlen(path);
    elen = strlen(ext);
    if (plen < elen)
        return 0;

    for (i = 0; i < elen; ++i) {
        char a = path[plen - elen + i];
        char b = ext[i];
        if (a >= 'A' && a <= 'Z')
            a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z')
            b = (char)(b - 'A' + 'a');
        if (a != b)
            return 0;
    }
    return 1;
}

int main(int argc, char **argv)
{
    Options        opt;
    Scene          scene;
    SceneDesc      desc;
    Camera         cam;
    Vec3           eye, target, up;
    double         vfov = 0.0;
    unsigned char *rgb = NULL;
    size_t         npixels;
    size_t         nbytes;
    struct stat    st;
    long           file_size = -1;
    char           errbuf[512];
    int            rc;

    if (parse_args(argc, argv, &opt) != 0)
        return 2;

#ifndef USE_PTHREADS
    if (opt.want_threads)
        fprintf(stderr,
                "note: multithreading unavailable in this serial build; "
                "rendering single-threaded (rebuild with 'make threads')\n");
#endif

    /* 0. --write-scene: dump the default description and exit without rendering. */
    if (opt.write_scene_path != NULL) {
        scene_desc_init(&desc);
        scene_default_desc(&desc);
        if (scene_desc_write(&desc, opt.write_scene_path,
                             errbuf, sizeof errbuf) != 0) {
            fprintf(stderr, "error: cannot write scene description to '%s': %s\n",
                    opt.write_scene_path, errbuf);
            scene_desc_free(&desc);
            return 1;
        }
        printf("Wrote default scene description to %s\n", opt.write_scene_path);
        scene_desc_free(&desc);
        return 0;
    }

    /* 1. Load the scene description: --scene FILE, else the embedded default. */
    scene_desc_init(&desc);
    if (opt.scene_path != NULL) {
        if (scene_desc_load(&desc, opt.scene_path, errbuf, sizeof errbuf) != 0) {
            fprintf(stderr, "error: cannot load scene '%s': %s\n",
                    opt.scene_path, errbuf);
            scene_desc_free(&desc);
            return 1;
        }
    } else {
        /* Built-in default: parse the embedded scene TEXT so the data-driven
         * description path is the single source of truth, exactly as for a
         * --scene file. DEFAULT_SCENE_TEXT is byte-identical to
         * scenes/default.scene, so the two renders agree byte-for-byte.
         * Failure is impossible for the shipped text but must not crash. */
        if (scene_desc_load_string(&desc, DEFAULT_SCENE_TEXT, "<embedded>",
                                   errbuf, sizeof errbuf) != 0) {
            fprintf(stderr,
                    "error: cannot parse the embedded default scene: %s\n",
                    errbuf);
            scene_desc_free(&desc);
            return 1;
        }
        if (desc.material_count == 0) {
            fprintf(stderr, "error: cannot build the embedded default scene\n");
            scene_desc_free(&desc);
            return 1;
        }
    }

    /* 2. Build the scene geometry from the description. */
    memset(&scene, 0, sizeof(scene));
    if (scene_build_from_desc(&scene, &desc) != 0) {
        fprintf(stderr, "error: failed to build the scene (seed %u)\n", opt.seed);
        scene_desc_free(&desc);
        return 1;
    }

    /* 3. Camera with the actual output aspect ratio.
     *
     * Prefer the parsed `camera { }` block (desc.camera) when the scene
     * supplied one; otherwise fall back to the hardcoded default view. The
     * camera is built HERE, before scene_desc_free(), because the description
     * owns the framing values. The depth-of-field fields are copied from the
     * parsed description; for the built-in default scene they are the camera
     * defaults (aperture 0 = pinhole, focus_distance 0 = derive from
     * |target - eye|), and its camera block repeats scene_default_view()
     * exactly, so the default render stays byte-identical. */
    if (desc.camera.present) {
        eye = desc.camera.eye;
        target = desc.camera.target;
        up = desc.camera.up;
        vfov = desc.camera.vfov_deg;
    } else {
        scene_default_view(&eye, &target, &up, &vfov);
    }
    cam = camera_create(eye, target, up, vfov, (double)opt.width / (double)opt.height);
    cam.aperture = desc.camera.aperture;
    /*
     * focus_distance == CAMERA_FOCUS_DISTANCE_DERIVED (0) means the key was
     * omitted: keep the default camera_create() already derived, the distance
     * from the eye to the target. A positive file value overrides it.
     */
    if (desc.camera.focus_distance > CAMERA_FOCUS_DISTANCE_DERIVED)
        cam.focus_distance = desc.camera.focus_distance;
    scene_desc_free(&desc);

    /* 4. Allocate the pixel buffer (overflow-guarded). */
    npixels = (size_t)opt.width * (size_t)opt.height;
    if (npixels > SIZE_MAX / 3u) {
        fprintf(stderr, "error: image dimensions too large (%dx%d)\n",
                opt.width, opt.height);
        scene_free(&scene);
        return 1;
    }
    nbytes = npixels * 3u;

    rgb = (unsigned char *)malloc(nbytes);
    if (rgb == NULL) {
        fprintf(stderr, "error: failed to allocate %zu bytes for the image\n", nbytes);
        scene_free(&scene);
        return 1;
    }

    /* 5. Render. The path tracer is the DEFAULT; --no-pathtrace selects the
     * legacy Whitted renderer (render_image_ex). Adaptive sampling is also ON
     * by default and applies only to the Whitted renderer. The --no-progress
     * flag is implemented by setting the documented environment variable, so
     * the renderer has a single silencing mechanism. */
    {
        RenderParams rp;
        rp.samples_per_pixel = opt.samples;
        rp.max_depth         = opt.depth;
        rp.adaptive          = opt.adaptive;
        rp.adaptive_max_spp  = opt.adaptive_max;
        rp.adaptive_tau      = opt.adaptive_tau;

        if (opt.no_progress)
            (void)setenv("RAYTRACER_NO_PROGRESS", "1", 1);

        rc = opt.pathtrace
                 ? pathtrace_render(&scene, &cam, opt.width, opt.height,
                                    opt.samples, opt.depth, rgb)
                 : render_image_ex(&scene, &cam, opt.width, opt.height, &rp, rgb);
    }
    if (rc != 0) {
        fprintf(stderr, "error: rendering failed (code %d)\n", rc);
        free(rgb);
        scene_free(&scene);
        return 1;
    }

    /* 6. Create the output directory, then write the image. Select the
     * format from the --out extension: ".ppm" (case-insensitive) -> PPM (P6),
     * anything else -> BMP. */
    if (ensure_parent_dir(opt.out) != 0) {
        free(rgb);
        scene_free(&scene);
        return 1;
    }

    {
        int wrc = has_suffix(opt.out, ".ppm")
                      ? ppm_write(opt.out, rgb, opt.width, opt.height)
                      : bmp_write(opt.out, rgb, opt.width, opt.height);
        if (wrc != 0) {
            fprintf(stderr, "error: failed to write '%s'\n", opt.out);
            free(rgb);
            scene_free(&scene);
            return 1;
        }
    }

    /* 7. Summary (actual on-disk size). */
    if (stat(opt.out, &st) == 0)
        file_size = (long)st.st_size;

    printf("Scene: %d primitives, %d materials\n",
           scene.geo.count, scene.material_count);
    printf("Render: %dx%d, %d spp, depth %d, seed %u\n",
           opt.width, opt.height, opt.samples, opt.depth, opt.seed);
    if (opt.pathtrace)
        printf("Mode: path tracer (global illumination, depth %d)\n", opt.depth);
    else
        printf("Mode: Whitted (legacy, depth %d)\n", opt.depth);
    if (opt.adaptive) {
        int n0 = opt.samples;
        int nmax = (opt.adaptive_max > 0) ? opt.adaptive_max : 4 * n0;
        double tau = (opt.adaptive_tau > 0.0) ? opt.adaptive_tau : 0.02;
        printf("Adaptive: on (n0=%d, max=%d, tau=%.4g, used %llu samples, "
               "max/px=%d)\n",
               n0, nmax, tau, render_last_total_samples(),
               render_last_max_samples());
    }
#ifdef USE_PTHREADS
    printf("Time: %.2f s (threaded)\n", render_last_seconds());
#else
    printf("Time: %.2f s (single-threaded)\n", render_last_seconds());
#endif
    printf("Progress: %s\n", render_progress_enabled() ? "on (stderr)" : "off");
    if (file_size >= 0)
        printf("Wrote %s (%ld bytes)\n", opt.out, file_size);
    else
        printf("Wrote %s\n", opt.out);

    /* 8. Cleanup. */
    free(rgb);
    scene_free(&scene);
    return 0;
}
