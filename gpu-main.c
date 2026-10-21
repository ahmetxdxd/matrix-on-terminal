/*
 * ============================================================================
 *  THE MATRIX: ROOFTOP BULLET-TIME (GPU TRUECOLOR TERMINAL EDITION)
 * ============================================================================
 *
 *  A real-time 3D ray-marched terminal simulation of the iconic Matrix rooftop
 *  bullet-dodge scene, accelerated by the GPU:
 *
 *    - Offscreen OpenGL 3.3+ / GLSL raymarching of the complete scene SDF
 *    - Offloads all heavy mathematical raymarching from CPU to GPU
 *    - Neo limbo backbend dodge, analytic IK legs, coat physics, sunglasses
 *    - 3 Agents shooting with extended pistols and muzzle flashes
 *    - 18 bullets with glowing brass cores and aerodynamic shockwave trails
 *    - Wet rooftop with puddles, raindrop ripples, and reflections
 *    - Water tower, stairwell, incandescent lamp, AC unit, chimney, skylight
 *    - Infinite procedural night city with lit windows and antenna beacons
 *    - 24-bit TrueColor half-block rendering, ACES tone-mapping, film grain
 *    - Rain toggle ([G]), 2x Ultra Super-Sampling SSAA ([+] / [-] / [T])
 *
 *  Build:
 *      gcc -O3 -march=native -ffast-math gpu-main.c -lglfw -lGL -lm -o gpu-main
 *  Run:
 *      ./gpu-main
 * ============================================================================
 */

#define _DEFAULT_SOURCE
#include <errno.h>
#include <fcntl.h>
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

#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GLFW/glfw3.h>

#define PI 3.14159265358979323846f

/* ------------------------------------------------------------------------- */
/*  Vector Math Library                                                      */
/* ------------------------------------------------------------------------- */

typedef struct { float x, y, z; } V3;

