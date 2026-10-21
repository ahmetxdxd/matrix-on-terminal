/*
 * ============================================================================
 *  THE MATRIX: ROOFTOP BULLET-TIME (CPU GREEN MATRIX ASCII EDITION)
 * ============================================================================
 *
 *  A real-time 3D ray-marched terminal simulation of the iconic Matrix rooftop
 *  bullet-dodge scene, written in pure C with OpenMP multi-threading:
 *
 *    - Neo limbo backbend dodge, analytic IK legs, coat physics, sunglasses
 *    - 3 Agents shooting with extended pistols and muzzle flashes
 *    - 18 bullets with glowing brass cores and aerodynamic vapor shockwave trails
 *    - Wet rooftop with puddles, raindrop ripples, and reflections
 *    - Water tower, stairwell, incandescent lamp, AC unit, chimney, skylight
 *    - Infinite procedural night city with lit windows and antenna beacons
 *    - Green Matrix digital rain ASCII glyph streaming with pure black background
 *    - Rain toggle ([G]), 2x Ultra Super-Sampling SSAA ([+] / [-] / [T])
 *
 *  Build:
 *      gcc -O3 -march=native -ffast-math -fopenmp ascii.c -lm -o ascii
 *  Run:
 *      ./ascii
 * ============================================================================
 */

#define _DEFAULT_SOURCE
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

#define PI 3.14159265358979f

/* ------------------------------------------------------------------------- */
/*  Small 3D vector library                                                  */
/* ------------------------------------------------------------------------- */

typedef struct { float x, y, z; } V3;