static inline V3 v3(float x, float y, float z) { V3 r = {x, y, z}; return r; }
static inline V3 vadd(V3 a, V3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline V3 vsub(V3 a, V3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline V3 vmul(V3 a, float s) { return v3(a.x * s, a.y * s, a.z * s); }
static inline float vdot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline float vlen(V3 a) { return sqrtf(vdot(a, a)); }
static inline V3 vnorm(V3 a) { float l = vlen(a); return l > 1e-6f ? vmul(a, 1.0f / l) : v3(0, 0, 0); }
static inline V3 vcross(V3 a, V3 b) { return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static inline float clampf(float v, float mn, float mx) { return v < mn ? mn : (v > mx ? mx : v); }

/* ------------------------------------------------------------------------- */
/*  Procedural Hashes & Math                                                 */
/* ------------------------------------------------------------------------- */

static inline uint32_t ihash(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352dU;
    x ^= x >> 15; x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

static inline float hash2(int a, int b)
{
    return (ihash((uint32_t)a * 374761393U + (uint32_t)b * 668265263U + 0x9e3779b9U) & 0xffffff) / 16777216.0f;
}

/* ------------------------------------------------------------------------- */
/*  Animation & Physics Structures                                           */
/* ------------------------------------------------------------------------- */

#define N_AGENTS 3
#define N_BULLETS 18

typedef struct {
    V3 hip;
    float lean, lc, ls;
    float hc, hs;
    V3 knee[2], ankle[2];
    V3 shoulder[2], elbow[2], hand[2];
    V3 tail_a[3], tail_b[3];
} NeoPose;

typedef struct {
    V3 pos, fwd, right, muzzle;
    float flash;
} Agent;

typedef struct {
    int active;
    V3 pos, dir;
    float trail;
} Bullet;

typedef struct {
    V3 a, b;
    V3 col;
    float core, halo, power;
} Glow;

static NeoPose neo;
static Agent agents[N_AGENTS];
static Bullet bullets[N_BULLETS];
static Glow glows[N_BULLETS * 2 + N_AGENTS];
static int n_glows = 0;

static float g_time = 0.0f;
static float g_real = 0.0f;
static int   g_rain = 1;        /* Rain ON / OFF toggle */
static int   ssaa = 0;          /* 2x Super-Sampling toggle */

/* ------------------------------------------------------------------------- */
/*  Procedural Scene Animation (CPU)                                         */
/* ------------------------------------------------------------------------- */

static void ik_solve_leg(V3 hip, V3 foot, float l1, float l2, V3 fwd_hint, V3 *knee)
{
    V3 d = vsub(foot, hip);
    float dist = vlen(d);
    float max_len = (l1 + l2) * 0.999f;
    if (dist > max_len) { d = vmul(d, max_len / dist); dist = max_len; }
    if (dist < 0.05f) dist = 0.05f;

    float cos_a = (l1 * l1 + dist * dist - l2 * l2) / (2.0f * l1 * dist);
    cos_a = clampf(cos_a, -1.0f, 1.0f);
    float sin_a = sqrtf(fmaxf(0.0f, 1.0f - cos_a * cos_a));

    V3 dir = vnorm(d);
    V3 side = vcross(dir, fwd_hint);
    if (vlen(side) < 1e-4f) side = vcross(dir, v3(1, 0, 0));
    side = vnorm(side);
    V3 bend = vcross(side, dir);

    *knee = vadd(hip, vadd(vmul(dir, l1 * cos_a), vmul(bend, l1 * sin_a)));
}

static void update_neo(float t)
{
    float cycle = fmodf(t * 0.65f, 2.0f * PI);
    float lean_deg = 58.0f + 24.0f * sinf(cycle);
    float lean = lean_deg * (PI / 180.0f);
    neo.lean = lean;
    neo.lc = cosf(lean);
    neo.ls = sinf(lean);

    float head_tilt = -lean * 0.55f + 0.12f * sinf(cycle * 1.5f);
    neo.hc = cosf(head_tilt);
    neo.hs = sinf(head_tilt);

    neo.hip = v3(0.0f, 0.78f - 0.12f * (lean / (PI * 0.5f)), 0.0f);

    V3 lfoot = v3(-0.28f, 0.05f, -0.22f);
    V3 rfoot = v3( 0.28f, 0.05f,  0.28f);
    V3 lhip = vadd(neo.hip, v3(-0.16f, 0, 0));
    V3 rhip = vadd(neo.hip, v3( 0.16f, 0, 0));

    ik_solve_leg(lhip, lfoot, 0.44f, 0.46f, v3(0.1f, 0.2f, 1.0f), &neo.knee[0]);
    ik_solve_leg(rhip, rfoot, 0.44f, 0.46f, v3(-0.1f, 0.2f, 1.0f), &neo.knee[1]);
    neo.ankle[0] = lfoot;
    neo.ankle[1] = rfoot;

    neo.shoulder[0] = v3(-0.25f, 0.50f, 0.02f);
    neo.shoulder[1] = v3( 0.25f, 0.50f, 0.02f);

    float a0 = -0.8f + 0.35f * sinf(cycle + 0.4f);
    float a1 =  0.9f + 0.30f * cosf(cycle - 0.2f);
    neo.elbow[0] = vadd(neo.shoulder[0], v3(-0.22f, 0.05f, a0 * 0.25f));
    neo.elbow[1] = vadd(neo.shoulder[1], v3( 0.22f, 0.05f, a1 * 0.25f));
    neo.hand[0]  = vadd(neo.elbow[0],    v3(-0.18f, 0.12f, a0 * 0.35f));
    neo.hand[1]  = vadd(neo.elbow[1],    v3( 0.18f, 0.12f, a1 * 0.35f));

    for (int i = 0; i < 3; i++) {
        float x = (i - 1) * 0.14f;
        float ph = t * 4.0f + i * 1.3f;
        float sway_x = sinf(ph) * 0.06f;
        float sway_z = -0.45f - 0.22f * (lean / (PI * 0.5f)) + cosf(ph * 0.8f) * 0.08f;
        neo.tail_a[i] = v3(x + sway_x * 0.5f, 0.35f, sway_z * 0.5f);
        neo.tail_b[i] = v3(x + sway_x,        0.05f, sway_z);
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
        float r1 = hash2(i * 7 + 1, cyc), r2 = hash2(i * 13 + 5, cyc * 3 + 1), r3 = hash2(i * 3 + 9, cyc + 5);
        V3 target = v3((r1 - 0.5f) * 0.9f, 0.7f + r2 * 1.2f, 0.25f + (r3 - 0.5f) * 0.5f);
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
/*  Camera Controller                                                        */
/* ------------------------------------------------------------------------- */

typedef struct {
    V3 pos;
    float yaw, pitch;
    float fov;
    V3 vel;
    float yaw_v, pitch_v;
    int cine;
} Camera;

static Camera cam;

static void camera_look_at(V3 from, V3 to)
{
    cam.pos = from;
    V3 d = vsub(to, from);
    cam.yaw = atan2f(d.x, d.z);
    cam.pitch = asinf(clampf(d.y / vlen(d), -1, 1));
}

static void camera_cinematic(float rt)
{
    float ang = rt * 0.17f + 0.6f;
    float rad = 2.8f + 0.6f * sinf(rt * 0.11f);
    float height = 1.15f + 0.65f * sinf(rt * 0.23f) + 0.2f;
    cam.pos = v3(sinf(ang) * rad, height, 0.3f + cosf(ang) * rad);
    cam.pos.x = clampf(cam.pos.x, -8.5f, 8.5f);
    cam.pos.z = clampf(cam.pos.z, -8.5f, 8.5f);
    camera_look_at(cam.pos, v3(0, 1.05f + 0.1f * sinf(rt * 0.3f), 0.2f));
    cam.fov = 1.0f - 0.2f * sinf(rt * 0.19f);
}

/* ------------------------------------------------------------------------- */
/*  Terminal Setup & ANSI Output                                             */
/* ------------------------------------------------------------------------- */

static struct termios orig_termios;
static int term_configured = 0;
static volatile sig_atomic_t running = 1;

static void reset_terminal(void)
{
    if (!term_configured) return;
    const char *s = "\033[0m\033[?7h\033[?25h\033[?1049l";
    if (write(STDOUT_FILENO, s, strlen(s)) < 0) { }
    tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
    term_configured = 0;
}

static void on_signal(int sig)
{
    (void)sig;
    running = 0;
}

static void setup_terminal(void)
{
    if (tcgetattr(STDIN_FILENO, &orig_termios) == -1) {
        perror("tcgetattr");
        exit(1);
    }
    atexit(reset_terminal);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGWINCH, SIG_IGN);

    struct termios raw = orig_termios;
    raw.c_lflag &= ~(tcflag_t)(ICANON | ECHO | ISIG | IEXTEN);
    raw.c_iflag &= ~(tcflag_t)(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    raw.c_cflag |= (CS8);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);

    int fl = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, fl | O_NONBLOCK);

    term_configured = 1;
    // Enter alternate buffer, disable line autowrap, disable mouse tracking, hide cursor, clear screen
    const char *s = "\033[?1049h\033[?7l\033[?1000l\033[?1002l\033[?1003l\033[?1006l\033[?25l\033[2J\033[H";
    if (write(STDOUT_FILENO, s, strlen(s)) < 0) { }
}

static char *obuf = NULL;
static size_t olen = 0, ocap = 0;

static void oensure(size_t n)
{
    if (olen + n > ocap) {
        ocap = (olen + n) * 2 + 4096;
        obuf = (char *)realloc(obuf, ocap);
    }
}

static inline void ostr(const char *s)
{
    size_t n = strlen(s);
    oensure(n);
    memcpy(obuf + olen, s, n);
    olen += n;
}

static inline void ouint(int v)
{
    char tmp[12]; int n = 0;
    if (v == 0) { tmp[n++] = '0'; }
    else {
        char rev[12]; int rn = 0;
        while (v > 0) { rev[rn++] = '0' + (v % 10); v /= 10; }
        while (rn > 0) tmp[n++] = rev[--rn];
    }
    oensure(n);
    memcpy(obuf + olen, tmp, n);
    olen += n;
}

static inline void ocolor(int fg, int r, int g, int b)
{
    oensure(24);
    if (fg) {
        obuf[olen++] = '\033'; obuf[olen++] = '['; obuf[olen++] = '3'; obuf[olen++] = '8';
        obuf[olen++] = ';'; obuf[olen++] = '2'; obuf[olen++] = ';';
    } else {
        obuf[olen++] = '\033'; obuf[olen++] = '['; obuf[olen++] = '4'; obuf[olen++] = '8';
        obuf[olen++] = ';'; obuf[olen++] = '2'; obuf[olen++] = ';';
    }
    ouint(r); obuf[olen++] = ';';
    ouint(g); obuf[olen++] = ';';
    ouint(b); obuf[olen++] = 'm';
}

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* ------------------------------------------------------------------------- */
/*  GLSL Full Raymarching Shaders                                            */
/* ------------------------------------------------------------------------- */

static const char *vs_src =
"#version 330 core\n"
"layout (location = 0) in vec2 aPos;\n"
"out vec2 uv;\n"
"void main() {\n"
"    uv = aPos * 0.5 + 0.5;\n"
"    gl_Position = vec4(aPos, 0.0, 1.0);\n"
"}\n";

static const char *fs_src =
"#version 330 core\n"
"out vec4 FragColor;\n"
"\n"
"uniform vec2 u_resolution;\n"
"uniform float u_time;\n"
"uniform float u_real_time;\n"
"uniform vec3 u_cam_pos;\n"
"uniform vec3 u_cam_fwd;\n"
"uniform vec3 u_cam_right;\n"
"uniform vec3 u_cam_up;\n"
"uniform float u_cam_fov;\n"
"uniform int u_rain;\n"
"\n"
"#define M_NONE 0\n"
"#define M_ROOF 1\n"
"#define M_PARAPET 2\n"
"#define M_TANK 3\n"
"#define M_METAL 4\n"
"#define M_DOOR 5\n"
"#define M_LAMP 6\n"
"#define M_GREEN 7\n"
"#define M_RED 8\n"
"#define M_BUILD 9\n"
"#define M_COAT 10\n"
"#define M_SUIT 11\n"
"#define M_SKIN 12\n"
"#define M_HAIR 13\n"
"#define M_GLASS 14\n"
"#define M_BOOT 15\n"
"#define M_SHIRT 16\n"
"#define M_TIE 17\n"
"#define M_GUN 18\n"
"#define M_FLASH 19\n"
"\n"
"float sd_sphere(vec3 p, float r) { return length(p) - r; }\n"
"float sd_box(vec3 p, vec3 b) {\n"
"    vec3 q = abs(p) - b;\n"
"    return length(max(q, 0.0)) + min(max(q.x, max(q.y, q.z)), 0.0);\n"
"}\n"
"float sd_round_box(vec3 p, vec3 b, float r) {\n"
"    vec3 q = abs(p) - b + r;\n"
"    return length(max(q, 0.0)) + min(max(q.x, max(q.y, q.z)), 0.0) - r;\n"
"}\n"
"float sd_capsule(vec3 p, vec3 a, vec3 b, float r) {\n"
"    vec3 pa = p - a, ba = b - a;\n"
"    float h = clamp(dot(pa, ba) / dot(ba, ba), 0.0, 1.0);\n"
"    return length(pa - ba * h) - r;\n"
"}\n"
"float sd_cyl_y(vec3 p, float h, float r) {\n"
"    vec2 d = abs(vec2(length(p.xz), p.y)) - vec2(r, h);\n"
"    return min(max(d.x, d.y), 0.0) + length(max(d, 0.0));\n"
"}\n"
"float sd_round_cone(vec3 p, vec3 a, vec3 b, float r1, float r2) {\n"
"    vec3 ba = b - a;\n"
"    float l2 = dot(ba, ba);\n"
"    vec3 pa = p - a;\n"
"    float y = dot(pa, ba);\n"
"    float x = length(pa * l2 - ba * y);\n"
"    float l = sqrt(l2);\n"
"    float k = r1 - r2;\n"
"    float ca = sqrt(max(0.0, l2 - k * k));\n"
"    float cb = k;\n"
"    float f = clamp((x * ca + y * cb) / (l2 * l), 0.0, 1.0);\n"
"    return length(pa - ba * f) - mix(r1, r2, f);\n"
"}\n"
"float sd_ellipsoid(vec3 p, vec3 r) {\n"
"    float k0 = length(p / r);\n"
"    float k1 = length(p / (r * r));\n"
"    return k0 * (k0 - 1.0) / max(k1, 1e-4);\n"
"}\n"
"float smin(float a, float b, float k) {\n"
"    float h = max(k - abs(a - b), 0.0) / k;\n"
"    return min(a, b) - h * h * h * k * (1.0 / 6.0);\n"
"}\n"
"float hash2(float x, float y) {\n"
"    return fract(sin(x * 127.1 + y * 311.7) * 43758.5453123);\n"
"}\n"
"float vnoise(float x, float y) {\n"
"    float ix = floor(x), iy = floor(y);\n"
"    float fx = fract(x), fy = fract(y);\n"
"    float ux = fx * fx * (3.0 - 2.0 * fx);\n"
"    float uy = fy * fy * (3.0 - 2.0 * fy);\n"
"    float a = hash2(ix, iy);\n"
"    float b = hash2(ix + 1.0, iy);\n"
"    float c = hash2(ix, iy + 1.0);\n"
"    float d = hash2(ix + 1.0, iy + 1.0);\n"
"    return mix(mix(a, b, ux), mix(c, d, ux), uy);\n"
"}\n"
"float fbm(float x, float y) {\n"
"    float v = 0.0, a = 0.5;\n"
"    for (int i = 0; i < 4; i++) {\n"
"        v += a * vnoise(x, y);\n"
"        x *= 2.05; y *= 2.05; a *= 0.5;\n"
"    }\n"
"    return v;\n"
"}\n"
"\n"
"void opt(inout float bd, inout int bm, float d, int m) {\n"
"    if (d < bd) { bd = d; bm = m; }\n"
"}\n"
"\n"
"const vec3 LAMP_POS = vec3(4.88, 2.05, 5.8);\n"
"const vec3 SKYLIGHT_POS = vec3(0.0, 0.2, -5.2);\n"
"#define ROOF_HALF 9.5\n"
"#define CITY_CELL 22.0\n"
"\n"
"void sdf_city(vec3 p, inout float d, inout int m) {\n"
"    float roof = sd_box(p - vec3(0.0, -60.0, 0.0), vec3(ROOF_HALF, 60.0, ROOF_HALF));\n"
"    opt(d, m, roof, M_ROOF);\n"
"    float par = sd_box(p - vec3(0.0, 0.45, 0.0), vec3(ROOF_HALF, 0.45, ROOF_HALF));\n"
"    float inner = sd_box(p - vec3(0.0, 0.45, 0.0), vec3(ROOF_HALF - 0.45, 1.0, ROOF_HALF - 0.45));\n"
"    par = max(par, -inner);\n"
"    float cap = sd_box(p - vec3(0.0, 0.93, 0.0), vec3(ROOF_HALF + 0.08, 0.05, ROOF_HALF + 0.08));\n"
"    cap = max(cap, -sd_box(p - vec3(0.0, 0.93, 0.0), vec3(ROOF_HALF - 0.5, 1.0, ROOF_HALF - 0.5)));\n"
"    opt(d, m, min(par, cap), M_PARAPET);\n"
"\n"
"    float ix = floor((p.x + CITY_CELL * 0.5) / CITY_CELL);\n"
"    float iz = floor((p.z + CITY_CELL * 0.5) / CITY_CELL);\n"
"    if (ix == 0.0 && iz == 0.0) return;\n"
"\n"
"    vec3 q = vec3(p.x - ix * CITY_CELL, p.y, p.z - iz * CITY_CELL);\n"
"    float h1 = hash2(ix, iz), h2 = hash2(ix + 91.0, iz + 13.0), h3 = hash2(ix - 37.0, iz + 71.0);\n"
"    float h4 = hash2(ix + 5.0, iz - 53.0), h5 = hash2(ix - 19.0, iz - 7.0);\n"
"\n"
"    float top = -8.0 + 54.0 * h1 * h1;\n"
"    float hx = 4.5 + 4.0 * h2, hz = 4.5 + 4.0 * h3;\n"
"    float jx = (h4 - 0.5) * 3.0, jz = (h5 - 0.5) * 3.0;\n"
"    float hy = (top + 120.0) * 0.5;\n"
"    float bld = sd_box(q - vec3(jx, top - hy, jz), vec3(hx, hy, hz));\n"
"    if (h1 > 0.30) {\n"
"        float tier = sd_box(q - vec3(jx, top + 1.6, jz), vec3(hx * 0.6, 1.6, hz * 0.6));\n"
"        bld = min(bld, tier);\n"
"    }\n"
"    bld *= 0.85;\n"
"    opt(d, m, bld, M_BUILD);\n"
"    if (h1 > 0.62) {\n"
"        float mast = sd_capsule(q, vec3(jx, top + 3.0, jz), vec3(jx, top + 11.0, jz), 0.09);\n"
"        opt(d, m, mast, M_METAL);\n"
"        opt(d, m, sd_sphere(q - vec3(jx, top + 11.2, jz), 0.22), M_RED);\n"
"    }\n"
"}\n"
"\n"
"void sdf_props(vec3 p, inout float d, inout int m) {\n"
"    vec3 T = vec3(-6.5, 0.0, 5.5);\n"
"    float db = length(p - (T + vec3(0.0, 3.2, 0.0))) - 3.9;\n"
"    if (db > 0.5) { opt(d, m, db, M_METAL); }\n"
"    else {\n"
"        vec3 q = p - T;\n"
"        vec3 qa = vec3(abs(q.x), q.y, abs(q.z));\n"
"        opt(d, m, sd_capsule(qa, vec3(1.0, 0.0, 1.0), vec3(0.85, 3.0, 0.85), 0.08), M_METAL);\n"
"        opt(d, m, sd_capsule(qa, vec3(1.0, 0.6, 1.0), vec3(0.0, 1.9, 1.0), 0.03), M_METAL);\n"
"        opt(d, m, sd_capsule(qa, vec3(1.0, 0.6, 1.0), vec3(1.0, 1.9, 0.0), 0.03), M_METAL);\n"
"        opt(d, m, sd_box(q - vec3(0.0, 3.05, 0.0), vec3(1.15, 0.06, 1.15)), M_METAL);\n"
"        float tank = sd_cyl_y(q - vec3(0.0, 4.2, 0.0), 1.2, 1.3);\n"
"        float hoop = abs(length(q.xz) - 1.33);\n"
"        float hy = abs(abs(q.y - 4.2) - 0.75);\n"
"        float hoops = max(hoop - 0.04, hy - 0.05);\n"
"        hoops = max(hoops, abs(q.y - 4.2) - 1.2);\n"
"        opt(d, m, tank, M_TANK);\n"
"        opt(d, m, hoops, M_METAL);\n"
"        opt(d, m, sd_round_cone(q, vec3(0.0, 5.4, 0.0), vec3(0.0, 6.3, 0.0), 1.3, 0.08), M_TANK);\n"
"        opt(d, m, sd_capsule(q, vec3(-1.0, 0.0, -1.45), vec3(-1.0, 5.2, -1.45), 0.03), M_METAL);\n"
"        opt(d, m, sd_capsule(q, vec3(-0.7, 0.0, -1.45), vec3(-0.7, 5.2, -1.45), 0.03), M_METAL);\n"
"    }\n"
"\n"
"    vec3 S = vec3(6.8, 0.0, 5.8);\n"
"    db = length(p - (S + vec3(0.0, 1.6, 0.0))) - 3.5;\n"
"    if (db > 0.5) { opt(d, m, db, M_PARAPET); }\n"
"    else {\n"
"        vec3 q = p - S;\n"
"        float body = sd_box(q - vec3(0.0, 1.6, 0.0), vec3(2.0, 1.6, 1.6));\n"
"        body = max(body, -sd_box(q - vec3(-2.0, 0.95, 0.0), vec3(0.2, 0.95, 0.62)));\n"
"        opt(d, m, body, M_PARAPET);\n"
"        opt(d, m, sd_box(q - vec3(0.0, 3.28, 0.0), vec3(2.18, 0.1, 1.78)), M_PARAPET);\n"
"        opt(d, m, sd_box(q - vec3(-1.82, 0.9, 0.0), vec3(0.05, 0.9, 0.55)), M_DOOR);\n"
"        opt(d, m, sd_box(q - vec3(-1.76, 0.9, 0.4), vec3(0.03, 0.04, 0.1)), M_METAL);\n"
"        opt(d, m, sd_sphere(q - (LAMP_POS - S), 0.1), M_LAMP);\n"
"        opt(d, m, sd_box(q - vec3(-2.1, 2.1, 0.0), vec3(0.08, 0.04, 0.04)), M_METAL);\n"
"    }\n"
"\n"
"    db = length(p - vec3(-3.0, 0.6, -2.5)) - 1.6;\n"
"    if (db > 0.4) { opt(d, m, db, M_METAL); }\n"
"    else {\n"
"        vec3 q = p - vec3(-3.0, 0.0, -2.5);\n"
"        opt(d, m, sd_round_box(q - vec3(0.0, 0.5, 0.0), vec3(0.9, 0.5, 0.6), 0.04), M_METAL);\n"
"        float ring = sd_cyl_y(q - vec3(0.0, 1.03, 0.0), 0.04, 0.42);\n"
"        opt(d, m, ring, M_PARAPET);\n"
"        opt(d, m, sd_cyl_y(q - vec3(0.0, 1.0, 0.0), 0.03, 0.35), M_DOOR);\n"
"        opt(d, m, sd_box(q - vec3(0.0, 0.55, 0.6), vec3(0.6, 0.3, 0.02)), M_DOOR);\n"
"    }\n"
"\n"
"    db = length(p - vec3(3.5, 1.2, -1.5)) - 1.8;\n"
"    if (db > 0.4) { opt(d, m, db, M_METAL); }\n"
"    else {\n"
"        vec3 q = p - vec3(3.5, 0.0, -1.5);\n"
"        opt(d, m, sd_cyl_y(q - vec3(0.0, 1.1, 0.0), 1.1, 0.26), M_METAL);\n"
"        opt(d, m, sd_cyl_y(q - vec3(0.0, 2.35, 0.0), 0.05, 0.42), M_METAL);\n"
"        opt(d, m, sd_cyl_y(q - vec3(0.0, 2.2, 0.0), 0.06, 0.30), M_METAL);\n"
"        opt(d, m, sd_cyl_y(q - vec3(0.0, 0.1, 0.0), 0.1, 0.45), M_PARAPET);\n"
"    }\n"
"\n"
"    db = length(p - vec3(-5.75, 0.5, 2.5)) - 3.4;\n"
"    if (db > 0.4) { opt(d, m, db, M_METAL); }\n"
"    else {\n"
"        vec3 q = p - vec3(-5.75, 0.0, 2.5);\n"
"        opt(d, m, sd_capsule(q, vec3(0.0, 0.25, -2.5), vec3(0.0, 0.25, 2.5), 0.08), M_METAL);\n"
"        opt(d, m, sd_capsule(q, vec3(0.0, 0.5, -2.5), vec3(0.0, 0.5, 2.5), 0.05), M_METAL);\n"
"        opt(d, m, sd_capsule(q, vec3(0.0, 0.5, -2.5), vec3(0.0, 0.0, -2.5), 0.05), M_METAL);\n"
"        opt(d, m, sd_capsule(q, vec3(0.0, 0.5, 2.5), vec3(0.0, 0.0, 2.5), 0.05), M_METAL);\n"
"    }\n"
"\n"
"    db = length(p - vec3(0.0, 0.2, -5.2)) - 2.5;\n"
"    if (db > 0.4) { opt(d, m, db, M_GREEN); }\n"
"    else {\n"
"        vec3 q = p - vec3(0.0, 0.0, -5.2);\n"
"        opt(d, m, sd_box(q - vec3(0.0, 0.15, 0.0), vec3(1.6, 0.15, 1.1)), M_PARAPET);\n"
"        opt(d, m, sd_box(q - vec3(0.0, 0.32, 0.0), vec3(1.4, 0.06, 0.9)), M_GREEN);\n"
"        opt(d, m, sd_box(q - vec3(0.0, 0.36, 0.0), vec3(0.04, 0.03, 0.92)), M_METAL);\n"
"        opt(d, m, sd_box(q - vec3(0.0, 0.36, 0.0), vec3(1.42, 0.03, 0.04)), M_METAL);\n"
"    }\n"
"\n"
"    db = length(p - vec3(4.8, 0.5, 1.0)) - 1.6;\n"
"    if (db > 0.4) { opt(d, m, db, M_TANK); }\n"
"    else {\n"
"        opt(d, m, sd_box(p - vec3(4.6, 0.35, 0.8), vec3(0.45, 0.35, 0.45)), M_TANK);\n"
"        opt(d, m, sd_box(p - vec3(5.1, 0.25, 1.4), vec3(0.35, 0.25, 0.35)), M_TANK);\n"
"        opt(d, m, sd_box(p - vec3(4.7, 0.85, 0.9), vec3(0.3, 0.2, 0.3)), M_TANK);\n"
"    }\n"
"}\n"
"\n"
"uniform vec3 u_neo_hip;\n"
"uniform float u_neo_lc, u_neo_ls;\n"
"uniform float u_neo_hc, u_neo_hs;\n"
"uniform vec3 u_neo_knee[2];\n"
"uniform vec3 u_neo_ankle[2];\n"
"uniform vec3 u_neo_shoulder[2];\n"
"uniform vec3 u_neo_elbow[2];\n"
"uniform vec3 u_neo_hand[2];\n"
"uniform vec3 u_neo_tail_a[3];\n"
"uniform vec3 u_neo_tail_b[3];\n"
"\n"
"void sdf_neo(vec3 p, inout float d, inout int m) {\n"
"    float db = length(p - vec3(0.0, 0.8, 0.0)) - 1.8;\n"
"    if (db > 0.4) { opt(d, m, db, M_COAT); return; }\n"
"\n"
"    for (int s = 0; s < 2; s++) {\n"
"        float x = (s == 0) ? -0.18 : 0.18;\n"
"        opt(d, m, sd_round_cone(p, vec3(x, 0.65, 0.0), u_neo_knee[s], 0.08, 0.065), M_COAT);\n"
"        opt(d, m, sd_round_cone(p, u_neo_knee[s], u_neo_ankle[s], 0.065, 0.05), M_COAT);\n"
"        vec3 foot_end = u_neo_ankle[s] + vec3(0.0, -0.04, 0.18);\n"
"        opt(d, m, sd_capsule(p, u_neo_ankle[s] + vec3(0.0, -0.04, 0.0), foot_end, 0.05), M_BOOT);\n"
"    }\n"
"\n"
"    for (int i = 0; i < 3; i++) {\n"
"        float x = (float(i) - 1.0) * 0.14;\n"
"        vec3 t0 = vec3(x, 0.65, -0.08);\n"
"        opt(d, m, sd_round_cone(p, t0, u_neo_tail_a[i], 0.09, 0.07), M_COAT);\n"
"        opt(d, m, sd_round_cone(p, u_neo_tail_a[i], u_neo_tail_b[i], 0.07, 0.04), M_COAT);\n"
"    }\n"
"\n"
"    vec3 r = p - u_neo_hip;\n"
"    vec3 q = vec3(r.x, r.y * u_neo_lc + r.z * u_neo_ls, -r.y * u_neo_ls + r.z * u_neo_lc);\n"
"\n"
"    opt(d, m, sd_capsule(q, vec3(-0.1, 0.0, 0.0), vec3(0.1, 0.0, 0.0), 0.14), M_COAT);\n"
"    opt(d, m, sd_round_cone(q, vec3(0.0, 0.0, 0.0), vec3(0.0, 0.5, 0.0), 0.16, 0.2), M_COAT);\n"
"    opt(d, m, sd_capsule(q, vec3(-0.21, 0.55, 0.0), vec3(0.21, 0.55, 0.0), 0.085), M_COAT);\n"
"    opt(d, m, sd_box(q - vec3(0.0, 0.38, -0.185), vec3(0.035, 0.14, 0.012)), M_SHIRT);\n"
"    opt(d, m, sd_box(q - vec3(0.0, 0.02, -0.15), vec3(0.17, 0.02, 0.03)), M_BOOT);\n"
"\n"
"    for (int s = 0; s < 2; s++) {\n"
"        opt(d, m, sd_round_cone(q, u_neo_shoulder[s], u_neo_elbow[s], 0.078, 0.062), M_COAT);\n"
"        opt(d, m, sd_round_cone(q, u_neo_elbow[s], u_neo_hand[s], 0.062, 0.045), M_COAT);\n"
"        opt(d, m, sd_ellipsoid(q - u_neo_hand[s], vec3(0.05, 0.05, 0.065)), M_SKIN);\n"
"    }\n"
"\n"
"    opt(d, m, sd_capsule(q, vec3(0.0, 0.56, 0.0), vec3(0.0, 0.67, 0.0), 0.05), M_SKIN);\n"
"\n"
"    vec3 hr = q - vec3(0.0, 0.66, 0.0);\n"
"    vec3 hq = vec3(hr.x, hr.y * u_neo_hc - hr.z * u_neo_hs, hr.y * u_neo_hs + hr.z * u_neo_hc);\n"
"    float skin = sd_ellipsoid(hq - vec3(0.0, 0.15, -0.01), vec3(0.088, 0.115, 0.1));\n"
"    skin = smin(skin, sd_ellipsoid(hq - vec3(0.0, 0.07, -0.035), vec3(0.065, 0.07, 0.07)), 0.04);\n"
"    float hair = sd_ellipsoid(hq - vec3(0.0, 0.185, 0.025), vec3(0.097, 0.115, 0.105));\n"
"    hair = max(hair, -(hq.y - 0.15) * 0.6 - (-hq.z) * 0.4);\n"
"    opt(d, m, skin, M_SKIN);\n"
"    opt(d, m, hair, M_HAIR);\n"
"    opt(d, m, sd_sphere(hq - vec3(0.0, 0.13, -0.098), 0.016), M_SKIN);\n"
"    float glasses = sd_round_box(hq - vec3(0.0, 0.17, -0.092), vec3(0.088, 0.02, 0.014), 0.008);\n"
"    float lens = min(sd_ellipsoid(hq - vec3(-0.04, 0.168, -0.094), vec3(0.035, 0.022, 0.016)),\n"
"                     sd_ellipsoid(hq - vec3(0.04, 0.168, -0.094), vec3(0.035, 0.022, 0.016)));\n"
"    opt(d, m, min(glasses, lens), M_GLASS);\n"
"    opt(d, m, sd_capsule(hq, vec3(-0.085, 0.17, -0.07), vec3(-0.09, 0.17, 0.03), 0.006), M_GLASS);\n"
"    opt(d, m, sd_capsule(hq, vec3(0.085, 0.17, -0.07), vec3(0.09, 0.17, 0.03), 0.006), M_GLASS);\n"
"}\n"
"\n"
"uniform vec3 u_agent_pos[3];\n"
"uniform vec3 u_agent_fwd[3];\n"
"uniform vec3 u_agent_right[3];\n"
"uniform float u_agent_flash[3];\n"
"\n"
"void sdf_agent(vec3 p, int i, inout float d, inout int m) {\n"
"    vec3 a_pos = u_agent_pos[i];\n"
"    vec3 c = a_pos + vec3(0.0, 0.95, 0.0);\n"
"    float db = length(p - c) - 1.6;\n"
"    if (db > 0.4) { opt(d, m, db, M_SUIT); return; }\n"
"\n"
"    vec3 diff = p - a_pos;\n"
"    vec3 q = vec3(dot(diff, u_agent_right[i]), diff.y, dot(diff, u_agent_fwd[i]));\n"
"\n"
"    for (int s = -1; s <= 1; s += 2) {\n"
"        float x = 0.1 * float(s);\n"
"        opt(d, m, sd_round_cone(q, vec3(x, 0.95, 0.0), vec3(x * 1.1, 0.5, 0.02), 0.105, 0.08), M_SUIT);\n"
"        opt(d, m, sd_round_cone(q, vec3(x * 1.1, 0.5, 0.02), vec3(x * 1.2, 0.09, 0.0), 0.08, 0.055), M_SUIT);\n"
"        opt(d, m, sd_capsule(q, vec3(x * 1.2, 0.06, 0.0), vec3(x * 1.2, 0.05, 0.18), 0.055), M_BOOT);\n"
"    }\n"
"    opt(d, m, sd_round_cone(q, vec3(0.0, 0.95, 0.0), vec3(0.0, 1.45, 0.0), 0.17, 0.2), M_SUIT);\n"
"    opt(d, m, sd_capsule(q, vec3(-0.22, 1.5, 0.0), vec3(0.22, 1.5, 0.0), 0.085), M_SUIT);\n"
"    opt(d, m, sd_round_box(q - vec3(0.0, 1.34, 0.16), vec3(0.05, 0.17, 0.03), 0.01), M_SHIRT);\n"
"    opt(d, m, sd_round_box(q - vec3(0.0, 1.30, 0.185), vec3(0.014, 0.15, 0.01), 0.005), M_TIE);\n"
"    opt(d, m, sd_capsule(q, vec3(0.0, 1.52, 0.0), vec3(0.0, 1.62, 0.0), 0.05), M_SKIN);\n"
"    float skin = sd_ellipsoid(q - vec3(0.0, 1.72, 0.0), vec3(0.088, 0.115, 0.1));\n"
"    skin = smin(skin, sd_ellipsoid(q - vec3(0.0, 1.64, 0.035), vec3(0.065, 0.07, 0.07)), 0.04);\n"
"    opt(d, m, skin, M_SKIN);\n"
"    float hair = sd_ellipsoid(q - vec3(0.0, 1.755, -0.025), vec3(0.095, 0.11, 0.105));\n"
"    hair = max(hair, -(q.y - 1.72) * 0.6 + q.z * 0.4);\n"
"    opt(d, m, hair, M_HAIR);\n"
"    opt(d, m, sd_round_box(q - vec3(0.0, 1.74, 0.092), vec3(0.088, 0.02, 0.014), 0.008), M_GLASS);\n"
"    opt(d, m, sd_ellipsoid(q - vec3(-0.04, 1.738, 0.094), vec3(0.035, 0.022, 0.016)), M_GLASS);\n"
"    opt(d, m, sd_ellipsoid(q - vec3(0.04, 1.738, 0.094), vec3(0.035, 0.022, 0.016)), M_GLASS);\n"
"    opt(d, m, sd_capsule(q, vec3(0.09, 1.72, 0.0), vec3(0.07, 1.52, -0.02), 0.005), M_METAL);\n"
"\n"
"    opt(d, m, sd_round_cone(q, vec3(0.22, 1.5, 0.0), vec3(0.23, 1.5, 0.32), 0.075, 0.06), M_SUIT);\n"
"    opt(d, m, sd_round_cone(q, vec3(0.23, 1.5, 0.32), vec3(0.13, 1.52, 0.58), 0.06, 0.045), M_SUIT);\n"
"    opt(d, m, sd_sphere(q - vec3(0.12, 1.52, 0.62), 0.05), M_SKIN);\n"
"    opt(d, m, sd_round_box(q - vec3(0.12, 1.575, 0.72), vec3(0.022, 0.032, 0.14), 0.008), M_GUN);\n"
"    opt(d, m, sd_round_box(q - vec3(0.12, 1.51, 0.64), vec3(0.02, 0.06, 0.025), 0.008), M_GUN);\n"
"    opt(d, m, sd_round_cone(q, vec3(-0.22, 1.5, 0.0), vec3(-0.28, 1.3, 0.2), 0.075, 0.06), M_SUIT);\n"
"    opt(d, m, sd_round_cone(q, vec3(-0.28, 1.3, 0.2), vec3(0.04, 1.5, 0.56), 0.06, 0.045), M_SUIT);\n"
"    opt(d, m, sd_sphere(q - vec3(0.05, 1.5, 0.58), 0.048), M_SKIN);\n"
"\n"
"    if (u_agent_flash[i] > 0.01) {\n"
"        float f = u_agent_flash[i];\n"
"        opt(d, m, sd_ellipsoid(q - vec3(0.12, 1.575, 0.88), vec3(0.05 * f, 0.05 * f, 0.16 * f)), M_FLASH);\n"
"    }\n"
"}\n"
"\n"
"float map(vec3 p, out int m) {\n"
"    float d = 1e9;\n"
"    m = M_NONE;\n"
"    sdf_city(p, d, m);\n"
"    sdf_props(p, d, m);\n"
"    sdf_neo(p, d, m);\n"
"    for (int i = 0; i < 3; i++) {\n"
"        sdf_agent(p, i, d, m);\n"
"    }\n"
"    return d;\n"
"}\n"
"\n"
"float map_d(vec3 p) {\n"
"    int m;\n"
"    return map(p, m);\n"
"}\n"
"\n"
"vec3 calc_normal(vec3 p, float eps) {\n"
"    vec3 k0 = vec3(1.0, -1.0, -1.0);\n"
"    vec3 k1 = vec3(-1.0, -1.0, 1.0);\n"
"    vec3 k2 = vec3(-1.0, 1.0, -1.0);\n"
"    vec3 k3 = vec3(1.0, 1.0, 1.0);\n"
"    return normalize(k0 * map_d(p + k0 * eps) +\n"
"                     k1 * map_d(p + k1 * eps) +\n"
"                     k2 * map_d(p + k2 * eps) +\n"
"                     k3 * map_d(p + k3 * eps));\n"
"}\n"
"\n"
"const vec3 MOON_DIR = vec3(-0.35, 0.78, 0.52);\n"
"\n"
"float soft_shadow(vec3 ro, vec3 rd, float tmax) {\n"
"    float res = 1.0;\n"
"    float t = 0.03;\n"
"    for (int i = 0; i < 24; i++) {\n"
"        float d = map_d(ro + rd * t);\n"
"        res = min(res, 10.0 * d / t);\n"
"        if (res < 0.01) return 0.0;\n"
"        t += clamp(d, 0.04, 1.2);\n"
"        if (t > tmax) break;\n"
"    }\n"
"    res = clamp(res, 0.0, 1.0);\n"
"    return res * res * (3.0 - 2.0 * res);\n"
"}\n"
"\n"
"float calc_ao(vec3 p, vec3 n) {\n"
"    float occ = 0.0, sca = 1.0;\n"
"    for (int i = 1; i <= 4; i++) {\n"
"        float hgt = 0.02 + 0.12 * float(i);\n"
"        float d = map_d(p + n * hgt);\n"
"        occ += (hgt - d) * sca;\n"
"        sca *= 0.8;\n"
"    }\n"
"    return clamp(1.0 - 2.0 * occ, 0.0, 1.0);\n"
"}\n"
"\n"
"vec3 sky_color(vec3 rd) {\n"
"    float y = clamp(rd.y, -0.2, 1.0);\n"
"    vec3 horizon = vec3(0.06, 0.17, 0.15), zenith = vec3(0.004, 0.018, 0.03);\n"
"    vec3 col = mix(horizon, zenith, pow(clamp(y * 1.3, 0.0, 1.0), 0.55));\n"
"\n"
"    float md = dot(rd, MOON_DIR);\n"
"    col += vec3(0.55, 0.85, 0.95) * (0.25 * pow(clamp(md, 0.0, 1.0), 28.0));\n"
"    col += vec3(0.2, 0.4, 0.45) * (0.18 * pow(clamp(md, 0.0, 1.0), 6.0));\n"
"    if (md > 0.9993) col += vec3(1.6, 2.0, 2.0);\n"
"\n"
"    if (rd.y > 0.0) {\n"
"        float u = atan(rd.x, rd.z) * 90.0, v = asin(clamp(rd.y, -1.0, 1.0)) * 90.0;\n"
"        float h = hash2(floor(u), floor(v));\n"
"        if (h > 0.985) {\n"
"            float fu = fract(u) - 0.5, fv = fract(v) - 0.5;\n"
"            float s = exp(-(fu * fu + fv * fv) * 18.0) * (h - 0.985) * 55.0;\n"
"            col += vec3(0.7, 0.9, 1.0) * (s * clamp(rd.y * 3.0, 0.0, 1.0));\n"
"        }\n"
"    }\n"
"    if (rd.y > 0.02) {\n"
"        float k = 1.0 / (rd.y + 0.15);\n"
"        float cx = rd.x * k * 1.4 + u_real_time * 0.01, cz = rd.z * k * 1.4;\n"
"        float c = smoothstep(0.45, 0.85, fbm(cx, cz));\n"
"        float lit = 0.25 + 0.75 * pow(clamp(md, 0.0, 1.0), 2.0);\n"
"        vec3 ccol = mix(vec3(0.025, 0.06, 0.065), vec3(0.18, 0.32, 0.34), lit);\n"
"        col = mix(col, ccol, c * 0.65 * clamp(rd.y * 4.0, 0.0, 1.0));\n"
"    }\n"
"    col += vec3(0.05, 0.12, 0.08) * exp(-abs(rd.y) * 7.0);\n"
"    return col;\n"
"}\n"
"\n"
"vec3 point_light(vec3 p, vec3 n, vec3 lp, vec3 col, float k) {\n"
"    vec3 d = lp - p;\n"
"    float r2 = dot(d, d);\n"
"    float l = inversesqrt(r2);\n"
"    float nd = max(dot(n, d * l), 0.0);\n"
"    return col * (nd * k / (1.0 + r2 * 0.35));\n"
"}\n"
"\n"
"void facade(vec3 p, vec3 n, out vec3 albedo, out vec3 emit, out float refl) {\n"
"    float ix = floor((p.x + CITY_CELL * 0.5) / CITY_CELL);\n"
"    float iz = floor((p.z + CITY_CELL * 0.5) / CITY_CELL);\n"
"    float side = abs(n.x) > abs(n.z) ? (n.x > 0.0 ? 1.0 : 2.0) : (n.z > 0.0 ? 3.0 : 4.0);\n"
"    float u = abs(n.x) > abs(n.z) ? p.z : p.x;\n"
"    float wx = 1.9, wy = 2.7;\n"
"    float gu = u / wx, gv = p.y / wy;\n"
"    float fu = fract(gu), fv = fract(gv);\n"
"    float cu = floor(gu), cv = floor(gv);\n"
"\n"
"    albedo = vec3(0.035, 0.04, 0.045);\n"
"    emit = vec3(0.0);\n"
"    refl = 0.0;\n"
"    float conc = vnoise(p.x * 2.1 + p.z * 1.7, p.y * 2.3) * 0.5 + 0.5;\n"
"    albedo *= 0.6 + 0.6 * conc;\n"
"\n"
"    bool win = fu > 0.18 && fu < 0.82 && fv > 0.22 && fv < 0.82;\n"
"    if (win) {\n"
"        float hl = hash2(cu + ix * 53.0 + side * 7.0, cv + iz * 31.0 + side * 11.0);\n"
"        float hc = hash2(cu + 7.0 + ix * 13.0, cv + 3.0 + iz * 5.0 + side);\n"
"        albedo = vec3(0.01, 0.014, 0.02);\n"
"        refl = 0.35;\n"
"        if (hl < 0.38) {\n"
"            vec3 c;\n"
"            if (hc < 0.55) c = vec3(1.0, 0.74, 0.42);\n"
"            else if (hc < 0.8) c = vec3(0.6, 0.9, 0.95);\n"
"            else c = vec3(0.4, 1.0, 0.55);\n"
"            float inner = 0.6 + 0.4 * hash2(cu * 3.0, cv * 7.0 + side);\n"
"            emit = c * (1.3 * inner);\n"
"            refl = 0.05;\n"
"        }\n"
"    }\n"
"}\n"
"\n"
"void roof_surface(vec3 p, out vec3 albedo, out float refl, out float puddle, out vec3 n_pert) {\n"
"    float asphalt = vnoise(p.x * 3.4, p.z * 3.4) * 0.5 + 0.5;\n"
"    float wear = vnoise(p.x * 0.7, p.z * 0.7);\n"
"    albedo = vec3(0.04, 0.045, 0.048) * (0.7 + 0.5 * asphalt);\n"
"    puddle = smoothstep(0.48, 0.72, wear);\n"
"    refl = mix(0.08, 0.65, puddle);\n"
"    albedo = mix(albedo, vec3(0.015, 0.02, 0.02), puddle);\n"
"    n_pert = vec3(0.0, 1.0, 0.0);\n"
"    if (u_rain != 0 && puddle > 0.05) {\n"
"        vec2 rip = vec2(0.0);\n"
"        for (int i = 0; i < 4; i++) {\n"
"            vec2 c = vec2(hash2(float(i) * 17.0, 3.0), hash2(float(i) * 31.0, 9.0)) * 16.0 - 8.0;\n"
"            vec2 d = p.xz - c;\n"
"            float r = length(d);\n"
"            float ph = u_real_time * 5.0 + float(i) * 1.7;\n"
"            float ring = sin(22.0 * r - 16.0 * ph) * exp(-r * 1.8) * exp(-fract(ph) * 3.0);\n"
"            rip += (r > 1e-4 ? d / r : vec2(0.0)) * (ring * 0.035);\n"
"        }\n"
"        n_pert = normalize(vec3(rip.x * puddle, 1.0, rip.y * puddle));\n"
"    }\n"
"}\n"
"\n"
"uniform int u_num_glows;\n"
"uniform vec3 u_glow_a[24];\n"
"uniform vec3 u_glow_b[24];\n"
"uniform vec3 u_glow_col[24];\n"
"uniform vec3 u_glow_param[24];\n"
"\n"
"void ray_seg(vec3 o, vec3 d, vec3 a, vec3 b, out float dist2, out float s_out, out float u_out) {\n"
"    vec3 e = b - a;\n"
"    float le2 = dot(e, e);\n"
"    if (le2 < 1e-8) {\n"
"        vec3 w = a - o;\n"
"        float s = max(dot(w, d), 0.0);\n"
"        vec3 cl = o + d * s;\n"
"        vec3 df = cl - a;\n"
"        dist2 = dot(df, df);\n"
"        s_out = s; u_out = 0.0;\n"
"        return;\n"
"    }\n"
"    vec3 r0 = o - a;\n"
"    float qd = dot(d, e);\n"
"    float dd = dot(r0, d);\n"
"    float de = dot(r0, e);\n"
"    float denom = 1.0 - qd * qd / le2;\n"
"    float u;\n"
"    if (abs(denom) < 1e-6) u = 0.5;\n"
"    else u = (qd * dd / le2 - de / le2) / denom;\n"
"    u = clamp(u, 0.0, 1.0);\n"
"    float s = max(u * qd - dd, 0.0);\n"
"    vec3 pr = o + d * s;\n"
"    vec3 pq = a + e * u;\n"
"    vec3 df = pr - pq;\n"
"    dist2 = dot(df, df);\n"
"    s_out = s;\n"
"    u_out = u;\n"
"}\n"
"\n"
"vec3 add_rain(vec3 col, float u, float v, float rt) {\n"
"    if (u_rain == 0) return col;\n"
"    float acc = 0.0;\n"
"    for (int L = 0; L < 3; L++) {\n"
"        float n = 55.0 + 38.0 * float(L);\n"
"        float xs = u * n + v * n * 0.10;\n"
"        float cx = floor(xs);\n"
"        float hsh = hash2(cx, float(L * 17 + 3));\n"
"        float yy = fract(v * (1.3 + 0.7 * float(L)) + rt * (0.9 + hsh * 0.6 + 0.4 * float(L)) + hsh * 9.0);\n"
"        float len = 0.07 + 0.04 * float(L);\n"
"        float streak = yy < len ? 1.0 - yy / len : 0.0;\n"
"        float width = max(0.0, 1.0 - abs((xs - cx) - 0.5) * 5.5);\n"
"        float on = hsh > 0.45 ? 1.0 : 0.0;\n"
"        acc += streak * width * on * (0.34 - 0.08 * float(L));\n"
"    }\n"
"    return col + vec3(0.55, 0.78, 0.85) * (acc * 0.55);\n"
"}\n"
"\n"
"vec3 tonemap(vec3 c) {\n"
"    c *= vec3(0.86, 1.06, 0.94);\n"
"    vec3 x = c * 0.9;\n"
"    vec3 num = x * (2.51 * x + 0.03);\n"
"    vec3 den = x * (2.43 * x + 0.59) + 0.14;\n"
"    vec3 mapped = clamp(num / den, 0.0, 1.0);\n"
"    return pow(mapped, vec3(1.0 / 2.2));\n"
"}\n"
"\n"
"void main() {\n"
"    vec2 coord = gl_FragCoord.xy / u_resolution;\n"
"    float aspect = u_resolution.x / u_resolution.y;\n"
"    float tan_fov = tan(u_cam_fov * 0.5);\n"
"    float u = (2.0 * coord.x - 1.0) * aspect * tan_fov;\n"
"    float v = (2.0 * coord.y - 1.0) * tan_fov;\n"
"    vec3 rd = normalize(u_cam_fwd + u_cam_right * u + u_cam_up * v);\n"
"    vec3 ro = u_cam_pos;\n"
"\n"
"    float t = 0.01;\n"
"    int mat = M_NONE;\n"
"    bool hit = false;\n"
"    for (int i = 0; i < 90; i++) {\n"
"        vec3 p = ro + rd * t;\n"
"        if (p.y > 52.0 && rd.y >= 0.0) break;\n"
"        int m;\n"
"        float d = map(p, m);\n"
"        if (d < 0.0012 + 0.0007 * t) {\n"
"            mat = m;\n"
"            hit = true;\n"
"            break;\n"
"        }\n"
"        t += d;\n"
"        if (t > 140.0) break;\n"
"    }\n"
"\n"
"    vec3 col = vec3(0.0);\n"
"    float hit_t = hit ? t : 1e6;\n"
"\n"
"    if (!hit) {\n"
"        col = sky_color(rd);\n"
"    } else {\n"
"        vec3 pos = ro + rd * t;\n"
"        vec3 n = calc_normal(pos, 0.0015 + 0.0005 * t);\n"
"        vec3 albedo = vec3(0.0);\n"
"        vec3 emit = vec3(0.0);\n"
"        float refl = 0.0;\n"
"\n"
"        if (mat == M_ROOF) {\n"
"            float puddle;\n"
"            vec3 n_pert;\n"
"            roof_surface(pos, albedo, refl, puddle, n_pert);\n"
"            n = n_pert;\n"
"        } else if (mat == M_PARAPET) {\n"
"            albedo = vec3(0.12, 0.13, 0.125);\n"
"            refl = 0.04;\n"
"        } else if (mat == M_TANK) {\n"
"            albedo = vec3(0.08, 0.05, 0.03);\n"
"            refl = 0.02;\n"
"        } else if (mat == M_METAL) {\n"
"            albedo = vec3(0.15, 0.16, 0.17);\n"
"            refl = 0.35;\n"
"        } else if (mat == M_DOOR) {\n"
"            albedo = vec3(0.05, 0.08, 0.06);\n"
"            refl = 0.08;\n"
"        } else if (mat == M_LAMP) {\n"
"            albedo = vec3(1.0, 0.9, 0.6);\n"
"            emit = vec3(4.0, 3.2, 1.6);\n"
"        } else if (mat == M_GREEN) {\n"
"            albedo = vec3(0.1, 0.4, 0.2);\n"
"            emit = vec3(0.5, 2.0, 0.8);\n"
"        } else if (mat == M_RED) {\n"
"            albedo = vec3(0.8, 0.1, 0.1);\n"
"            emit = vec3(2.5, 0.1, 0.1);\n"
"        } else if (mat == M_BUILD) {\n"
"            facade(pos, n, albedo, emit, refl);\n"
"        } else if (mat == M_COAT) {\n"
"            albedo = vec3(0.018, 0.022, 0.02);\n"
"            refl = 0.14;\n"
"        } else if (mat == M_SUIT) {\n"
"            albedo = vec3(0.025, 0.03, 0.028);\n"
"            refl = 0.05;\n"
"        } else if (mat == M_SKIN) {\n"
"            albedo = vec3(0.42, 0.32, 0.26);\n"
"            refl = 0.06;\n"
"        } else if (mat == M_HAIR) {\n"
"            albedo = vec3(0.012, 0.012, 0.014);\n"
"            refl = 0.04;\n"
"        } else if (mat == M_GLASS) {\n"
"            albedo = vec3(0.01, 0.01, 0.01);\n"
"            refl = 0.85;\n"
"        } else if (mat == M_BOOT) {\n"
"            albedo = vec3(0.015, 0.015, 0.016);\n"
"            refl = 0.22;\n"
"        } else if (mat == M_SHIRT) {\n"
"            albedo = vec3(0.65, 0.68, 0.7);\n"
"            refl = 0.04;\n"
"        } else if (mat == M_TIE) {\n"
"            albedo = vec3(0.02, 0.022, 0.025);\n"
"            refl = 0.06;\n"
"        } else if (mat == M_GUN) {\n"
"            albedo = vec3(0.08, 0.09, 0.09);\n"
"            refl = 0.45;\n"
"        } else if (mat == M_FLASH) {\n"
"            albedo = vec3(1.0, 0.9, 0.5);\n"
"            emit = vec3(6.0, 4.5, 1.8);\n"
"        }\n"
"\n"
"        float sh = soft_shadow(pos + n * 0.02, MOON_DIR, 35.0);\n"
"        float ao = calc_ao(pos, n);\n"
"        float ndl = max(dot(n, MOON_DIR), 0.0);\n"
"        vec3 moon_light = vec3(0.35, 0.55, 0.6) * (ndl * sh * 1.8);\n"
"        vec3 amb = sky_color(n) * (0.45 * ao);\n"
"        vec3 hdir = normalize(MOON_DIR - rd);\n"
"        float spec = pow(max(dot(n, hdir), 0.0), 32.0) * sh * refl;\n"
"        vec3 spec_col = vec3(0.7, 0.85, 0.9) * (spec * 2.2);\n"
"        float fr = pow(1.0 - max(dot(n, -rd), 0.0), 2.5);\n"
"        vec3 rim = vec3(0.25, 0.55, 0.5) * (fr * 0.7 * ao);\n"
"\n"
"        vec3 pl = point_light(pos, n, LAMP_POS, vec3(1.0, 0.7, 0.35), 4.2);\n"
"        pl += point_light(pos, n, SKYLIGHT_POS, vec3(0.2, 1.0, 0.4), 2.5);\n"
"        for (int i = 0; i < 3; i++) {\n"
"            if (u_agent_flash[i] > 0.05) {\n"
"                pl += point_light(pos, n, u_agent_pos[i] + vec3(0.12, 1.575, 0.88), vec3(1.0, 0.8, 0.4), 8.0 * u_agent_flash[i]);\n"
"            }\n"
"        }\n"
"\n"
"        col = albedo * (moon_light + amb + pl + rim) + emit + spec_col;\n"
"        if (refl > 0.08) {\n"
"            vec3 ref_rd = reflect(rd, n);\n"
"            vec3 ref_col = sky_color(ref_rd);\n"
"            col = mix(col, ref_col, refl * (0.25 + 0.75 * fr));\n"
"        }\n"
"        float fog = 1.0 - exp(-t * 0.012);\n"
"        col = mix(col, vec3(0.02, 0.06, 0.065), fog);\n"
"    }\n"
"\n"
"    for (int i = 0; i < u_num_glows; i++) {\n"
"        float d2, s, uu;\n"
"        ray_seg(ro, rd, u_glow_a[i], u_glow_b[i], d2, s, uu);\n"
"        if (s > hit_t) continue;\n"
"        float k = 0.15 + 0.85 * uu * uu;\n"
"        float c2 = u_glow_param[i].x * u_glow_param[i].x;\n"
"        float h2 = u_glow_param[i].y * u_glow_param[i].y;\n"
"        float g = 1.7 * c2 / (d2 + c2) * k + 0.05 * h2 / (d2 + h2) * k;\n"
"        col += u_glow_col[i] * (g * u_glow_param[i].z);\n"
"    }\n"
"\n"
"    for (int i = 0; i < 3; i++) {\n"
"        if (u_agent_flash[i] < 0.01) continue;\n"
"        vec3 m = u_agent_pos[i] + vec3(0.12, 1.575, 0.88);\n"
"        vec3 oc = m - ro;\n"
"        float s = dot(oc, rd);\n"
"        if (s < 0.0 || s > hit_t) continue;\n"
"        vec3 cl = oc - rd * s;\n"
"        float d2 = dot(cl, cl), r2 = 0.14 * 0.14 * u_agent_flash[i];\n"
"        col += vec3(1.0, 0.75, 0.4) * (1.6 * r2 / (d2 + r2) * u_agent_flash[i]);\n"
"    }\n"
"\n"
"    col = add_rain(col, u / (aspect * tan_fov), v / tan_fov, u_real_time * 0.5);\n"
"\n"
"    vec2 vc = coord * 2.0 - 1.0;\n"
"    float vig = 1.0 - 0.35 * (vc.x * vc.x * 0.6 + vc.y * vc.y);\n"
"    col *= vig;\n"
"    float grain = (hash2(gl_FragCoord.x, gl_FragCoord.y) - 0.5) * 0.02;\n"
"    col = tonemap(col) + vec3(grain);\n"
"\n"
"    FragColor = vec4(clamp(col, 0.0, 1.0), 1.0);\n"
"}\n";

/* ------------------------------------------------------------------------- */
/*  GPU State & Framebuffer                                                  */
/* ------------------------------------------------------------------------- */

static GLFWwindow *gl_win = NULL;
static GLuint gl_prog = 0;
static GLuint gl_fbo = 0;
static GLuint gl_tex = 0;
static GLuint gl_vao = 0;
static int cur_fbo_w = 0, cur_fbo_h = 0;
static unsigned char *read_pixels = NULL;

static int init_gpu(void)
{
    if (!glfwInit()) {
        fprintf(stderr, "\n[ERROR] glfwInit failed. Display server or OpenGL not available.\n");
        return 0;
    }

    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    gl_win = glfwCreateWindow(64, 64, "Matrix GPU", NULL, NULL);
    if (!gl_win) {
        fprintf(stderr, "\n[ERROR] Failed to create offscreen OpenGL window.\n");
        glfwTerminate();
        return 0;
    }
    glfwMakeContextCurrent(gl_win);

    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &vs_src, NULL);
    glCompileShader(vs);

    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &fs_src, NULL);
    glCompileShader(fs);

    GLint ok;
    glGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(fs, sizeof(log), NULL, log);
        fprintf(stderr, "\n[ERROR] Fragment shader compilation failed:\n%s\n", log);
        return 0;
    }

    gl_prog = glCreateProgram();
    glAttachShader(gl_prog, vs);
    glAttachShader(gl_prog, fs);
    glLinkProgram(gl_prog);
    glGetProgramiv(gl_prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(gl_prog, sizeof(log), NULL, log);
        fprintf(stderr, "\n[ERROR] Shader linking failed:\n%s\n", log);
        return 0;
    }
    glUseProgram(gl_prog);

    glGenFramebuffers(1, &gl_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, gl_fbo);

    glGenTextures(1, &gl_tex);
    glBindTexture(GL_TEXTURE_2D, gl_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    // Disable 4-byte padding so odd column widths don't misalign rows
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    float quad[] = { -1, -1, 1, -1, -1, 1, -1, 1, 1, -1, 1, 1 };
    GLuint vbo;
    glGenVertexArrays(1, &gl_vao);
    glBindVertexArray(gl_vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, NULL);
    glEnableVertexAttribArray(0);

    return 1;
}

static void update_fbo(int w, int h)
{
    if (w == cur_fbo_w && h == cur_fbo_h) return;
    cur_fbo_w = w;
    cur_fbo_h = h;
    glBindTexture(GL_TEXTURE_2D, gl_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gl_tex, 0);

    free(read_pixels);
    read_pixels = (unsigned char *)malloc((size_t)w * h * 3);
}

/* ------------------------------------------------------------------------- */
/*  Frame Rendering & Terminal Emission                                      */
/* ------------------------------------------------------------------------- */

static void render_gpu(int pw, int ph)
{
    update_fbo(pw, ph);

    glBindFramebuffer(GL_FRAMEBUFFER, gl_fbo);
    glViewport(0, 0, pw, ph);

    float cy = cosf(cam.yaw), sy = sinf(cam.yaw), cp = cosf(cam.pitch), sp = sinf(cam.pitch);
    V3 fwd = v3(sy * cp, sp, cy * cp);
    V3 right = vnorm(vcross(fwd, v3(0, 1, 0)));
    V3 up = vcross(right, fwd);

    glUseProgram(gl_prog);
    glUniform2f(glGetUniformLocation(gl_prog, "u_resolution"), (float)pw, (float)ph);
    glUniform1f(glGetUniformLocation(gl_prog, "u_time"), g_time);
    glUniform1f(glGetUniformLocation(gl_prog, "u_real_time"), g_real);
    glUniform3f(glGetUniformLocation(gl_prog, "u_cam_pos"), cam.pos.x, cam.pos.y, cam.pos.z);
    glUniform3f(glGetUniformLocation(gl_prog, "u_cam_fwd"), fwd.x, fwd.y, fwd.z);
    glUniform3f(glGetUniformLocation(gl_prog, "u_cam_right"), right.x, right.y, right.z);
    glUniform3f(glGetUniformLocation(gl_prog, "u_cam_up"), up.x, up.y, up.z);
    glUniform1f(glGetUniformLocation(gl_prog, "u_cam_fov"), cam.fov);
    glUniform1i(glGetUniformLocation(gl_prog, "u_rain"), g_rain);

    // Neo uniforms
    glUniform3f(glGetUniformLocation(gl_prog, "u_neo_hip"), neo.hip.x, neo.hip.y, neo.hip.z);
    glUniform1f(glGetUniformLocation(gl_prog, "u_neo_lc"), neo.lc);
    glUniform1f(glGetUniformLocation(gl_prog, "u_neo_ls"), neo.ls);
    glUniform1f(glGetUniformLocation(gl_prog, "u_neo_hc"), neo.hc);
    glUniform1f(glGetUniformLocation(gl_prog, "u_neo_hs"), neo.hs);
    glUniform3fv(glGetUniformLocation(gl_prog, "u_neo_knee"), 2, (const GLfloat *)neo.knee);
    glUniform3fv(glGetUniformLocation(gl_prog, "u_neo_ankle"), 2, (const GLfloat *)neo.ankle);
    glUniform3fv(glGetUniformLocation(gl_prog, "u_neo_shoulder"), 2, (const GLfloat *)neo.shoulder);
    glUniform3fv(glGetUniformLocation(gl_prog, "u_neo_elbow"), 2, (const GLfloat *)neo.elbow);
    glUniform3fv(glGetUniformLocation(gl_prog, "u_neo_hand"), 2, (const GLfloat *)neo.hand);
    glUniform3fv(glGetUniformLocation(gl_prog, "u_neo_tail_a"), 3, (const GLfloat *)neo.tail_a);
    glUniform3fv(glGetUniformLocation(gl_prog, "u_neo_tail_b"), 3, (const GLfloat *)neo.tail_b);

    // Agent uniforms
    GLfloat agent_pos[9], agent_fwd[9], agent_right[9], agent_flash[3];
    for (int i = 0; i < N_AGENTS; i++) {
        agent_pos[i * 3 + 0] = agents[i].pos.x; agent_pos[i * 3 + 1] = agents[i].pos.y; agent_pos[i * 3 + 2] = agents[i].pos.z;
        agent_fwd[i * 3 + 0] = agents[i].fwd.x; agent_fwd[i * 3 + 1] = agents[i].fwd.y; agent_fwd[i * 3 + 2] = agents[i].fwd.z;
        agent_right[i * 3 + 0] = agents[i].right.x; agent_right[i * 3 + 1] = agents[i].right.y; agent_right[i * 3 + 2] = agents[i].right.z;
        agent_flash[i] = agents[i].flash;
    }
    glUniform3fv(glGetUniformLocation(gl_prog, "u_agent_pos"), 3, agent_pos);
    glUniform3fv(glGetUniformLocation(gl_prog, "u_agent_fwd"), 3, agent_fwd);
    glUniform3fv(glGetUniformLocation(gl_prog, "u_agent_right"), 3, agent_right);
    glUniform1fv(glGetUniformLocation(gl_prog, "u_agent_flash"), 3, agent_flash);

    // Glow uniforms
    glUniform1i(glGetUniformLocation(gl_prog, "u_num_glows"), n_glows);
    GLfloat ga[24 * 3], gb[24 * 3], gc[24 * 3], gp[24 * 3];
    for (int i = 0; i < n_glows && i < 24; i++) {
        ga[i * 3 + 0] = glows[i].a.x; ga[i * 3 + 1] = glows[i].a.y; ga[i * 3 + 2] = glows[i].a.z;
        gb[i * 3 + 0] = glows[i].b.x; gb[i * 3 + 1] = glows[i].b.y; gb[i * 3 + 2] = glows[i].b.z;
        gc[i * 3 + 0] = glows[i].col.x; gc[i * 3 + 1] = glows[i].col.y; gc[i * 3 + 2] = glows[i].col.z;
        gp[i * 3 + 0] = glows[i].core; gp[i * 3 + 1] = glows[i].halo; gp[i * 3 + 2] = glows[i].power;
    }
    glUniform3fv(glGetUniformLocation(gl_prog, "u_glow_a"), n_glows, ga);
    glUniform3fv(glGetUniformLocation(gl_prog, "u_glow_b"), n_glows, gb);
    glUniform3fv(glGetUniformLocation(gl_prog, "u_glow_col"), n_glows, gc);
    glUniform3fv(glGetUniformLocation(gl_prog, "u_glow_param"), n_glows, gp);

    glBindVertexArray(gl_vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    glReadPixels(0, 0, pw, ph, GL_RGB, GL_UNSIGNED_BYTE, read_pixels);
}

static void emit_terminal_frame(int cols, int rows, int ds)
{
    olen = 0;
    ostr("\033[H");
    int lr = -1, lg = -1, lb = -1, br = -1, bg = -1, bb = -1;
    int scale = (ssaa && ds == 1) ? 2 : 1;

    for (int cy = 0; cy < rows; cy++) {
        for (int cx = 0; cx < cols; cx++) {
            int ix = (cx / ds) * scale;
            if (ix >= cur_fbo_w) ix = cur_fbo_w - 1;

            int iy0 = ((cy * 2) / ds) * scale;
            int iy1 = ((cy * 2 + 1) / ds) * scale;
            if (iy0 >= cur_fbo_h) iy0 = cur_fbo_h - 1;
            if (iy1 >= cur_fbo_h) iy1 = cur_fbo_h - 1;

            // OpenGL has row 0 at the bottom, so flip Y
            int sy0 = cur_fbo_h - 1 - iy0;
            int sy1 = cur_fbo_h - 1 - iy1;

            unsigned char *p0 = &read_pixels[(sy0 * cur_fbo_w + ix) * 3];
            unsigned char *p1 = &read_pixels[(sy1 * cur_fbo_w + ix) * 3];

            int tr = p0[0], tg = p0[1], tb = p0[2];
            int br_val = p1[0], bg_val = p1[1], bb_val = p1[2];

            if (scale == 2 && ix + 1 < cur_fbo_w) {
                unsigned char *p0b = &read_pixels[(sy0 * cur_fbo_w + ix + 1) * 3];
                unsigned char *p1b = &read_pixels[(sy1 * cur_fbo_w + ix + 1) * 3];
                tr = (tr + p0b[0]) / 2; tg = (tg + p0b[1]) / 2; tb = (tb + p0b[2]) / 2;
                br_val = (br_val + p1b[0]) / 2; bg_val = (bg_val + p1b[1]) / 2; bb_val = (bb_val + p1b[2]) / 2;
            }

            tr &= 0xF8; tg &= 0xF8; tb &= 0xF8;
            br_val &= 0xF8; bg_val &= 0xF8; bb_val &= 0xF8;

            if (tr != lr || tg != lg || tb != lb) {
                ocolor(1, tr, tg, tb);
                lr = tr; lg = tg; lb = tb;
            }
            if (br_val != br || bg_val != bg || bb_val != bb) {
                ocolor(0, br_val, bg_val, bb_val);
                br = br_val; bg = bg_val; bb = bb_val;
            }

            oensure(3);
            obuf[olen++] = 0xE2; obuf[olen++] = 0x96; obuf[olen++] = 0x80; // UTF-8 '▀'
        }
        ostr("\033[0m");
        lr = lg = lb = br = bg = bb = -1;
        ostr("\r\n");
    }
}

static void emit_hud(int cols, int rows, double fps, int ds, float tscale, int help)
{
    char line[512];
    snprintf(line, sizeof(line),
             " MATRIX GPU TRUECOLOR | %.1f fps | res %s | rain [%s] | time x%.2f%s | [H]elp [X]quit ",
             fps, ssaa ? "2x (ULTRA)" : (ds == 1 ? "1x (NATIVE)" : (ds == 2 ? "1/2" : (ds == 3 ? "1/3" : "1/4"))),
             g_rain ? "ON" : "OFF", tscale, cam.cine ? " | cinematic" : "");
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
            "  1-4        fixed resolution scaling       ",
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
            char pos[32];
            snprintf(pos, sizeof(pos), "\033[%d;%dH\033[30;47m", y0 + i, x0);
            ostr(pos);
            ostr(lines[i]);
            ostr("\033[0m");
        }
    }
}

/* ------------------------------------------------------------------------- */
/*  Main Loop                                                                */
/* ------------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    if (!init_gpu()) return 1;

    if (argc >= 5 && strcmp(argv[1], "--snapshot") == 0) {
        int w = atoi(argv[3]), h = atoi(argv[4]);
        float t = argc > 5 ? (float)atof(argv[5]) : 1.0f;
        float ct = argc > 6 ? (float)atof(argv[6]) : t;
        g_time = t; g_real = ct;
        update_neo(g_time);
        update_agents(g_time);
        update_bullets(g_time);
        camera_cinematic(ct);
        if (argc > 9) {
            cam.pos = v3((float)atof(argv[7]), (float)atof(argv[8]), (float)atof(argv[9]));
            camera_look_at(cam.pos, v3(0, 1.0f, 0.2f));
        }
        render_gpu(w, h);
        FILE *f = fopen(argv[2], "wb");
        if (!f) return 1;
        fprintf(f, "P6\n%d %d\n255\n", w, h);
        for (int y = 0; y < h; y++) {
            int sy = h - 1 - y;
            fwrite(&read_pixels[sy * w * 3], 1, w * 3, f);
        }
        fclose(f);
        return 0;
    }

    setup_terminal();

    cam.pos = v3(0.0f, 1.25f, -3.2f);
    cam.pitch = 0.05f;
    cam.yaw = 0.0f;
    cam.fov = 1.05f;
    cam.vel = v3(0, 0, 0);
    cam.yaw_v = cam.pitch_v = 0.0f;
    cam.cine = 1;

    int ds = 1, help = 0, paused = 0;
    float tscale = 0.35f;
    double last = now_sec(), fps = 0, smooth_ft = 0.016;

    while (running) {
        double t0 = now_sec();
        double dt = t0 - last;
        last = t0;
        if (dt > 0.25) dt = 0.25;

        struct winsize ws;
        int cols = 120, rows = 40;
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 10 && ws.ws_row > 5) {
            cols = ws.ws_col; rows = ws.ws_row;
        } else if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 10 && ws.ws_row > 5) {
            cols = ws.ws_col; rows = ws.ws_row;
        }
        int scene_rows = rows - 1;

        // Non-blocking keyboard & escape sequence input
        unsigned char buf[512];
        ssize_t n;
        while ((n = read(STDIN_FILENO, buf, sizeof(buf))) > 0) {
            for (ssize_t i = 0; i < n; i++) {
                int ch = buf[i];
                float acc = 3.2f, rot = 1.6f;
                V3 f = v3(sinf(cam.yaw), 0, cosf(cam.yaw));
                V3 r = vnorm(vcross(v3(sinf(cam.yaw), 0, cosf(cam.yaw)), v3(0, 1, 0)));
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
                case 'g': case 'G': g_rain = !g_rain; break;
                case 'b': case 'B': tscale = tscale < 0.7f ? 1.0f : 0.35f; break;
                case '[': tscale = fmaxf(0.05f, tscale * 0.7f); break;
                case ']': tscale = fminf(3.0f, tscale / 0.7f); break;
                case 'z': case 'Z': cam.fov = clampf(cam.fov - 0.1f, 0.4f, 1.8f); break;
                case 'u': case 'U': cam.fov = clampf(cam.fov + 0.1f, 0.4f, 1.8f); break;
                case '+': case '=': case 't': case 'T':
                    if (ds > 1) { ds--; }
                    else { ssaa = !ssaa; }
                    break;
                case '-': case '_':
                    if (ssaa) { ssaa = 0; }
                    else if (ds < 4) { ds++; }
                    break;
                case '1': case '2': case '3': case '4': ds = ch - '0'; ssaa = 0; break;
                default: break;
                }
            }
        }

        if (!paused) {
            g_time += (float)dt * tscale;
            g_real += (float)dt;
        }

        if (cam.cine) {
            camera_cinematic(g_real);
        } else {
            cam.pos = vadd(cam.pos, vmul(cam.vel, (float)dt));
            cam.yaw += cam.yaw_v * (float)dt;
            cam.pitch += cam.pitch_v * (float)dt;
            cam.pitch = clampf(cam.pitch, -1.35f, 1.35f);
            cam.vel = vmul(cam.vel, expf(-6.0f * (float)dt));
            cam.yaw_v *= expf(-8.0f * (float)dt);
            cam.pitch_v *= expf(-8.0f * (float)dt);
            if (cam.pos.y < 0.25f) cam.pos.y = 0.25f;
            if (cam.pos.y > 45.0f) cam.pos.y = 45.0f;
        }

        update_neo(g_time);
        update_agents(g_time);
        update_bullets(g_time);

        int pw = (cols + ds - 1) / ds;
        int ph = (scene_rows * 2 + ds - 1) / ds;
        if (ssaa && ds == 1) { pw *= 2; ph *= 2; }

        render_gpu(pw, ph);

        emit_terminal_frame(cols, scene_rows, ds);
        emit_hud(cols, rows, fps, ds, tscale, help);

        // Complete write loop ensuring all buffer bytes reach terminal
        ssize_t off = 0;
        while (off < (ssize_t)olen) {
            ssize_t w = write(STDOUT_FILENO, obuf + off, olen - off);
            if (w < 0) {
                if (errno == EAGAIN || errno == EINTR) continue;
                break;
            }
            off += w;
        }

        double frame_time = now_sec() - t0;
        smooth_ft = smooth_ft * 0.9 + frame_time * 0.1;
        fps = smooth_ft > 1e-4 ? 1.0 / smooth_ft : 999.0;

        // Cap frame rate around 60 FPS to avoid terminal flood
        if (frame_time < 0.016) {
            struct timespec req = {0, (long)((0.016 - frame_time) * 1e9)};
            nanosleep(&req, NULL);
        }
    }

    reset_terminal();
    if (gl_win) {
        glfwDestroyWindow(gl_win);
        glfwTerminate();
    }
    return 0;
}