static inline V3 v3(float x, float y, float z) { V3 r = {x, y, z}; return r; }
static inline V3 vadd(V3 a, V3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline V3 vsub(V3 a, V3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline V3 vmul(V3 a, float s) { return v3(a.x * s, a.y * s, a.z * s); }
static inline V3 vmulv(V3 a, V3 b) { return v3(a.x * b.x, a.y * b.y, a.z * b.z); }
static inline float vdot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline float vlen(V3 a) { return sqrtf(vdot(a, a)); }
static inline V3 vnorm(V3 a) { float l = vlen(a); return l > 1e-8f ? vmul(a, 1.0f / l) : v3(0, 1, 0); }
static inline V3 vcross(V3 a, V3 b) { return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static inline V3 vmix(V3 a, V3 b, float t) { return v3(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t); }
static inline V3 vabs(V3 a) { return v3(fabsf(a.x), fabsf(a.y), fabsf(a.z)); }
static inline V3 vmax0(V3 a) { return v3(fmaxf(a.x, 0), fmaxf(a.y, 0), fmaxf(a.z, 0)); }
static inline float clampf(float x, float a, float b) { return x < a ? a : (x > b ? b : x); }
static inline float mixf(float a, float b, float t) { return a + (b - a) * t; }
static inline float fractf(float x) { return x - floorf(x); }
static inline float smoothstepf(float a, float b, float x)
{
    float t = clampf((x - a) / (b - a), 0, 1);
    return t * t * (3 - 2 * t);
}

/* ------------------------------------------------------------------------- */
/*  Hashing / noise                                                          */
/* ------------------------------------------------------------------------- */

static inline uint32_t ihash(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352dU;
    x ^= x >> 15; x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

/* Deterministic pseudo random number in [0,1) from two integers. */
static inline float hash2(int a, int b)
{
    return (ihash((uint32_t)a * 374761393U + (uint32_t)b * 668265263U + 0x9e3779b9U) & 0xffffff) / 16777216.0f;
}

/* Smooth 2D value noise. */
static float vnoise(float x, float y)
{
    float fx = floorf(x), fy = floorf(y);
    int ix = (int)fx, iy = (int)fy;
    float tx = x - fx, ty = y - fy;
    tx = tx * tx * (3 - 2 * tx);
    ty = ty * ty * (3 - 2 * ty);
    float a = hash2(ix, iy), b = hash2(ix + 1, iy);
    float c = hash2(ix, iy + 1), d = hash2(ix + 1, iy + 1);
    return mixf(mixf(a, b, tx), mixf(c, d, tx), ty);
}

/* Fractal Brownian motion built from the value noise (3 octaves). */
static float fbm(float x, float y)
{
    float s = 0, a = 0.5f;
    for (int i = 0; i < 3; i++) {
        s += a * vnoise(x, y);
        x = x * 2.03f + 17.1f;
        y = y * 2.03f + 9.7f;
        a *= 0.5f;
    }
    return s / 0.875f;
}

/* ------------------------------------------------------------------------- */
/*  Signed distance primitives                                               */
/* ------------------------------------------------------------------------- */

static inline float sd_sphere(V3 p, float r) { return vlen(p) - r; }

static inline float sd_capsule(V3 p, V3 a, V3 b, float r)
{
    V3 pa = vsub(p, a), ba = vsub(b, a);
    float h = clampf(vdot(pa, ba) / vdot(ba, ba), 0, 1);
    return vlen(vsub(pa, vmul(ba, h))) - r;
}

static inline float sd_box(V3 p, V3 b)
{
    V3 q = vsub(vabs(p), b);
    return vlen(vmax0(q)) + fminf(fmaxf(q.x, fmaxf(q.y, q.z)), 0);
}

static inline float sd_round_box(V3 p, V3 b, float r)
{
    return sd_box(p, v3(b.x - r, b.y - r, b.z - r)) - r;
}

static inline float sd_ellipsoid(V3 p, V3 r)
{
    float k0 = vlen(v3(p.x / r.x, p.y / r.y, p.z / r.z));
    float k1 = vlen(v3(p.x / (r.x * r.x), p.y / (r.y * r.y), p.z / (r.z * r.z)));
    return k1 > 1e-8f ? k0 * (k0 - 1.0f) / k1 : -fminf(r.x, fminf(r.y, r.z));
}

/* Vertical cylinder with half height h and radius r. */
static inline float sd_cyl_y(V3 p, float h, float r)
{
    float dx = sqrtf(p.x * p.x + p.z * p.z) - r;
    float dy = fabsf(p.y) - h;
    return fminf(fmaxf(dx, dy), 0) + sqrtf(fmaxf(dx, 0) * fmaxf(dx, 0) + fmaxf(dy, 0) * fmaxf(dy, 0));
}

/* Round cone: a capsule whose two end caps have different radii (limbs, coat). */
static float sd_round_cone(V3 p, V3 a, V3 b, float r1, float r2)
{
    V3 ba = vsub(b, a);
    float l2 = vdot(ba, ba);
    float rr = r1 - r2;
    float a2 = l2 - rr * rr;
    float il2 = 1.0f / l2;
    V3 pa = vsub(p, a);
    float y = vdot(pa, ba);
    float z = y - l2;
    V3 w = vsub(vmul(pa, l2), vmul(ba, y));
    float x2 = vdot(w, w);
    float y2 = y * y * l2;
    float z2 = z * z * l2;
    float k = (rr > 0 ? 1.0f : -1.0f) * rr * rr * x2;
    if ((z > 0 ? 1.0f : -1.0f) * a2 * z2 > k) return sqrtf(x2 + z2) * il2 - r2;
    if ((y > 0 ? 1.0f : -1.0f) * a2 * y2 < k) return sqrtf(x2 + y2) * il2 - r1;
    return (sqrtf(x2 * a2 * il2) + y * rr) * il2 - r1;
}

static inline float smin(float a, float b, float k)
{
    float h = clampf(0.5f + 0.5f * (b - a) / k, 0, 1);
    return mixf(b, a, h) - k * h * (1 - h);
}

/* ------------------------------------------------------------------------- */
/*  Materials                                                                */
/* ------------------------------------------------------------------------- */

enum {
    M_NONE = 0, M_ROOF, M_PARAPET, M_BUILD, M_SKIN, M_COAT, M_SUIT, M_SHIRT,
    M_GLASS, M_HAIR, M_BOOT, M_METAL, M_GUN, M_TANK, M_LAMP, M_DOOR, M_FLASH,
    M_GREEN, M_TIE, M_RED
};

typedef struct { float d; int m; } Hit;

static inline void opt(Hit *h, float d, int m)
{
    if (d < h->d) { h->d = d; h->m = m; }
}

/* ------------------------------------------------------------------------- */
/*  Scene state (animated every frame, read-only while rendering)            */
/* ------------------------------------------------------------------------- */

#define N_AGENTS  3
#define N_BULLETS 18
#define CITY_CELL 24.0f
#define ROOF_HALF 9.6f

static V3 MOON_DIR;           /* direction towards the moon (main light)      */
static V3 FILL_DIR;           /* soft fill light from the camera side          */
static V3 LAMP_POS;           /* warm stairwell lamp                           */
static V3 SKYLIGHT_POS;       /* green glow of the skylight                    */


/* Neo pose, computed once per frame. */
typedef struct {
    V3 hip;
    float lean, lc, ls;       /* lean angle, cos(lean), sin(lean)              */
    float hc, hs;             /* head counter rotation (cos, sin of 0.8*lean)  */
    V3 knee[2], ankle[2];
    V3 shoulder[2], elbow[2], hand[2];     /* in torso space                    */
    V3 tail_a[3], tail_b[3];               /* coat tails (world space)          */
} NeoPose;

static NeoPose neo;

/* Agent pose. */
typedef struct {
    V3 pos;
    V3 fwd, right;            /* local basis on the xz plane                   */
    V3 muzzle;                /* world position of the gun tip                 */
    float flash;              /* muzzle flash intensity 0..1                   */
} Agent;

static Agent agents[N_AGENTS];

/* Bullets. */
typedef struct {
    int active;
    V3 pos, dir;
    float trail;
} Bullet;

static Bullet bullets[N_BULLETS];

/* Additive glow primitives (bullet cores, trails, muzzle flashes). */
typedef struct { V3 a, b; V3 col; float core, halo, power; } Glow;
static Glow glows[N_BULLETS + N_AGENTS];
static int n_glows = 0;

static float g_time = 0.0f;       /* animated "world" time (slowed by bullet time) */
static float g_real = 0.0f;       /* real time                                       */
static int   g_rain = 1;      /* digital rain streams active by default */
static int   ssaa = 0;        /* 2x super-sampling high-resolution mode */

/* ------------------------------------------------------------------------- */
/*  Scene description: signed distance function                              */
/* ------------------------------------------------------------------------- */

/* Rotate a point around X by (c,s). */
static inline V3 rotx_cs(V3 p, float c, float s) { return v3(p.x, p.y * c - p.z * s, p.y * s + p.z * c); }

/* The procedural city and the rooftop slab/parapet. */
static void sdf_city(V3 p, Hit *h)
{
    /* The roof we stand on: a huge box whose top face is y = 0. */
    float roof = sd_box(vsub(p, v3(0, -60, 0)), v3(ROOF_HALF, 60, ROOF_HALF));
    opt(h, roof, M_ROOF);

    /* Hollow parapet wall around the roof edge. */
    float par = sd_box(vsub(p, v3(0, 0.45f, 0)), v3(ROOF_HALF, 0.45f, ROOF_HALF));
    float inner = sd_box(vsub(p, v3(0, 0.45f, 0)), v3(ROOF_HALF - 0.45f, 1.0f, ROOF_HALF - 0.45f));
    par = fmaxf(par, -inner);
    /* Little stone cap on top of the wall. */
    float cap = sd_box(vsub(p, v3(0, 0.93f, 0)), v3(ROOF_HALF + 0.08f, 0.05f, ROOF_HALF + 0.08f));
    cap = fmaxf(cap, -sd_box(vsub(p, v3(0, 0.93f, 0)), v3(ROOF_HALF - 0.5f, 1.0f, ROOF_HALF - 0.5f)));
    opt(h, fminf(par, cap), M_PARAPET);

    /* Neighbouring buildings through domain repetition. */
    int ix = (int)floorf((p.x + CITY_CELL * 0.5f) / CITY_CELL);
    int iz = (int)floorf((p.z + CITY_CELL * 0.5f) / CITY_CELL);
    if (ix == 0 && iz == 0) return;

    V3 q = v3(p.x - ix * CITY_CELL, p.y, p.z - iz * CITY_CELL);
    float h1 = hash2(ix, iz), h2 = hash2(ix + 91, iz + 13), h3 = hash2(ix - 37, iz + 71);
    float h4 = hash2(ix + 5, iz - 53), h5 = hash2(ix - 19, iz - 7);

    float top = -8.0f + 54.0f * h1 * h1;
    float hx = 4.5f + 4.0f * h2, hz = 4.5f + 4.0f * h3;
    float jx = (h4 - 0.5f) * 3.0f, jz = (h5 - 0.5f) * 3.0f;
    float hy = (top + 120.0f) * 0.5f;
    float d = sd_box(vsub(q, v3(jx, top - hy, jz)), v3(hx, hy, hz));

    /* Setback tier + rooftop details for taller towers. */
    if (h1 > 0.30f) {
        float tier = sd_box(vsub(q, v3(jx, top + 1.6f, jz)), v3(hx * 0.6f, 1.6f, hz * 0.6f));
        d = fminf(d, tier);
    }
    d *= 0.85f;
    opt(h, d, M_BUILD);

    if (h1 > 0.62f) {
        /* Antenna mast with a red aircraft warning light. */
        float mast = sd_capsule(q, v3(jx, top + 3, jz), v3(jx, top + 11, jz), 0.09f);
        opt(h, mast, M_METAL);
        opt(h, sd_sphere(vsub(q, v3(jx, top + 11.2f, jz)), 0.22f), M_RED);
    }
}

/* Rooftop furniture: water tower, stairwell, vents, pipes, crates, skylight. */
static void sdf_props(V3 p, Hit *h)
{
    /* --- Water tower ------------------------------------------------------ */
    V3 T = v3(-6.5f, 0, 5.5f);
    float db = vlen(vsub(p, vadd(T, v3(0, 3.2f, 0)))) - 3.9f;
    if (db > 0.5f) opt(h, db, M_METAL);
    else {
        V3 q = vsub(p, T);
        V3 qa = v3(fabsf(q.x), q.y, fabsf(q.z));
        opt(h, sd_capsule(qa, v3(1.0f, 0.0f, 1.0f), v3(0.85f, 3.0f, 0.85f), 0.08f), M_METAL);
        /* Cross bracing between the legs. */
        opt(h, sd_capsule(qa, v3(1.0f, 0.6f, 1.0f), v3(0.0f, 1.9f, 1.0f), 0.03f), M_METAL);
        opt(h, sd_capsule(qa, v3(1.0f, 0.6f, 1.0f), v3(1.0f, 1.9f, 0.0f), 0.03f), M_METAL);
        opt(h, sd_box(vsub(q, v3(0, 3.05f, 0)), v3(1.15f, 0.06f, 1.15f)), M_METAL);
        /* Barrel with metal hoops. */
        float tank = sd_cyl_y(vsub(q, v3(0, 4.2f, 0)), 1.2f, 1.3f);
        float hoop = fabsf(sqrtf(q.x * q.x + q.z * q.z) - 1.33f);
        float hy = fabsf(fabsf(q.y - 4.2f) - 0.75f);
        float hoops = fmaxf(hoop - 0.04f, hy - 0.05f);
        hoops = fmaxf(hoops, fabsf(q.y - 4.2f) - 1.2f);
        opt(h, tank, M_TANK);
        opt(h, hoops, M_METAL);
        opt(h, sd_round_cone(q, v3(0, 5.4f, 0), v3(0, 6.3f, 0), 1.3f, 0.08f), M_TANK);
        /* Ladder rail. */
        opt(h, sd_capsule(q, v3(-1.0f, 0.0f, -1.45f), v3(-1.0f, 5.2f, -1.45f), 0.03f), M_METAL);
        opt(h, sd_capsule(q, v3(-0.7f, 0.0f, -1.45f), v3(-0.7f, 5.2f, -1.45f), 0.03f), M_METAL);
    }

    /* --- Stairwell with door and lamp ------------------------------------ */
    V3 S = v3(6.8f, 0, 5.8f);
    db = vlen(vsub(p, vadd(S, v3(0, 1.6f, 0)))) - 3.5f;
    if (db > 0.5f) opt(h, db, M_PARAPET);
    else {
        V3 q = vsub(p, S);
        float body = sd_box(vsub(q, v3(0, 1.6f, 0)), v3(2.0f, 1.6f, 1.6f));
        body = fmaxf(body, -sd_box(vsub(q, v3(-2.0f, 0.95f, 0)), v3(0.2f, 0.95f, 0.62f)));
        opt(h, body, M_PARAPET);
        opt(h, sd_box(vsub(q, v3(0, 3.28f, 0)), v3(2.18f, 0.1f, 1.78f)), M_PARAPET);
        opt(h, sd_box(vsub(q, v3(-1.82f, 0.9f, 0)), v3(0.05f, 0.9f, 0.55f)), M_DOOR);
        opt(h, sd_box(vsub(q, v3(-1.76f, 0.9f, 0.4f)), v3(0.03f, 0.04f, 0.1f)), M_METAL);
        opt(h, sd_sphere(vsub(q, vsub(LAMP_POS, S)), 0.1f), M_LAMP);
        opt(h, sd_box(vsub(q, v3(-2.1f, 2.1f, 0)), v3(0.08f, 0.04f, 0.04f)), M_METAL);
    }

    /* --- AC unit with round fan housing ---------------------------------- */
    db = vlen(vsub(p, v3(-3.0f, 0.6f, -2.5f))) - 1.6f;
    if (db > 0.4f) opt(h, db, M_METAL);
    else {
        V3 q = vsub(p, v3(-3.0f, 0, -2.5f));
        opt(h, sd_round_box(vsub(q, v3(0, 0.5f, 0)), v3(0.9f, 0.5f, 0.6f), 0.04f), M_METAL);
        float ring = sd_cyl_y(vsub(q, v3(0.0f, 1.03f, 0)), 0.04f, 0.42f);
        opt(h, ring, M_PARAPET);
        opt(h, sd_cyl_y(vsub(q, v3(0.0f, 1.0f, 0)), 0.03f, 0.35f), M_DOOR);
        opt(h, sd_box(vsub(q, v3(0, 0.55f, 0.6f)), v3(0.6f, 0.3f, 0.02f)), M_DOOR);
    }

    /* --- Chimney pipe with cap ------------------------------------------- */
    db = vlen(vsub(p, v3(3.5f, 1.2f, -1.5f))) - 1.8f;
    if (db > 0.4f) opt(h, db, M_METAL);
    else {
        V3 q = vsub(p, v3(3.5f, 0, -1.5f));
        opt(h, sd_cyl_y(vsub(q, v3(0, 1.1f, 0)), 1.1f, 0.26f), M_METAL);
        opt(h, sd_cyl_y(vsub(q, v3(0, 2.35f, 0)), 0.05f, 0.42f), M_METAL);
        opt(h, sd_cyl_y(vsub(q, v3(0, 2.2f, 0)), 0.06f, 0.30f), M_METAL);
        opt(h, sd_cyl_y(vsub(q, v3(0, 0.1f, 0)), 0.1f, 0.45f), M_PARAPET);
    }

    /* --- Long pipe run along the wall, with a vertical riser ------------- */
    db = vlen(vsub(p, v3(-5.75f, 0.5f, 2.5f))) - 3.4f;
    if (db > 0.4f) opt(h, db, M_METAL);
    else {
        opt(h, sd_capsule(p, v3(-9.0f, 0.28f, 2.5f), v3(-3.0f, 0.28f, 2.5f), 0.12f), M_METAL);
        opt(h, sd_capsule(p, v3(-8.2f, 0.28f, 2.5f), v3(-8.2f, 1.6f, 2.5f), 0.1f), M_METAL);
        for (int i = 0; i < 4; i++) {
            float x = -8.4f + i * 1.6f;
            opt(h, sd_box(vsub(p, v3(x, 0.08f, 2.5f)), v3(0.05f, 0.08f, 0.2f)), M_PARAPET);
        }
    }

    /* --- Crates ---------------------------------------------------------- */
    db = vlen(vsub(p, v3(7.4f, 0.5f, -3.5f))) - 1.8f;
    if (db > 0.4f) opt(h, db, M_TANK);
    else {
        opt(h, sd_round_box(vsub(p, v3(7.6f, 0.35f, -3.5f)), v3(0.7f, 0.35f, 0.5f), 0.03f), M_TANK);
        opt(h, sd_round_box(vsub(p, v3(7.3f, 0.95f, -3.3f)), v3(0.4f, 0.25f, 0.4f), 0.03f), M_TANK);
    }

    /* --- Skylight emitting green light (the Matrix glow) ----------------- */
    db = vlen(vsub(p, v3(-6.0f, 0.2f, -6.5f))) - 2.3f;
    if (db > 0.4f) opt(h, db, M_PARAPET);
    else {
        V3 q = vsub(p, v3(-6.0f, 0, -6.5f));
        float frame = sd_box(vsub(q, v3(0, 0.12f, 0)), v3(1.4f, 0.12f, 1.0f));
        float pane = sd_box(vsub(q, v3(0, 0.24f, 0)), v3(1.25f, 0.03f, 0.85f));
        opt(h, fmaxf(frame, -pane + 0.0f), M_PARAPET);
        opt(h, pane, M_GREEN);
    }
}

/* Humanoid Neo performing the limbo dodge. */
static void sdf_neo(V3 p, Hit *h)
{
    V3 c = v3(0, 1.0f, 0.3f);
    float db = vlen(vsub(p, c)) - 1.75f;
    if (db > 0.4f) { opt(h, db, M_COAT); return; }

    const NeoPose *n = &neo;

    /* Legs: thigh + calf as round cones, plus boots. */
    for (int s = 0; s < 2; s++) {
        V3 hip = v3(s ? 0.1f : -0.1f, n->hip.y, 0);
        opt(h, sd_round_cone(p, hip, n->knee[s], 0.115f, 0.075f), M_SUIT);
        opt(h, sd_round_cone(p, n->knee[s], n->ankle[s], 0.075f, 0.052f), M_SUIT);
        V3 toe = vadd(n->ankle[s], v3(0, -0.03f, -0.2f));
        opt(h, sd_capsule(p, vadd(n->ankle[s], v3(0, -0.03f, 0.02f)), toe, 0.055f), M_BOOT);
        opt(h, sd_capsule(p, vadd(n->ankle[s], v3(0, 0.06f, 0.0f)), n->ankle[s], 0.06f), M_BOOT);
    }

    /* Flowing coat tails (world space, they hang from the hips). */
    for (int i = 0; i < 3; i++)
        opt(h, sd_round_cone(p, n->tail_a[i], n->tail_b[i], 0.115f, 0.035f), M_COAT);

    /* Torso space: rotate around the hip by the lean angle. */
    V3 r = vsub(p, n->hip);
    V3 q = v3(r.x, r.y * n->lc + r.z * n->ls, -r.y * n->ls + r.z * n->lc);

    opt(h, sd_capsule(q, v3(-0.1f, 0.0f, 0), v3(0.1f, 0.0f, 0), 0.14f), M_COAT);
    opt(h, sd_round_cone(q, v3(0, 0.0f, 0), v3(0, 0.5f, 0), 0.16f, 0.2f), M_COAT);
    opt(h, sd_capsule(q, v3(-0.21f, 0.55f, 0), v3(0.21f, 0.55f, 0), 0.085f), M_COAT);
    /* Lapel / shirt collar hint on the chest. */
    opt(h, sd_box(vsub(q, v3(0, 0.38f, -0.185f)), v3(0.035f, 0.14f, 0.012f)), M_SHIRT);
    /* Coat buttons / belt. */
    opt(h, sd_box(vsub(q, v3(0, 0.02f, -0.15f)), v3(0.17f, 0.02f, 0.03f)), M_BOOT);

    /* Arms: coat sleeves, forearms and skin hands. */
    for (int s = 0; s < 2; s++) {
        opt(h, sd_round_cone(q, n->shoulder[s], n->elbow[s], 0.078f, 0.062f), M_COAT);
        opt(h, sd_round_cone(q, n->elbow[s], n->hand[s], 0.062f, 0.045f), M_COAT);
        opt(h, sd_ellipsoid(vsub(q, n->hand[s]), v3(0.05f, 0.05f, 0.065f)), M_SKIN);
    }

    /* Neck. */
    opt(h, sd_capsule(q, v3(0, 0.56f, 0), v3(0, 0.67f, 0), 0.05f), M_SKIN);

    /* Head, counter-rotated so Neo keeps watching the Agents. */
    V3 hr = vsub(q, v3(0, 0.66f, 0));
    V3 hq = v3(hr.x, hr.y * n->hc - hr.z * n->hs, hr.y * n->hs + hr.z * n->hc);
    float skin = sd_ellipsoid(vsub(hq, v3(0, 0.15f, -0.01f)), v3(0.088f, 0.115f, 0.1f));
    /* Jaw / chin. */
    skin = smin(skin, sd_ellipsoid(vsub(hq, v3(0, 0.07f, -0.035f)), v3(0.065f, 0.07f, 0.07f)), 0.04f);
    float hair = sd_ellipsoid(vsub(hq, v3(0, 0.185f, 0.025f)), v3(0.097f, 0.115f, 0.105f));
    /* Short hair only on the upper/back part of the head. */
    hair = fmaxf(hair, -(hq.y - 0.15f) * 0.6f - (-hq.z) * 0.4f + 0.0f * hq.x);
    opt(h, skin, M_SKIN);
    opt(h, hair, M_HAIR);
    /* Nose. */
    opt(h, sd_sphere(vsub(hq, v3(0, 0.13f, -0.098f)), 0.016f), M_SKIN);
    /* Round sunglasses and temples. */
    float glasses = sd_round_box(vsub(hq, v3(0, 0.17f, -0.092f)), v3(0.088f, 0.02f, 0.014f), 0.008f);
    float lens = fminf(sd_ellipsoid(vsub(hq, v3(-0.04f, 0.168f, -0.094f)), v3(0.035f, 0.022f, 0.016f)),
                       sd_ellipsoid(vsub(hq, v3(0.04f, 0.168f, -0.094f)), v3(0.035f, 0.022f, 0.016f)));
    opt(h, fminf(glasses, lens), M_GLASS);
    opt(h, sd_capsule(hq, v3(-0.085f, 0.17f, -0.07f), v3(-0.09f, 0.17f, 0.03f), 0.006f), M_GLASS);
    opt(h, sd_capsule(hq, v3(0.085f, 0.17f, -0.07f), v3(0.09f, 0.17f, 0.03f), 0.006f), M_GLASS);
}

/* Agent in a black suit, aiming a pistol at Neo. */
static void sdf_agent(V3 p, const Agent *a, Hit *h)
{
    V3 c = vadd(a->pos, v3(0, 0.95f, 0));
    float db = vlen(vsub(p, c)) - 1.6f;
    if (db > 0.4f) { opt(h, db, M_SUIT); return; }

    /* Convert to the agent's local frame (z forward, x right). */
    V3 d = vsub(p, a->pos);
    V3 q = v3(vdot(d, a->right), d.y, vdot(d, a->fwd));

    /* Legs and shoes. */
    for (int s = -1; s <= 1; s += 2) {
        float x = 0.1f * s;
        opt(h, sd_round_cone(q, v3(x, 0.95f, 0), v3(x * 1.1f, 0.5f, 0.02f), 0.105f, 0.08f), M_SUIT);
        opt(h, sd_round_cone(q, v3(x * 1.1f, 0.5f, 0.02f), v3(x * 1.2f, 0.09f, 0), 0.08f, 0.055f), M_SUIT);
        opt(h, sd_capsule(q, v3(x * 1.2f, 0.06f, 0), v3(x * 1.2f, 0.05f, 0.18f), 0.055f), M_BOOT);
    }
    /* Jacket torso and shoulders. */
    opt(h, sd_round_cone(q, v3(0, 0.95f, 0), v3(0, 1.45f, 0), 0.17f, 0.2f), M_SUIT);
    opt(h, sd_capsule(q, v3(-0.22f, 1.5f, 0), v3(0.22f, 1.5f, 0), 0.085f), M_SUIT);
    /* White shirt V, black tie. */
    opt(h, sd_round_box(vsub(q, v3(0, 1.34f, 0.16f)), v3(0.05f, 0.17f, 0.03f), 0.01f), M_SHIRT);
    opt(h, sd_round_box(vsub(q, v3(0, 1.30f, 0.185f)), v3(0.014f, 0.15f, 0.01f), 0.005f), M_TIE);
    /* Neck + head. */
    opt(h, sd_capsule(q, v3(0, 1.52f, 0), v3(0, 1.62f, 0), 0.05f), M_SKIN);
    float skin = sd_ellipsoid(vsub(q, v3(0, 1.72f, 0.0f)), v3(0.088f, 0.115f, 0.1f));
    skin = smin(skin, sd_ellipsoid(vsub(q, v3(0, 1.64f, 0.035f)), v3(0.065f, 0.07f, 0.07f)), 0.04f);
    opt(h, skin, M_SKIN);
    float hair = sd_ellipsoid(vsub(q, v3(0, 1.755f, -0.025f)), v3(0.095f, 0.11f, 0.105f));
    hair = fmaxf(hair, -(q.y - 1.72f) * 0.6f + (q.z) * 0.4f);
    opt(h, hair, M_HAIR);
    opt(h, sd_round_box(vsub(q, v3(0, 1.74f, 0.092f)), v3(0.088f, 0.02f, 0.014f), 0.008f), M_GLASS);
    opt(h, sd_ellipsoid(vsub(q, v3(-0.04f, 1.738f, 0.094f)), v3(0.035f, 0.022f, 0.016f)), M_GLASS);
    opt(h, sd_ellipsoid(vsub(q, v3(0.04f, 1.738f, 0.094f)), v3(0.035f, 0.022f, 0.016f)), M_GLASS);
    /* Earpiece wire. */
    opt(h, sd_capsule(q, v3(0.09f, 1.72f, 0.0f), v3(0.07f, 1.52f, -0.02f), 0.005f), M_METAL);

    /* Shooting arm (extended) + pistol. */
    opt(h, sd_round_cone(q, v3(0.22f, 1.5f, 0), v3(0.23f, 1.5f, 0.32f), 0.075f, 0.06f), M_SUIT);
    opt(h, sd_round_cone(q, v3(0.23f, 1.5f, 0.32f), v3(0.13f, 1.52f, 0.58f), 0.06f, 0.045f), M_SUIT);
    opt(h, sd_sphere(vsub(q, v3(0.12f, 1.52f, 0.62f)), 0.05f), M_SKIN);
    opt(h, sd_round_box(vsub(q, v3(0.12f, 1.575f, 0.72f)), v3(0.022f, 0.032f, 0.14f), 0.008f), M_GUN);
    opt(h, sd_round_box(vsub(q, v3(0.12f, 1.51f, 0.64f)), v3(0.02f, 0.06f, 0.025f), 0.008f), M_GUN);
    /* Support arm. */
    opt(h, sd_round_cone(q, v3(-0.22f, 1.5f, 0), v3(-0.28f, 1.3f, 0.2f), 0.075f, 0.06f), M_SUIT);
    opt(h, sd_round_cone(q, v3(-0.28f, 1.3f, 0.2f), v3(0.04f, 1.5f, 0.56f), 0.06f, 0.045f), M_SUIT);
    opt(h, sd_sphere(vsub(q, v3(0.05f, 1.5f, 0.58f)), 0.048f), M_SKIN);

    /* Muzzle flash as geometry too. */
    if (a->flash > 0.01f)
        opt(h, sd_ellipsoid(vsub(q, v3(0.12f, 1.575f, 0.88f)), v3(0.05f * a->flash, 0.05f * a->flash, 0.16f * a->flash)), M_FLASH);
}

/* Entire scene. */
static Hit map(V3 p)
{
    Hit h = {1e9f, M_NONE};
    sdf_city(p, &h);
    sdf_props(p, &h);
    sdf_neo(p, &h);
    for (int i = 0; i < N_AGENTS; i++) sdf_agent(p, &agents[i], &h);
    return h;
}

static inline float map_d(V3 p) { return map(p).d; }

static V3 calc_normal(V3 p, float eps)
{
    const V3 k0 = {1, -1, -1}, k1 = {-1, -1, 1}, k2 = {-1, 1, -1}, k3 = {1, 1, 1};
    V3 n = vmul(k0, map_d(vadd(p, vmul(k0, eps))));
    n = vadd(n, vmul(k1, map_d(vadd(p, vmul(k1, eps)))));
    n = vadd(n, vmul(k2, map_d(vadd(p, vmul(k2, eps)))));
    n = vadd(n, vmul(k3, map_d(vadd(p, vmul(k3, eps)))));
    return vnorm(n);
}

/* ------------------------------------------------------------------------- */
/*  Ray marching and shading                                                 */
/* ------------------------------------------------------------------------- */

static int trace(V3 ro, V3 rd, int max_steps, float tmax, float *t_out, int *mat_out)
{
    float t = 0.01f;
    for (int i = 0; i < max_steps; i++) {
        V3 p = vadd(ro, vmul(rd, t));
        /* Above the tallest building and going up: nothing more to hit. */
        if (p.y > 52.0f && rd.y >= 0) return 0;
        Hit h = map(p);
        if (h.d < 0.0012f + 0.0007f * t) { *t_out = t; *mat_out = h.m; return 1; }
        t += h.d;
        if (t > tmax) return 0;
    }
    return 0;
}

static float soft_shadow(V3 ro, V3 rd, float tmax)
{
    float res = 1.0f, t = 0.03f;
    for (int i = 0; i < 26; i++) {
        float d = map_d(vadd(ro, vmul(rd, t)));
        res = fminf(res, 10.0f * d / t);
        if (res < 0.01f) return 0.0f;
        t += clampf(d, 0.04f, 1.2f);
        if (t > tmax) break;
    }
    res = clampf(res, 0, 1);
    return res * res * (3 - 2 * res);
}

static float calc_ao(V3 p, V3 n)
{
    float occ = 0, sca = 1;
    for (int i = 1; i <= 4; i++) {
        float hgt = 0.02f + 0.12f * i;
        float d = map_d(vadd(p, vmul(n, hgt)));
        occ += (hgt - d) * sca;
        sca *= 0.8f;
    }
    return clampf(1.0f - 2.0f * occ, 0, 1);
}

/* Night sky: gradient, moon, halo, stars and drifting clouds. */
static V3 sky_color(V3 rd)
{
    float y = clampf(rd.y, -0.2f, 1.0f);
    V3 horizon = v3(0.06f, 0.17f, 0.15f), zenith = v3(0.004f, 0.018f, 0.03f);
    V3 col = vmix(horizon, zenith, powf(clampf(y * 1.3f, 0, 1), 0.55f));

    float md = vdot(rd, MOON_DIR);
    /* Moon disc + glow. */
    col = vadd(col, vmul(v3(0.55f, 0.85f, 0.95f), 0.25f * powf(clampf(md, 0, 1), 28.0f)));
    col = vadd(col, vmul(v3(0.2f, 0.4f, 0.45f), 0.18f * powf(clampf(md, 0, 1), 6.0f)));
    if (md > 0.9993f) col = vadd(col, v3(1.6f, 2.0f, 2.0f) );

    /* Stars. */
    if (rd.y > 0.0f) {
        float u = atan2f(rd.x, rd.z) * 90.0f, v = asinf(clampf(rd.y, -1, 1)) * 90.0f;
        float h = hash2((int)floorf(u), (int)floorf(v));
        if (h > 0.985f) {
            float fu = fractf(u) - 0.5f, fv = fractf(v) - 0.5f;
            float s = expf(-(fu * fu + fv * fv) * 18.0f) * (h - 0.985f) * 55.0f;
            col = vadd(col, vmul(v3(0.7f, 0.9f, 1.0f), s * clampf(rd.y * 3, 0, 1)));
        }
    }
    /* Clouds on a plane, lit by the moon. */
    if (rd.y > 0.02f) {
        float k = 1.0f / (rd.y + 0.15f);
        float cx = rd.x * k * 1.4f + g_real * 0.01f, cz = rd.z * k * 1.4f;
        float c = smoothstepf(0.45f, 0.85f, fbm(cx, cz));
        float lit = 0.25f + 0.75f * powf(clampf(md, 0, 1), 2.0f);
        V3 ccol = vmix(v3(0.025f, 0.06f, 0.065f), v3(0.18f, 0.32f, 0.34f), lit);
        col = vmix(col, ccol, c * 0.65f * clampf(rd.y * 4, 0, 1));
    }
    /* City light pollution along the horizon. */
    col = vadd(col, vmul(v3(0.05f, 0.12f, 0.08f), expf(-fabsf(rd.y) * 7.0f)));
    return col;
}

/* Point light helper (no shadows). */
static inline V3 point_light(V3 p, V3 n, V3 lp, V3 col, float k)
{
    V3 d = vsub(lp, p);
    float r2 = vdot(d, d);
    float l = 1.0f / sqrtf(r2);
    float nd = fmaxf(vdot(n, vmul(d, l)), 0);
    return vmul(col, nd * k / (1.0f + r2 * 0.35f));
}

/* Window facade albedo/emission for any building wall. */
static void facade(V3 p, V3 n, V3 *albedo, V3 *emit, float *refl)
{
    int ix = (int)floorf((p.x + CITY_CELL * 0.5f) / CITY_CELL);
    int iz = (int)floorf((p.z + CITY_CELL * 0.5f) / CITY_CELL);
    int side = fabsf(n.x) > fabsf(n.z) ? (n.x > 0 ? 1 : 2) : (n.z > 0 ? 3 : 4);
    float u = fabsf(n.x) > fabsf(n.z) ? p.z : p.x;
    float wx = 1.9f, wy = 2.7f;
    float gu = u / wx, gv = p.y / wy;
    float fu = fractf(gu), fv = fractf(gv);
    int cu = (int)floorf(gu), cv = (int)floorf(gv);

    *albedo = v3(0.035f, 0.04f, 0.045f);
    *emit = v3(0, 0, 0);
    *refl = 0.0f;
    /* Horizontal slab lines and vertical mullions. */
    float conc = vnoise(p.x * 2.1f + p.z * 1.7f, p.y * 2.3f) * 0.5f + 0.5f;
    *albedo = vmul(*albedo, 0.6f + 0.6f * conc);

    int win = fu > 0.18f && fu < 0.82f && fv > 0.22f && fv < 0.82f;
    if (win) {
        float hl = hash2(cu + ix * 53 + side * 7, cv + iz * 31 + side * 11);
        float hc = hash2(cu + 7 + ix * 13, cv + 3 + iz * 5 + side);
        *albedo = v3(0.01f, 0.014f, 0.02f);
        *refl = 0.35f;
        if (hl < 0.38f) {
            V3 c;
            if (hc < 0.55f) c = v3(1.0f, 0.74f, 0.42f);       /* warm tungsten  */
            else if (hc < 0.8f) c = v3(0.6f, 0.9f, 0.95f);   /* cold office    */
            else c = v3(0.4f, 1.0f, 0.55f);                  /* Matrix green   */
            float inner = 0.6f + 0.4f * hash2(cu * 3, cv * 7 + side);
            *emit = vmul(c, 1.3f * inner);
            *refl = 0.05f;
        }
    }
}

static void roof_surface(V3 p, V3 *albedo, float *refl, float *puddle, V3 *n_pert)
{
    float g = vnoise(p.x * 9.0f, p.z * 9.0f) * 0.5f + vnoise(p.x * 31.0f, p.z * 31.0f) * 0.5f;
    *albedo = vmul(v3(0.035f, 0.038f, 0.04f), 0.6f + 0.9f * g);
    float pm = smoothstepf(0.58f, 0.68f, fbm(p.x * 0.33f + 3.1f, p.z * 0.33f - 1.7f));
    /* Seams of the roofing felt every 2 m. */
    float seam = fmaxf(0.0f, 1.0f - fabsf(fractf(p.x * 0.5f) - 0.5f) * 40.0f + 19.0f);
    seam = seam > 0 ? 0.4f : 0.0f;
    *albedo = vmul(*albedo, 1.0f - seam * 0.3f);
    *puddle = pm;
    *albedo = vmul(*albedo, 1.0f - 0.55f * pm);
    *refl = 0.10f + 0.72f * pm;

    /* Raindrop ripples inside puddles. */
    *n_pert = v3(0, 0, 0);
    if (pm > 0.2f) {
        float cs = 0.55f;
        int cx = (int)floorf(p.x / cs), cz = (int)floorf(p.z / cs);
        for (int dz = -1; dz <= 1; dz++)
            for (int dx = -1; dx <= 1; dx++) {
                float h1 = hash2(cx + dx, cz + dz), h2 = hash2(cx + dx + 99, cz + dz - 17);
                float ox = (cx + dx + h1) * cs, oz = (cz + dz + h2) * cs;
                float ph = fractf(g_real * (0.5f + h1 * 0.6f) * 1.4f + h2 * 7.0f);
                float ex = p.x - ox, ez = p.z - oz;
                float dd = sqrtf(ex * ex + ez * ez) + 1e-5f;
                float ring = expf(-powf((dd - ph * 0.25f) * 40.0f, 2)) * (1 - ph);
                n_pert->x += ex / dd * ring * 0.55f;
                n_pert->z += ez / dd * ring * 0.55f;
            }
        n_pert->x *= pm; n_pert->z *= pm;
    }
}

/* Shading with optional shadows/reflection (depth 0) or a cheap pass (depth 1). */
static V3 shade(V3 ro, V3 rd, int depth, float *t_hit, int *mat_hit)
{
    float t; int mat;
    int steps = depth == 0 ? 150 : 56;
    float tmax = depth == 0 ? 280.0f : 120.0f;
    if (!trace(ro, rd, steps, tmax, &t, &mat)) {
        if (t_hit) *t_hit = 1e9f;
        if (mat_hit) *mat_hit = M_NONE;
        return sky_color(rd);
    }
    if (t_hit) *t_hit = t;
    if (mat_hit) *mat_hit = mat;

    V3 p = vadd(ro, vmul(rd, t));
    V3 n = calc_normal(p, 0.0015f + 0.0006f * t);

    V3 albedo = v3(0.1f, 0.1f, 0.1f), emit = v3(0, 0, 0);
    float spec = 0.15f, shin = 24.0f, refl = 0.0f, ao_k = 1.0f;
    float puddle = 0.0f;

    switch (mat) {
    case M_ROOF:
        if (n.y > 0.7f) {
            V3 pert;
            roof_surface(p, &albedo, &refl, &puddle, &pert);
            n = vnorm(vadd(n, pert));
            spec = 0.25f + 0.6f * puddle; shin = 60.0f;
        } else facade(p, n, &albedo, &emit, &refl);
        break;
    case M_BUILD:
        facade(p, n, &albedo, &emit, &refl);
        if (n.y > 0.7f) { albedo = v3(0.03f, 0.033f, 0.035f); emit = v3(0, 0, 0); refl = 0.05f; }
        break;
    case M_PARAPET: {
        float g = vnoise(p.x * 5.0f + p.y * 3.0f, p.z * 5.0f) * 0.5f + 0.5f;
        albedo = vmul(v3(0.11f, 0.12f, 0.12f), 0.55f + 0.7f * g);
        spec = 0.1f; refl = 0.03f;
    } break;
    case M_SKIN:   albedo = v3(0.55f, 0.65f, 0.45f); emit = v3(0.06f, 0.18f, 0.08f); spec = 0.4f; shin = 35; break;
    case M_COAT:   albedo = v3(0.24f, 0.48f, 0.32f); emit = v3(0.05f, 0.16f, 0.07f); spec = 0.9f; shin = 45; refl = 0.12f; break;
    case M_SUIT:   albedo = v3(0.20f, 0.40f, 0.26f); emit = v3(0.04f, 0.12f, 0.05f); spec = 0.5f; shin = 25; break;
    case M_SHIRT:  albedo = v3(0.70f, 0.95f, 0.75f); emit = v3(0.12f, 0.28f, 0.15f); spec = 0.2f; break;
    case M_TIE:    albedo = v3(0.08f, 0.18f, 0.10f); spec = 0.6f; break;
    case M_GLASS:  albedo = v3(0.01f, 0.02f, 0.01f); emit = v3(0.08f, 0.25f, 0.12f); spec = 3.0f; shin = 160; refl = 0.75f; break;
    case M_HAIR:   albedo = v3(0.12f, 0.25f, 0.15f); spec = 0.5f; shin = 40; break;
    case M_BOOT:   albedo = v3(0.15f, 0.30f, 0.20f); spec = 1.0f; shin = 60; refl = 0.15f; break;
    case M_METAL:  albedo = v3(0.14f, 0.15f, 0.155f); spec = 1.0f; shin = 45; refl = 0.12f; break;
    case M_GUN:    albedo = v3(0.025f, 0.027f, 0.03f); spec = 1.3f; shin = 70; refl = 0.2f; break;
    case M_TANK:   albedo = v3(0.20f, 0.11f, 0.065f); spec = 0.1f;
        albedo = vmul(albedo, 0.7f + 0.6f * vnoise(p.x * 25.0f + p.z * 25.0f, p.y * 4.0f)); break;
    case M_DOOR:   albedo = v3(0.04f, 0.05f, 0.05f); spec = 0.3f; break;
    case M_LAMP:   emit = v3(4.0f, 2.8f, 1.4f); albedo = v3(0, 0, 0); break;
    case M_FLASH:  emit = v3(7.0f, 5.0f, 2.5f); albedo = v3(0, 0, 0); break;
    case M_GREEN:  emit = v3(0.25f, 1.8f, 0.6f); albedo = v3(0, 0, 0); break;
    case M_RED:    emit = v3(3.0f, 0.1f, 0.05f); albedo = v3(0, 0, 0); break;
    default: break;
    }

    V3 col = v3(0, 0, 0);
    V3 vdir = vmul(rd, -1.0f);

    if (albedo.x + albedo.y + albedo.z > 0) {
        float ao = depth == 0 ? calc_ao(p, n) : 1.0f;
        ao = mixf(1.0f, ao, ao_k);

        /* Main moon light with soft shadow. */
        float ndl = fmaxf(vdot(n, MOON_DIR), 0);
        float sh = 1.0f;
        if (depth == 0 && ndl > 0.0f && t < 80.0f) sh = soft_shadow(vadd(p, vmul(n, 0.01f)), MOON_DIR, 40.0f);
        V3 moon_col = v3(0.55f, 0.82f, 1.0f);
        V3 lit = vmul(moon_col, 2.6f * ndl * sh);

        /* Soft fill light from the camera side (cool green). */
        float ndf = fmaxf(vdot(n, FILL_DIR), 0);
        lit = vadd(lit, vmul(v3(0.10f, 0.22f, 0.20f), ndf));

        /* Hemispheric ambient. */
        float hemi = 0.5f + 0.5f * n.y;
        V3 amb = vmix(v3(0.012f, 0.025f, 0.025f), v3(0.035f, 0.085f, 0.09f), hemi);
        lit = vadd(lit, vmul(amb, ao));

        /* Rim light for silhouettes - gives razor-sharp glowing Matrix edges! */
        float fr = powf(1.0f - fmaxf(vdot(n, vdir), 0), 2.2f);
        float rim_mult = (mat == M_COAT || mat == M_SUIT || mat == M_SKIN || mat == M_HAIR) ? 2.5f : 0.8f;
        lit = vadd(lit, vmul(v3(0.15f, 0.75f, 0.35f), fr * ao * rim_mult));

        /* Local point lights. */
        lit = vadd(lit, point_light(p, n, LAMP_POS, v3(1.0f, 0.65f, 0.3f), 3.2f));
        lit = vadd(lit, point_light(p, n, SKYLIGHT_POS, v3(0.1f, 1.0f, 0.35f), 3.0f));
        /* Warm light of the bullets and flashes. */
        for (int i = 0; i < N_AGENTS; i++)
            if (agents[i].flash > 0.01f)
                lit = vadd(lit, point_light(p, n, agents[i].muzzle, v3(1.0f, 0.75f, 0.4f), 6.0f * agents[i].flash));

        col = vmulv(albedo, lit);

        /* Specular highlight. */
        V3 hv = vnorm(vadd(MOON_DIR, vdir));
        float sp = powf(fmaxf(vdot(n, hv), 0), shin) * spec * sh * (shin + 8) / 24.0f;
        col = vadd(col, vmul(v3(0.55f, 0.85f, 1.0f), sp * 1.1f));
        V3 hf = vnorm(vadd(vnorm(vsub(LAMP_POS, p)), vdir));
        col = vadd(col, vmul(v3(1.0f, 0.7f, 0.35f), powf(fmaxf(vdot(n, hf), 0), shin) * spec * 0.4f / (1.0f + vdot(vsub(LAMP_POS, p), vsub(LAMP_POS, p)) * 0.2f)));
    }
    col = vadd(col, emit);

    /* Reflections: wet roof, glass, metal, polished surfaces. */
    if (depth == 0 && refl > 0.01f) {
        V3 rdir = vsub(rd, vmul(n, 2.0f * vdot(rd, n)));
        float fres = refl + (1 - refl) * powf(1.0f - fmaxf(vdot(n, vdir), 0), 5.0f) * (mat == M_ROOF ? 0.9f : 0.5f);
        float tt;
        V3 rc = shade(vadd(p, vmul(n, 0.02f)), rdir, 1, &tt, NULL);
        col = vadd(vmul(col, 1.0f - fres * 0.8f), vmul(rc, fres));
    }

    /* Distance fog tinted like the horizon. */
    float fog = 1.0f - expf(-t * (depth == 0 ? 0.0105f : 0.012f));
    V3 fogc = v3(0.035f, 0.1f, 0.095f);
    return vmix(col, fogc, fog);
}

/* ------------------------------------------------------------------------- */
/*  Animation: Neo pose, agents and bullets                                  */
/* ------------------------------------------------------------------------- */

static void update_neo(float t)
{
    NeoPose *n = &neo;
    /* Lean oscillates: leaning back deeper, then recovering slightly. */
    float lean = (50.0f + 10.0f * sinf(t * 1.55f)) * PI / 180.0f;
    n->lean = lean;
    n->lc = cosf(lean); n->ls = sinf(lean);
    n->hc = cosf(0.82f * lean); n->hs = sinf(0.82f * lean);

    n->hip = v3(0.0f, 0.96f - 0.26f * sinf(lean), 0.0f);
    float sway = sinf(t * 1.55f + 0.7f);

    /* Feet planted, knees bent forwards by simple analytic IK. */
    const V3 foot[2] = { v3(-0.2f, 0.085f, 0.12f), v3(0.2f, 0.085f, -0.14f) };
    for (int s = 0; s < 2; s++) {
        V3 hip = v3(s ? 0.1f : -0.1f, n->hip.y, 0);
        n->ankle[s] = foot[s];
        V3 mid = vmul(vadd(hip, foot[s]), 0.5f);
        float dist = vlen(vsub(hip, foot[s])) * 0.5f;
        float off = sqrtf(fmaxf(0.47f * 0.47f - dist * dist, 0.0005f));
        n->knee[s] = vadd(mid, v3(s ? 0.04f : -0.04f, 0.0f, -off));
    }

    /* Arms in torso space: balancing outwards. */
    n->shoulder[0] = v3(-0.22f, 0.55f, 0);
    n->shoulder[1] = v3(0.22f, 0.55f, 0);
    n->elbow[0] = v3(-0.46f, 0.42f + 0.04f * sway, -0.12f);
    n->hand[0] = v3(-0.72f, 0.30f + 0.08f * sway, -0.32f - 0.05f * sway);
    n->elbow[1] = v3(0.44f, 0.36f - 0.04f * sway, -0.06f);
    n->hand[1] = v3(0.68f, 0.12f - 0.06f * sway, -0.22f + 0.05f * sway);

    /* Coat tails hang from the hips, swaying like cloth. */
    float hy = n->hip.y;
    for (int i = 0; i < 3; i++) {
        float x = (i - 1) * 0.15f;
        float ph = t * 2.3f + i * 1.3f;
        n->tail_a[i] = v3(x, hy + 0.05f, 0.08f + 0.05f * fabsf((float)(i - 1)));
        n->tail_b[i] = v3(x * 2.8f + 0.04f * sinf(ph), hy - 0.36f, 0.42f + 0.1f * sinf(ph + 0.8f) + 0.05f * sway + 0.05f * fabsf((float)(i - 1)));
    }
}

static void update_agents(float t)
{
    static const V3 pos[N_AGENTS] = { {-4.6f, 0, -6.4f}, {0.4f, 0, -7.3f}, {5.0f, 0, -6.0f} };
    V3 target = v3(0, 1.0f, 0.3f);
    for (int i = 0; i < N_AGENTS; i++) {
        Agent *a = &agents[i];
        a->pos = pos[i];
        V3 d = vsub(target, a->pos);
        d.y = 0;
        a->fwd = vnorm(d);
        a->right = v3(a->fwd.z, 0, -a->fwd.x);
        a->muzzle = vadd(vadd(a->pos, vmul(a->right, 0.12f)), vadd(vmul(a->fwd, 0.9f), v3(0, 1.575f, 0)));
        a->flash = 0.0f;
    }
    (void)t;
}

/* Fire bullets in a continuous stream: each bullet has its own phase. */
static void update_bullets(float t)
{
    const float period = 2.0f, speed = 8.5f;
    n_glows = 0;
    for (int i = 0; i < N_BULLETS; i++) {
        Bullet *b = &bullets[i];
        int ag = i % N_AGENTS;
        float ph = t * 1.0f + i * (period / N_BULLETS);
        int cyc = (int)floorf(ph / period);
        float life = ph - cyc * period;
        /* Per-cycle random aim around Neo's torso so they graze his body. */
        float r1 = hash2(i * 7 + 1, cyc), r2 = hash2(i * 13 + 5, cyc * 3 + 1), r3 = hash2(i * 3 + 9, cyc + 5);
        V3 target = v3((r1 - 0.5f) * 0.9f, 0.7f + r2 * 1.2f, 0.25f + (r3 - 0.5f) * 0.5f);
        /* Keep a safe distance from Neo's torso so he really "dodges". */
        V3 org = agents[ag].muzzle;
        b->dir = vnorm(vsub(target, org));
        b->pos = vadd(org, vmul(b->dir, speed * life));
        b->trail = fminf(speed * life, 0.9f);
        b->active = (life < 1.9f) && (b->pos.y > 0.04f) && fabsf(b->pos.x) < 9 && fabsf(b->pos.z) < 9;

        if (life < 0.07f) {
            float f = 1.0f - life / 0.07f;
            if (f > agents[ag].flash) agents[ag].flash = f;
        }
        if (b->active && life > 0.03f) {
            Glow *g = &glows[n_glows++];
            g->a = vsub(b->pos, vmul(b->dir, b->trail));
            g->b = b->pos;
            g->col = v3(1.0f, 0.78f, 0.42f);
            g->core = 0.032f; g->halo = 0.22f; g->power = 1.0f;
        }
    }
}

/* ------------------------------------------------------------------------- */
/*  Camera                                                                   */
/* ------------------------------------------------------------------------- */

typedef struct { V3 pos; float yaw, pitch, fov; V3 vel; float yaw_v, pitch_v; int cine; } Camera;
static Camera cam = { {3.0f, 1.2f, 3.6f}, 0, 0, 1.0f, {0, 0, 0}, 0, 0, 1 };

static void camera_look_at(V3 from, V3 to)
{
    V3 d = vsub(to, from);
    cam.yaw = atan2f(d.x, d.z);
    cam.pitch = asinf(clampf(d.y / vlen(d), -1, 1));
}

/* Push the camera out of any solid geometry so it never clips through props. */
static void camera_push_out(void)
{
    for (int i = 0; i < 4; i++) {
        float d = map_d(cam.pos);
        if (d > 0.45f) break;
        V3 n = calc_normal(cam.pos, 0.01f);
        cam.pos = vadd(cam.pos, vmul(n, 0.45f - d + 0.02f));
    }
}

/* Cinematic orbit: slow circling with changing height and radius. */
static void camera_cinematic(float rt)
{
    float ang = rt * 0.17f + 0.6f;
    float rad = 2.8f + 0.6f * sinf(rt * 0.11f);
    float height = 1.15f + 0.65f * sinf(rt * 0.23f) + 0.2f;
    cam.pos = v3(sinf(ang) * rad, height, 0.3f + cosf(ang) * rad);
    /* Stay inside the roof walls. */
    cam.pos.x = clampf(cam.pos.x, -8.5f, 8.5f);
    cam.pos.z = clampf(cam.pos.z, -8.5f, 8.5f);
    camera_push_out();
    camera_look_at(cam.pos, v3(0, 1.05f + 0.1f * sinf(rt * 0.3f), 0.2f));
    cam.fov = 1.0f - 0.2f * sinf(rt * 0.19f);
}

/* ------------------------------------------------------------------------- */
/*  Rendering                                                                */
/* ------------------------------------------------------------------------- */

typedef struct { float r, g, b; float th; int mat; float glow; } Pix;

/* Closest approach between a camera ray and a segment (used for glows). */
static void ray_seg(V3 o, V3 d, V3 a, V3 b, float *dist2, float *s_out, float *u_out)
{
    V3 e = vsub(b, a), w = vsub(o, a);
    float bb = vdot(d, e), c = vdot(e, e), dd = vdot(d, w), ee = vdot(e, w);
    float den = c - bb * bb, u;
    if (c < 1e-8f) u = 0;
    else if (den < 1e-7f) u = 0.5f;
    else u = clampf((ee - bb * dd) / den, 0, 1);
    float s = u * bb - dd;
    if (s < 0) s = 0;
    V3 pr = vadd(o, vmul(d, s));
    V3 pq = vadd(a, vmul(e, u));
    V3 df = vsub(pr, pq);
    *dist2 = vdot(df, df);
    *s_out = s;
    *u_out = u;
}

static inline V3 tonemap(V3 c)
{
    /* Matrix green-ish grade, then ACES (Narkowicz) approximation. */
    c = vmulv(c, v3(0.86f, 1.06f, 0.94f));
    float *a = &c.x;
    for (int i = 0; i < 3; i++) {
        float x = a[i] * 0.9f;
        x = (x * (2.51f * x + 0.03f)) / (x * (2.43f * x + 0.59f) + 0.14f);
        a[i] = powf(clampf(x, 0, 1), 1.0f / 2.2f);
    }
    return c;
}

/* Rain streaks composited in screen space with 3 parallax layers. */
static V3 add_rain(V3 col, float u, float v, float rt)
{
    if (!g_rain) return col;
    float acc = 0;
    for (int L = 0; L < 3; L++) {
        float n = 55.0f + 38.0f * L;
        float xs = u * n + v * n * 0.10f;
        float cx = floorf(xs), fx = xs - cx;
        float hsh = hash2((int)cx, L * 17 + 3);
        float yy = fractf(v * (1.3f + 0.7f * L) + rt * (0.9f + hsh * 0.6f + 0.4f * L) + hsh * 9.0f);
        float len = 0.07f + 0.04f * L;
        float streak = yy < len ? 1.0f - yy / len : 0.0f;
        float width = fmaxf(0.0f, 1.0f - fabsf(fx - 0.5f) * 5.5f);
        float on = hsh > 0.45f ? 1.0f : 0.0f;
        acc += streak * width * on * (0.34f - 0.08f * L);
    }
    return vadd(col, vmul(v3(0.55f, 0.78f, 0.85f), acc * 0.55f));
}

static Pix *img = NULL;
static int img_w = 0, img_h = 0;

/* Render the scene into the float image (img_w x img_h). */
static void render_frame(int w, int h, int frame)
{
    if (w != img_w || h != img_h) {
        free(img);
        img = (Pix *)malloc(sizeof(Pix) * (size_t)w * h);
        img_w = w; img_h = h;
    }

    float cy = cosf(cam.yaw), sy = sinf(cam.yaw), cp = cosf(cam.pitch), sp = sinf(cam.pitch);
    V3 fwd = v3(sy * cp, sp, cy * cp);
    V3 right = vnorm(vcross(fwd, v3(0, 1, 0)));
    V3 up = vcross(right, fwd);
    float tanf_h = tanf(cam.fov * 0.5f);
    float aspect = (float)w / (float)h;
    V3 ro = cam.pos;

#pragma omp parallel for schedule(dynamic, 2)
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float u = (2.0f * (x + 0.5f) / w - 1.0f) * aspect * tanf_h;
            float v = (1.0f - 2.0f * (y + 0.5f) / h) * tanf_h;
            V3 rd = vnorm(vadd(fwd, vadd(vmul(right, u), vmul(up, v))));

            float th; int mat_id = M_NONE;
            V3 col = shade(ro, rd, 0, &th, &mat_id);

            /* Additive bullet cores / trails / muzzle glow, only if in front of the hit. */
            float bullet_glow = 0.0f;
            for (int i = 0; i < n_glows; i++) {
                float d2, s, uu;
                ray_seg(ro, rd, glows[i].a, glows[i].b, &d2, &s, &uu);
                if (s > th) continue;
                float k = 0.15f + 0.85f * uu * uu;
                float c2 = glows[i].core * glows[i].core, h2 = glows[i].halo * glows[i].halo;
                float g = 2.4f * c2 / (d2 + c2) * k + 0.15f * h2 / (d2 + h2) * k;
                /* Bullet core threshold: only close rays count as bullet glyphs */
                if (d2 < 0.045f * 0.045f) {
                    bullet_glow += (1.0f - sqrtf(d2) / 0.045f) * k * glows[i].power;
                }
                col = vadd(col, vmul(glows[i].col, g * glows[i].power));
            }
            /* Muzzle flash halos. */
            for (int i = 0; i < N_AGENTS; i++) {
                if (agents[i].flash < 0.01f) continue;
                V3 m = agents[i].muzzle;
                V3 oc = vsub(m, ro);
                float s = vdot(oc, rd);
                if (s < 0 || s > th) continue;
                V3 cl = vsub(oc, vmul(rd, s));
                float d2 = vdot(cl, cl), r2 = 0.14f * 0.14f * agents[i].flash;
                col = vadd(col, vmul(v3(1.0f, 0.75f, 0.4f), 1.6f * r2 / (d2 + r2) * agents[i].flash));
            }

            /* Rain (fainter on very close/dark surfaces is irrelevant here). */
            col = add_rain(col, u / (aspect * tanf_h), v / tanf_h, g_real * 0.5f);

            /* Vignette + grain, then tone-map. */
            float vx = (2.0f * (x + 0.5f) / w - 1.0f), vy = (2.0f * (y + 0.5f) / h - 1.0f);
            float vig = 1.0f - 0.35f * (vx * vx * 0.6f + vy * vy);
            col = vmul(col, vig);
            float grain = (hash2(x + frame * 31, y * 7 + frame) - 0.5f) * 0.02f;
            col = tonemap(col);
            Pix *p = &img[y * w + x];
            p->r = clampf(col.x + grain, 0, 1);
            p->g = clampf(col.y + grain, 0, 1);
            p->b = clampf(col.z + grain, 0, 1);
            p->th = th;
            p->mat = mat_id;
            p->glow = bullet_glow;
        }
    }
}

/* ------------------------------------------------------------------------- */
/*  Terminal output                                                          */
/* ------------------------------------------------------------------------- */

static char *obuf = NULL;
static size_t olen = 0, ocap = 0;

static void oensure(size_t n)
{
    if (olen + n > ocap) {
        ocap = (olen + n) * 2;
        obuf = (char *)realloc(obuf, ocap);
    }
}

static void ostr(const char *s)
{
    size_t n = strlen(s);
    oensure(n);
    memcpy(obuf + olen, s, n);
    olen += n;
}

static void ouint(int v)
{
    char tmp[12]; int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v > 0) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    oensure(n);
    while (n) obuf[olen++] = tmp[--n];
}

static void ocolor(int fg, int r, int g, int b)
{
    oensure(48);   /* worst case escape sequence length, so raw writes below are safe */
    ostr(fg ? "\033[38;2;" : "\033[48;2;");
    ouint(r); obuf[olen++] = ';'; ouint(g); obuf[olen++] = ';'; ouint(b); obuf[olen++] = 'm';
}

enum { STYLE_CODE = 0, STYLE_GREEN, STYLE_ASCII, N_STYLES };
static const char *style_names[N_STYLES] = { "matrix code rain", "green phosphor ASCII", "coloured ASCII" };
static const char RAMP[] = " .'`^\",:;Il!i><~+_-?][}{1)(|\\/tfjrxnuvczXYUJCLQ0OZmwqpdbkhao*#MW&8%B@$";

static int q8(float v) { int i = (int)(v * 255.0f + 0.5f); return i < 0 ? 0 : (i > 255 ? 255 : i); }

/* Convert the rendered image to ANSI text. 'rows' terminal rows x 'cols' columns. */
static void emit_frame(int cols, int rows, int ds, int style)
{
    olen = 0;
    ostr("\033[H");
    int lr = -1, lg = -1, lb = -1, br = -1, bg = -1, bb = -1;
    for (int cy = 0; cy < rows; cy++) {
        for (int cx = 0; cx < cols; cx++) {
            int scale = (ssaa && ds == 1) ? 2 : 1;
            int ix = (cx / ds) * scale; if (ix >= img_w) ix = img_w - 1;
            int iy0 = ((cy * 2) / ds) * scale, iy1 = ((cy * 2 + 1) / ds) * scale;
            if (iy0 >= img_h) iy0 = img_h - 1;
            if (iy1 >= img_h) iy1 = img_h - 1;
            Pix top = img[iy0 * img_w + ix], bot = img[iy1 * img_w + ix];
            if (scale == 2) {
                int ix2 = ix + 1 < img_w ? ix + 1 : ix;
                Pix top2 = img[iy0 * img_w + ix2], bot2 = img[iy1 * img_w + ix2];
                top.r = (top.r + top2.r) * 0.5f; top.g = (top.g + top2.g) * 0.5f; top.b = (top.b + top2.b) * 0.5f;
                bot.r = (bot.r + bot2.r) * 0.5f; bot.g = (bot.g + bot2.g) * 0.5f; bot.b = (bot.b + bot2.b) * 0.5f;
                top.glow = fmaxf(top.glow, top2.glow); bot.glow = fmaxf(bot.glow, bot2.glow);
                if (top.mat == M_NONE && top2.mat != M_NONE) top.mat = top2.mat;
                if (bot.mat == M_NONE && bot2.mat != M_NONE) bot.mat = bot2.mat;
            }

            if (style == STYLE_CODE) {
                float r = (top.r + bot.r) * 0.5f, g = (top.g + bot.g) * 0.5f, b = (top.b + bot.b) * 0.5f;
                float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
                float glow = fmaxf(top.glow, bot.glow);
                int mat = (top.mat != M_NONE && top.mat != M_BUILD) ? top.mat : bot.mat;
                if (mat == M_NONE && top.mat != M_NONE) mat = top.mat;
                float min_th = fminf(top.th, bot.th);

                /* Rain stream in this column */
                float hs = hash2(cx, 77), hl = hash2(cx, 91), hp = hash2(cx, 5);
                float speed = 7.0f + 14.0f * hs;
                float len = 8.0f + 16.0f * hl;
                float cyc = (float)rows + len + 10.0f;
                float head = fmodf(g_real * speed + hp * cyc, cyc);
                float dist = head - (float)cy;
                int is_head = g_rain && (dist >= 0.0f && dist < 1.0f);
                int is_trail = g_rain && (dist >= 0.0f && dist < len);
                float trail = is_trail ? (1.0f - dist / len) : 0.0f;

                int is_char = (mat == M_COAT || mat == M_SUIT || mat == M_SKIN ||
                               mat == M_HAIR || mat == M_BOOT || mat == M_GLASS || mat == M_SHIRT || mat == M_TIE);
                int is_bullet = (glow > 0.35f) || (mat == M_FLASH);
                int is_roof = (mat == M_ROOF || mat == M_PARAPET || mat == M_TANK ||
                               mat == M_METAL || mat == M_GUN || mat == M_DOOR || mat == M_GREEN || mat == M_RED);
                int is_city = (mat == M_BUILD);
                int is_sky = (mat == M_NONE || min_th > 1e7f);

                /* Black background always for clean, high-contrast Matrix vision */
                if (br != 0 || bg != 0 || bb != 0) { ostr("\033[48;2;0;0;0m"); br = bg = bb = 0; }

                char ch = ' ';
                int fr = 0, fg = 0, fb = 0;

                int tick = (int)(g_real * (4.0f + 8.0f * hs) + hp * 64.0f);
                uint32_t k = ihash((uint32_t)cx * 7919u + (uint32_t)cy * 104729u + (uint32_t)tick * 2654435761u);
                static const char GLYPHS[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ@#$%&*+-=<>/\[]{}|~:;?";
                char code_ch = GLYPHS[k % (sizeof(GLYPHS) - 1)];

                if (is_bullet) {
                    /* BULLET: White-hot blazing core with golden halo */
                    ch = (glow > 0.4f) ? '@' : (glow > 0.2f ? '#' : '*');
                    float intense = clampf(glow * 1.5f + lum, 0, 1);
                    fr = (int)(220 + 35 * intense);
                    fg = (int)(230 + 25 * intense);
                    fb = (int)(150 + 95 * intense);
                } else if (is_char) {
                    /* NEO & AGENTS: Crystal clear vibrant green with bright rim highlights! */
                    ch = code_ch;
                    float c_int = clampf(lum * 1.3f + 0.25f, 0, 1);
                    float edge_w = clampf((lum - 0.55f) * 3.5f, 0, 1);
                    if (is_head) edge_w = 1.0f;
                    fr = (int)(25 + 210.0f * edge_w);
                    fg = (int)(140 + 115.0f * c_int);
                    fb = (int)(35 + 190.0f * edge_w);
                } else if (is_roof) {
                    /* ROOFTOP: Subtle grid / puddle reflections / props */
                    float r_int = lum * 1.1f + trail * 0.4f;
                    if (r_int > 0.16f || is_trail) {
                        ch = code_ch;
                        if (is_head) {
                            fr = 240; fg = 255; fb = 230;
                        } else {
                            float w = clampf((r_int - 0.6f) * 3.0f, 0, 1);
                            fr = (int)(15 + 180.0f * w);
                            fg = (int)(60 + 180.0f * clampf(r_int, 0, 1));
                            fb = (int)(20 + 150.0f * w);
                        }
                    }
                } else if (is_city) {
                    /* SKYSCRAPERS: Windows glow bright green/warm, walls are faint */
                    if (lum > 0.45f) {
                        ch = code_ch;
                        fr = 80; fg = 250; fb = 120;
                    } else if (is_trail) {
                        ch = code_ch;
                        if (is_head) { fr = 240; fg = 255; fb = 230; }
                        else { fr = 15; fg = (int)(50 + 180 * trail); fb = 25; }
                    } else if (lum > 0.12f) {
                        ch = '.';
                        fr = 5; fg = 45; fb = 12;
                    }
                } else if (is_sky) {
                    /* SKY: Clean deep black, only falling rain columns stream through! */
                    if (is_trail) {
                        ch = code_ch;
                        if (is_head) {
                            fr = 255; fg = 255; fb = 255;   /* White-hot rain head */
                        } else {
                            fr = (int)(15 * trail);
                            fg = (int)(40 + 200 * powf(trail, 1.2f));
                            fb = (int)(25 * trail);
                        }
                    }
                }

                if (fr > 255) fr = 255;
                if (fg > 255) fg = 255;
                if (fb > 255) fb = 255;
                fr &= 0xF8; fg &= 0xF8; fb &= 0xF8;

                if (ch != ' ' && (fr != lr || fg != lg || fb != lb)) { ocolor(1, fr, fg, fb); lr = fr; lg = fg; lb = fb; }
                oensure(1); obuf[olen++] = ch;
            } else {
                float r = (top.r + bot.r) * 0.5f, g = (top.g + bot.g) * 0.5f, b = (top.b + bot.b) * 0.5f;
                float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
                lum = powf(clampf(lum * 1.35f, 0, 1), 0.8f);
                int ci = (int)(lum * (sizeof(RAMP) - 2) + 0.5f);
                char ch = RAMP[ci];
                int fr, fg, fb;
                if (style == STYLE_ASCII) {
                    float m = fmaxf(r, fmaxf(g, b)) + 1e-4f;
                    float boost = clampf(0.35f / m, 1.0f, 4.0f);   /* keep dark chars readable */
                    fr = q8(r * boost) & 0xF8; fg = q8(g * boost) & 0xF8; fb = q8(b * boost) & 0xF8;
                } else {
                    fr = (int)(30 * lum) & 0xF8; fg = (60 + (int)(195 * lum)) & 0xF8; fb = (int)(60 * lum) & 0xF8;
                }
                if (br != 0 || bg != 0 || bb != 0) { ostr("\033[48;2;0;0;0m"); br = bg = bb = 0; }
                if (fr != lr || fg != lg || fb != lb) { ocolor(1, fr, fg, fb); lr = fr; lg = fg; lb = fb; }
                oensure(1); obuf[olen++] = ch;
            }
        }
        ostr("\033[0m");
        lr = lg = lb = br = bg = bb = -1;
        ostr("\r\n");
    }
}

/* Draw the HUD line (and optional help box) after the frame. */
static void emit_hud(int cols, int rows, double fps, int ds, int style, float tscale, int auto_q, int help)
{
    char line[512];
    snprintf(line, sizeof(line),
             " MATRIX CODE | %.1f fps | res %s%s | rain [%s] | %s | time x%.2f%s | [H]elp [X]quit ",
             fps, ssaa ? "2x (ULTRA)" : (ds == 1 ? "1x (NATIVE)" : (ds == 2 ? "1/2" : (ds == 3 ? "1/3" : (ds == 4 ? "1/4" : "1/5")))),
             auto_q ? " auto" : "", g_rain ? "ON" : "OFF", style_names[style], tscale, cam.cine ? " | cinematic" : "");
    ostr("\033[0m\033[30;42m");
    int max_w = cols > 1 ? cols - 1 : 1;
    int n = (int)strlen(line);
    if (n > max_w) n = max_w;
    oensure(cols + 8);
    memcpy(obuf + olen, line, n);
    olen += n;
    for (int i = n; i < max_w; i++) obuf[olen++] = ' ';
    ostr("\033[0m");

    if (help) {
        static const char *lines[] = {
            "  W/S  A/D   move forward/back, strafe      ",
            "  R/F        fly up / down                  ",
            "  Arrows     look around (or I/J/K/L)       ",
            "  C          toggle cinematic orbit camera  ",
            "  B          toggle bullet time / real time ",
            "  [ ]        slower / faster time           ",
            "  P / Space  pause the world                ",
            "  V          change render style            ",
            "  1-5        fixed resolution, 0 = auto     ",
            "  + / -      increase / decrease resolution ",
            "  T          toggle 2x ULTRA super-sampling ",
            "  G          toggle rain ON / OFF           ",
            "  Z / U      zoom (field of view)           ",
            "  H          close this help                ",
            "  X          quit                           ",
        };
        int nl = (int)(sizeof(lines) / sizeof(lines[0]));
        int x0 = (cols - 46) / 2 + 1, y0 = (rows - nl) / 2;
        if (x0 < 1) x0 = 1;
        if (y0 < 1) y0 = 1;
        for (int i = 0; i < nl; i++) {
            ostr("\033[");
            ouint(y0 + i); ostr(";"); ouint(x0); ostr("H\033[0m\033[30;42m");
            ostr(lines[i]);
        }
        ostr("\033[0m");
    }
}

/* ------------------------------------------------------------------------- */
/*  Terminal / input handling                                                */
/* ------------------------------------------------------------------------- */

static struct termios old_termios;
static int term_ready = 0;
static volatile sig_atomic_t running = 1;

static void restore_terminal(void)
{
    if (!term_ready) return;
    term_ready = 0;
    const char *s = "\033[0m\033[?7h\033[?25h\033[?1049l";
    if (write(STDOUT_FILENO, s, strlen(s)) < 0) { }
    tcsetattr(STDIN_FILENO, TCSANOW, &old_termios);
}

static void on_signal(int sig) { (void)sig; running = 0; }

static void setup_terminal(void)
{
    tcgetattr(STDIN_FILENO, &old_termios);
    struct termios raw = old_termios;
    raw.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    fcntl(STDIN_FILENO, F_SETFL, fcntl(STDIN_FILENO, F_GETFL, 0) | O_NONBLOCK);
    const char *s = "\033[?1049h\033[?7l\033[?1000l\033[?1002l\033[?1003l\033[?1006l\033[?25l\033[2J\033[H";
    if (write(STDOUT_FILENO, s, strlen(s)) < 0) { }
    term_ready = 1;
    atexit(restore_terminal);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
}

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* ------------------------------------------------------------------------- */
/*  Scene update helper                                                      */
/* ------------------------------------------------------------------------- */

static void init_scene_constants(void)
{
    /* The moon sits behind the Agents: strong rim light on Neo, long shadows. */
    MOON_DIR = vnorm(v3(-0.42f, 0.55f, -0.72f));
    FILL_DIR = vnorm(v3(0.35f, 0.3f, 0.88f));
    LAMP_POS = v3(6.8f - 2.15f, 2.1f, 5.8f);
    SKYLIGHT_POS = v3(-6.0f, 0.6f, -6.5f);
}

static void update_scene(float t)
{
    update_neo(t);
    update_agents(t);
    update_bullets(t);
}

/* ------------------------------------------------------------------------- */
/*  Main                                                                     */
/* ------------------------------------------------------------------------- */

static int write_ppm(const char *path, int w, int h)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++) {
        unsigned char c[3] = { (unsigned char)q8(img[i].r), (unsigned char)q8(img[i].g), (unsigned char)q8(img[i].b) };
        fwrite(c, 1, 3, f);
    }
    fclose(f);
    return 0;
}

int main(int argc, char **argv)
{
    init_scene_constants();

    /* Headless snapshot for testing: --snapshot file.ppm W H [time [camera_time]] */
    if (argc >= 5 && strcmp(argv[1], "--snapshot") == 0) {
        int w = atoi(argv[3]), h = atoi(argv[4]);
        float t = argc > 5 ? (float)atof(argv[5]) : 1.0f;
        float ct = argc > 6 ? (float)atof(argv[6]) : t;
        g_time = t; g_real = ct;
        update_scene(g_time);
        camera_cinematic(ct);
        if (argc > 9) {
            cam.pos = v3((float)atof(argv[7]), (float)atof(argv[8]), (float)atof(argv[9]));
            camera_look_at(cam.pos, v3(0, 1.0f, 0.2f));
        }
        render_frame(w, h, 0);
        return write_ppm(argv[2], w, h) == 0 ? 0 : 1;
    }

    setup_terminal();

    int ds = 1, auto_q = 0, style = STYLE_CODE, help = 0, paused = 0;
    float tscale = 0.35f;
    int frame = 0;
    double last = now_sec(), fps = 0, smooth_ft = 0.1;
    double target_ft = 0.04;       /* auto quality aims for ~25 fps */

    while (running) {
        double t0 = now_sec();
        double dt = t0 - last;
        last = t0;
        if (dt > 0.25) dt = 0.25;

        struct winsize ws;
        int cols = 120, rows = 40;
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 20 && ws.ws_row > 6) {
            cols = ws.ws_col; rows = ws.ws_row;
        }
        int scene_rows = rows - 1;

        /* --------------------------- input ------------------------------ */
        unsigned char buf[256];
        ssize_t n;
        while ((n = read(STDIN_FILENO, buf, sizeof(buf))) > 0) {
            for (ssize_t i = 0; i < n; i++) {
                int ch = buf[i];
                float acc = 3.2f, rot = 1.6f;
                V3 f = v3(sinf(cam.yaw), 0, cosf(cam.yaw));
                V3 r = v3(-cosf(cam.yaw), 0, sinf(cam.yaw)); /* matches the render basis */
                r = vnorm(vcross(v3(sinf(cam.yaw), 0, cosf(cam.yaw)), v3(0, 1, 0)));
                int moved = 1;
                if (ch == 27) {
                    if (i + 1 < n && (buf[i + 1] == '[' || buf[i + 1] == 'O')) {
                        int is_csi = (buf[i + 1] == '[');
                        ssize_t j = i + 2;
                        if (is_csi) {
                            while (j < n && buf[j] >= 0x20 && buf[j] <= 0x3F) j++;
                        }
                        if (j < n && buf[j] >= 0x40 && buf[j] <= 0x7E) {
                            char final_char = buf[j];
                            if (j == i + 2) {
                                if (final_char == 'A') cam.pitch_v += rot;
                                else if (final_char == 'B') cam.pitch_v -= rot;
                                else if (final_char == 'C') cam.yaw_v += rot;
                                else if (final_char == 'D') cam.yaw_v -= rot;
                                else moved = 0;
                                if (moved) cam.cine = 0;
                            }
                            i = j;
                            continue;
                        }
                    }
                    continue;
                }
                switch (ch) {
                case 'w': case 'W': cam.vel = vadd(cam.vel, vmul(f, acc)); break;
                case 's': case 'S': cam.vel = vadd(cam.vel, vmul(f, -acc)); break;
                case 'd': case 'D': cam.vel = vadd(cam.vel, vmul(r, acc)); break;
                case 'a': case 'A': cam.vel = vadd(cam.vel, vmul(r, -acc)); break;
                case 'r': case 'R': cam.vel.y += acc; break;
                case 'f': case 'F': cam.vel.y -= acc; break;
                case 'j': case 'J': cam.yaw_v -= rot; break;
                case 'l': case 'L': cam.yaw_v += rot; break;
                case 'i': case 'I': cam.pitch_v += rot; break;
                case 'k': case 'K': cam.pitch_v -= rot; break;
                default: moved = 0; break;
                }
                if (moved) cam.cine = 0;

                switch (ch) {
                case 'x': case 'X': running = 0; break;
                case 'c': case 'C': cam.cine = !cam.cine; break;
                case 'h': case 'H': help = !help; break;
                case 'p': case 'P': case ' ': paused = !paused; break;
                case 'v': case 'V': style = (style + 1) % N_STYLES; break;
                case 'g': case 'G': g_rain = !g_rain; break;
                case 'b': case 'B': tscale = tscale < 0.7f ? 1.0f : 0.35f; break;
                case '[': tscale = fmaxf(0.05f, tscale * 0.7f); break;
                case ']': tscale = fminf(3.0f, tscale / 0.7f); break;
                case 'z': case 'Z': cam.fov = clampf(cam.fov - 0.1f, 0.4f, 1.8f); break;
                case 'u': case 'U': cam.fov = clampf(cam.fov + 0.1f, 0.4f, 1.8f); break;
                case '+': case '=': case 't': case 'T':
                    if (ds > 1) { ds--; }
                    else { ssaa = !ssaa; }
                    auto_q = 0; break;
                case '-': case '_':
                    if (ssaa) { ssaa = 0; }
                    else if (ds < 5) { ds++; }
                    auto_q = 0; break;
                case '0': auto_q = 1; break;
                case '1': case '2': case '3': case '4': case '5': auto_q = 0; ds = ch - '0'; ssaa = 0; break;
                default: break;
                }
            }
        }

        /* ---------------------- simulation update ----------------------- */
        g_real += (float)dt;
        if (!paused) g_time += (float)dt * tscale;

        if (cam.cine) {
            camera_cinematic(g_real);
        } else {
            cam.pos = vadd(cam.pos, vmul(cam.vel, (float)dt));
            cam.yaw += cam.yaw_v * (float)dt;
            cam.pitch = clampf(cam.pitch + cam.pitch_v * (float)dt, -1.45f, 1.45f);
            float damp = expf(-4.0f * (float)dt);
            cam.vel = vmul(cam.vel, damp);
            cam.yaw_v *= damp; cam.pitch_v *= damp;
            /* Keep inside the arena (above the roof and within the city cell). */
            cam.pos.x = clampf(cam.pos.x, -13.0f, 13.0f);
            cam.pos.z = clampf(cam.pos.z, -13.0f, 13.0f);
            cam.pos.y = clampf(cam.pos.y, 0.15f, 45.0f);
            camera_push_out();
        }
        update_scene(g_time);

        /* --------------------------- render ----------------------------- */
        int pw = (cols + ds - 1) / ds;
        int ph = (scene_rows * 2 + ds - 1) / ds;
        if (ssaa && ds == 1) { pw *= 2; ph *= 2; }
        render_frame(pw, ph, frame++);
        emit_frame(cols, scene_rows, ds, style);
        emit_hud(cols, rows, fps, ds, style, tscale, auto_q, help);
        {
            ssize_t off = 0;
            while (off < (ssize_t)olen) {
                ssize_t w = write(STDOUT_FILENO, obuf + off, olen - off);
                if (w < 0) { if (errno == EAGAIN) continue; break; }
                off += w;
            }
        }

        /* ------------------------ adaptive quality ---------------------- */
        double ft = now_sec() - t0;
        smooth_ft = smooth_ft * 0.8 + ft * 0.2;
        fps = fps * 0.8 + (1.0 / (ft > 1e-4 ? ft : 1e-4)) * 0.2;
        if (auto_q && frame % 6 == 0) {
            if (smooth_ft > target_ft * 1.25 && ds < 6) ds++;
            else if (smooth_ft < target_ft * 0.5 && ds > 1) ds--;
        }
        if (ft < 0.03) usleep((useconds_t)((0.03 - ft) * 1e6));
    }

    restore_terminal();
    return 0;
}
