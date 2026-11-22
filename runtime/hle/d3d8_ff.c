// HLE Direct3D 8: отрисовка поверх GL 4.1 core (Android — ES 3.0, platform/gles_shim.h) — цели рендера, состояние GL, фиксированный конвейер D3D
// (преобразования, освещение по формулам D3D, туман, генерация/матрицы текстурных координат, текстурные стадии,
// альфа-тест), сгенерированный в GLSL по ключу состояния с кешем программ; Present.
// Матрицы D3D (строки, вектор-строка) загружаются в GL как есть: GL читает их по столбцам, получается
// транспонированная — ровно то, что нужно для M * v.
#include <math.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#ifdef __APPLE__
#include <libproc.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

#include "platform/window.h"
#ifdef __ANDROID__
#include "platform/android/perf.h"
#endif
#include "runtime/hle/bink.h"
#include "runtime/hle/d3d8.h"
#include "runtime/game/menu.h"
#include "runtime/script_fix.h"
#include "runtime/hle/overlay.h"
#include "runtime/hle/d3d8_int.h"
#include "runtime/hle/vfs.h"

#ifndef GL_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
#endif

static float u2f(u32 v)
{
    float f;
    memcpy(&f, &v, 4);
    return f;
}

// d3d * d3d (строки): r = a * b
static void mat_mul(float *r, const float *a, const float *b)
{
    float t[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            t[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
    memcpy(r, t, 64);
}

// ================================================================ цели рендера
static void check_fbo(void *arg)
{
    const u32 *rt = arg;
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE)
        fprintf(stderr, "[d3d8] framebuffer incomplete 0x%x (rt %ux%u fmt %u)\n", st, rt[0], rt[1], rt[2]);
}

void d3d8_bind_target(d3ddev *d)
{
    if (!d->fbo)
        glGenFramebuffers(1, &d->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, d->fbo);
    if (d->fbo_rt != d->rt) {
        d3dsurf *s = d->rt;
        if (s && s->tex)
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                   s->tex->cube ? (GLenum)(GL_TEXTURE_CUBE_MAP_POSITIVE_X + s->face) : GL_TEXTURE_2D,
                                   s->tex->gl, s->level);
        else
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, s ? s->rb : 0);
        d->fbo_rt = s;
    }
    if (d->fbo_ds != d->ds) {
        d3dsurf *s = d->ds;
        const d3dfmt *f = s ? d3d8_format(s->fmt) : NULL;
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, 0);
        if (s)
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, f->stencil ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT,
                                      GL_RENDERBUFFER, s->rb);
        d->fbo_ds = s;
    }
    u32 info[3] = {d->rt ? d->rt->w : 0, d->rt ? d->rt->h : 0, d->rt ? d->rt->fmt : 0};
    cs_async(check_fbo, info, sizeof info);  // проверка в потоке рендера, без ожидания
    d->gen_misc++;
}

// ================================================================ ключ фиксированного конвейера
enum { FOG_NONE, FOG_SPECULAR, FOG_VERTEX, FOG_PIXEL };

typedef struct {
    u8 cop, ca1, ca2, ca0, aop, aa1, aa2, aa0, res, tex;  // tex: 0 нет, 1 2D, 2 куб
    u8 tci, tcgen, ttf_count, ttf_proj;
} stagekey;

typedef struct {
    u16 inputs;            // маска регистров D3DVSDE (0..15), есть в вершине
    u8 tcsize[8];          // компонент во входных наборах текстурных координат
    u8 rhw, lighting, normalize, localviewer, specular, colorvertex;
    u8 fog, fogmode, rangefog, alphafunc, clipmask, nstages;
    u8 mat_src[4];         // diffuse, specular, ambient, emissive: 0 материал, 1 color1, 2 color2
    u8 light[MAX_LIGHTS];  // 0 выкл, 1 point, 2 spot, 3 directional
    u32 ps;                // пиксельный шейдер (0 — стадии)
    stagekey st[MAX_STAGES];
#ifdef SNX_GLES
    u16 bgra;              // входы D3DCOLOR (B,G,R,A в памяти): в ES нет размера GL_BGRA — переставляет шейдер
    u8 clipvary;           // плоскости отсечения без GL_EXT_clip_cull_distance: varying и discard
#endif
} ffkey;

typedef struct {
    ffkey key;
    u32 hash;
    GLuint prog;
    GLint u_wv, u_proj, u_world, u_vp, u_depth, u_texm, u_mat, u_power, u_ambient, u_ld, u_ls, u_la, u_lpos,
        u_ldir, u_latt, u_fog, u_fogcolor, u_clip, u_tfactor, u_aref, u_psc, u_bump;
    u32 gen_xf, gen_light, gen_mat, gen_misc;
    int used;
    _Atomic int ready;  // собрана потоком рендера (программы прошлых запусков собираются в фоне)
} ffprog;

#define PROG_CACHE 4096
static ffprog g_progs[PROG_CACHE];
static int g_nprogs;

static u32 fnv(const void *p, size_t n)
{
    const u8 *b = p;
    u32 h = 2166136261u;
    for (size_t i = 0; i < n; i++)
        h = (h ^ b[i]) * 16777619u;
    return h;
}

static void make_key(d3ddev *d, const vdecl *decl, ffkey *k)
{
    memset(k, 0, sizeof *k);
    for (int i = 0; i < decl->n; i++) {
        u32 r = decl->e[i].reg;
        if (r < 16)
            k->inputs |= (u16)(1u << r);
        if (r >= 7 && r < 15) {
            static const u8 comps[8] = {1, 2, 3, 4, 4, 4, 2, 4};
            k->tcsize[r - 7] = comps[decl->e[i].type & 7];
        }
#ifdef SNX_GLES
        if (r < 16 && decl->e[i].type == 4)
            k->bgra |= (u16)(1u << r);
#endif
    }
    const u32 *rs = d->rs;
    k->rhw = (u8)decl->rhw;
    k->lighting = !decl->rhw && rs[137];
    k->specular = rs[29] != 0;
    if (k->lighting) {
        k->normalize = rs[143] != 0;
        k->localviewer = rs[142] != 0;
        k->colorvertex = rs[141] != 0;
        static const u32 src_rs[4] = {145, 146, 147, 148};  // DIFFUSE/SPECULAR/AMBIENT/EMISSIVEMATERIALSOURCE
        for (int i = 0; i < 4; i++) {
            u32 s = k->colorvertex ? rs[src_rs[i]] : 0;
            if (s == 1 && !(k->inputs & (1u << 5)))
                s = 0;
            if (s == 2 && !(k->inputs & (1u << 6)))
                s = 0;
            k->mat_src[i] = (u8)(s <= 2 ? s : 0);
        }
        for (int i = 0; i < MAX_LIGHTS; i++)
            k->light[i] = d->light_on[i] ? (u8)(d->lights[i].type <= 3 ? d->lights[i].type : 3) : 0;
    }
    if (rs[28]) {  // FOGENABLE
        u32 table = rs[35], vert = rs[140];
        if (table) {
            k->fog = FOG_PIXEL;
            k->fogmode = (u8)table;
        } else if (vert && !decl->rhw) {
            k->fog = FOG_VERTEX;
            k->fogmode = (u8)vert;
            k->rangefog = rs[48] != 0;
        } else
            k->fog = FOG_SPECULAR;
    }
    k->alphafunc = rs[15] ? (u8)rs[25] : 8;  // ALPHATESTENABLE ? ALPHAFUNC : ALWAYS
    if (k->alphafunc == 8)
        k->alphafunc = 0;
    k->clipmask = decl->rhw ? 0 : (u8)(rs[152] & 0x3F);
#ifdef SNX_GLES
    k->clipvary = k->clipmask && !gles_caps.clip_distance;
#endif
    k->ps = d->ps;
    int nst = 0;
    for (int s = 0; s < MAX_STAGES; s++) {
        const u32 *t = d->tss[s];
        if (!d->ps && t[1] == 1)  // COLOROP DISABLE: дальше стадий нет
            break;
        if (d->ps && s >= 6)
            break;
        stagekey *sk = &k->st[s];
        if (!d->ps) {
            sk->cop = (u8)t[1];
            sk->ca1 = (u8)t[2];
            sk->ca2 = (u8)t[3];
            sk->ca0 = (u8)t[26];
            sk->aop = (u8)(t[4] == 1 ? 2 : t[4]);  // ALPHAOP DISABLE при включённом цвете — как SELECTARG1(CURRENT)
            sk->aa1 = (u8)(t[4] == 1 ? 1 : t[5]);
            sk->aa2 = (u8)t[6];
            sk->aa0 = (u8)t[27];
            sk->res = (u8)(t[28] == 5);  // RESULTARG: TEMP
        }
        sk->tex = d->tex[s] ? (u8)(d->tex[s]->cube ? 2 : 1) : 0;
        sk->tci = (u8)(t[11] & 7);
        sk->tcgen = (u8)((t[11] >> 16) & 3);
        sk->ttf_count = (u8)(t[24] & 7);
        sk->ttf_proj = (t[24] & 0x100) != 0;
        nst = s + 1;
    }
    k->nstages = (u8)nst;
}

// ================================================================ генерация GLSL
typedef struct {
    char *p;
    size_t n, cap;
} sbuf;

static void put(sbuf *b, const char *fmt, ...)
{
    va_list ap;
    for (;;) {
        va_start(ap, fmt);
        int w = vsnprintf(b->p + b->n, b->cap - b->n, fmt, ap);
        va_end(ap);
        if (w >= 0 && b->n + (size_t)w < b->cap) {
            b->n += (size_t)w;
            return;
        }
        b->cap = b->cap ? b->cap * 2 : 8192;
        b->p = realloc(b->p, b->cap);
    }
}

// вход D3DCOLOR в ES (ffkey.bgra): атрибут читается как есть под именем <имя>_in, а в начале main — перестановка
static int in_bgra(const ffkey *k, int reg)
{
#ifdef SNX_GLES
    return (k->bgra >> reg) & 1;
#else
    (void)k;
    (void)reg;
    return 0;
#endif
}

static int clip_vary(const ffkey *k)
{
#ifdef SNX_GLES
    return k->clipvary;
#else
    (void)k;
    return 0;
#endif
}

static void gen_vs(const ffkey *k, sbuf *b)
{
    put(b, SNX_GLSL_HEADER);
    if (k->clipmask && !clip_vary(k) && SNX_IS_GLES)
        put(b, "#extension GL_EXT_clip_cull_distance : enable\n");
    put(b, SNX_GLSL_PRECISION);
    put(b, "layout(location=0) in vec4 a_pos;\n");
    if (k->inputs & (1u << 3))
        put(b, "layout(location=3) in vec3 a_nrm;\n");
    if (k->inputs & (1u << 5))
        put(b, "layout(location=5) in vec4 a_col0%s;\n", in_bgra(k, 5) ? "_in" : "");
    if (k->inputs & (1u << 6))
        put(b, "layout(location=6) in vec4 a_col1%s;\n", in_bgra(k, 6) ? "_in" : "");
    for (int i = 0; i < 8; i++)
        if (k->inputs & (1u << (7 + i)))
            put(b, "layout(location=%d) in vec4 a_tc%d%s;\n", 7 + i, i, in_bgra(k, 7 + i) ? "_in" : "");
    put(b, "uniform mat4 u_wv, u_proj, u_world, u_texm[8];\n"
           "uniform vec4 u_vp; uniform vec2 u_depth;\n"
           "uniform vec4 u_mat[4]; uniform float u_power; uniform vec4 u_ambient;\n"
           "uniform vec4 u_ld[8], u_ls[8], u_la[8], u_lpos[8], u_ldir[8], u_latt[8];\n"
           "uniform vec4 u_fog; uniform vec4 u_clip[6];\n"
           "out vec4 v_col0, v_col1; out float v_fog;\n");
    for (int s = 0; s < k->nstages; s++)
        put(b, "out vec4 v_tc%d;\n", s);
    if (clip_vary(k))
        put(b, "out float v_clip[6];\n");
    else if (k->clipmask && !SNX_IS_GLES)
        put(b, "out float gl_ClipDistance[6];\n");
    put(b, "void main() {\n");
    if (in_bgra(k, 5) && (k->inputs & (1u << 5)))
        put(b, "  vec4 a_col0 = a_col0_in.zyxw;\n");
    if (in_bgra(k, 6) && (k->inputs & (1u << 6)))
        put(b, "  vec4 a_col1 = a_col1_in.zyxw;\n");
    for (int i = 0; i < 8; i++)
        if (in_bgra(k, 7 + i) && (k->inputs & (1u << (7 + i))))
            put(b, "  vec4 a_tc%d = a_tc%d_in.zyxw;\n", i, i);
    if (k->rhw) {
        // экранные координаты D3D -> NDC; центры пикселей D3D целые (GL: +0,5)
        put(b, "  float w = a_pos.w != 0.0 ? 1.0 / a_pos.w : 1.0;\n"
               "  vec2 ndc = (a_pos.xy + 0.5 - u_vp.xy) / u_vp.zw * 2.0 - 1.0;\n"
               "  float z = (a_pos.z - u_depth.x) / max(u_depth.y - u_depth.x, 1e-6) * 2.0 - 1.0;\n"
               "  gl_Position = vec4(ndc * w, z * w, w);\n"
               "  vec4 epos = vec4(0.0, 0.0, w, 1.0);\n");
    } else {
        put(b, "  vec4 pos = vec4(a_pos.xyz, 1.0);\n"
               "  vec4 epos = u_wv * pos;\n"
               "  vec4 cp = u_proj * epos;\n"
               // полпикселя (центры пикселей D3D целые): вправо и вниз по экрану, а ось y клипа D3D — вверх.
               // С неверным знаком по y полноэкранный квад не накрывал нижнюю строку цели (чёрный край
               // размытой текстуры тени при CLAMP затемнял целые треугольники под проектором).
               "  cp.x += cp.w / u_vp.z;\n"
               "  cp.y -= cp.w / u_vp.w;\n"
               "  cp.y = -cp.y;\n"               // кадр хранится сверху вниз
               "  cp.z = 2.0 * cp.z - cp.w;\n"   // глубина D3D [0,w] -> GL [-w,w]
               "  gl_Position = cp;\n");
        for (int i = 0; i < 6; i++)
            if (k->clipmask & (1u << i))
                put(b, "  %s[%d] = dot(u_world * pos, u_clip[%d]);\n", clip_vary(k) ? "v_clip" : "gl_ClipDistance", i, i);
    }
    int need_en = k->lighting;
    for (int s = 0; s < k->nstages; s++)
        need_en |= k->st[s].tcgen == 1 || k->st[s].tcgen == 3;
    if (need_en && !k->rhw)
        put(b, "  vec3 En = %s;\n", (k->inputs & (1u << 3)) ? "transpose(inverse(mat3(u_wv))) * a_nrm" : "vec3(0.0)");
    const char *c0 = (k->inputs & (1u << 5)) ? "a_col0" : "vec4(1.0)";
    const char *c1 = (k->inputs & (1u << 6)) ? "a_col1" : "vec4(0.0)";
    if (k->lighting) {
        static const char *src[3] = {NULL, "a_col0", "a_col1"};
        const char *md = k->mat_src[0] ? src[k->mat_src[0]] : "u_mat[0]";
        const char *ms = k->mat_src[1] ? src[k->mat_src[1]] : "u_mat[2]";
        const char *ma = k->mat_src[2] ? src[k->mat_src[2]] : "u_mat[1]";
        const char *me = k->mat_src[3] ? src[k->mat_src[3]] : "u_mat[3]";
        put(b, "  vec3 N = En;\n");
        if (k->normalize)
            put(b, "  if (dot(N, N) > 0.0) N = normalize(N);\n");
        put(b, "  vec3 V = %s;\n", k->localviewer ? "normalize(-epos.xyz)" : "vec3(0.0, 0.0, -1.0)");
        put(b, "  vec3 dif = vec3(0.0), amb = vec3(0.0), spe = vec3(0.0);\n");
        for (int i = 0; i < MAX_LIGHTS; i++) {
            if (!k->light[i])
                continue;
            put(b, "  {\n");
            if (k->light[i] == 3)
                put(b, "    vec3 L = -u_ldir[%d].xyz; float att = 1.0;\n", i);
            else {
                put(b, "    vec3 Lv = u_lpos[%d].xyz - epos.xyz; float dist = length(Lv); vec3 L = Lv / max(dist, 1e-6);\n"
                       "    float att = dist > u_lpos[%d].w ? 0.0 : 1.0 / max(u_latt[%d].x + u_latt[%d].y * dist + u_latt[%d].z * dist * dist, 1e-6);\n",
                    i, i, i, i, i);
                if (k->light[i] == 2)  // spot: u_ldir.w = falloff, u_ld.w/u_ls.w — cos(theta/2), cos(phi/2)
                    put(b, "    float rho = dot(-L, u_ldir[%d].xyz);\n"
                           "    att *= rho > u_ld[%d].w ? 1.0 : rho <= u_ls[%d].w ? 0.0 : pow((rho - u_ls[%d].w) / max(u_ld[%d].w - u_ls[%d].w, 1e-6), u_ldir[%d].w);\n",
                        i, i, i, i, i, i, i);
            }
            put(b, "    float nl = max(dot(N, L), 0.0);\n"
                   "    dif += u_ld[%d].rgb * nl * att; amb += u_la[%d].rgb * att;\n"
                   "    if (nl > 0.0 && u_power > 0.0) spe += u_ls[%d].rgb * pow(max(dot(N, normalize(L + V)), 0.0), u_power) * att;\n"
                   "  }\n",
                i, i, i);
        }
        put(b, "  v_col0 = vec4(clamp(%s.rgb + %s.rgb * (u_ambient.rgb + amb) + %s.rgb * dif, 0.0, 1.0), %s.a);\n",
            me, ma, md, md);
        put(b, "  v_col1 = vec4(clamp(%s.rgb * spe, 0.0, 1.0), %s.a);\n", ms, c1);
    } else {
        put(b, "  v_col0 = %s; v_col1 = %s;\n", c0, c1);
    }
    // туман: v_fog — коэффициент (1 — без тумана) или глубина для попиксельного
    switch (k->fog) {
    case FOG_SPECULAR:
        put(b, "  v_fog = v_col1.a;\n");
        break;
    case FOG_VERTEX:
        put(b, "  float fd = %s;\n", k->rangefog ? "length(epos.xyz)" : "abs(epos.z)");
        if (k->fogmode == 3)
            put(b, "  v_fog = (u_fog.y - fd) * u_fog.w;\n");
        else if (k->fogmode == 1)
            put(b, "  v_fog = exp(-u_fog.z * fd);\n");
        else
            put(b, "  v_fog = exp(-(u_fog.z * fd) * (u_fog.z * fd));\n");
        break;
    case FOG_PIXEL:
        put(b, "  v_fog = abs(epos.z);\n");
        break;
    default:
        put(b, "  v_fog = 1.0;\n");
    }
    // текстурные координаты стадий
    for (int s = 0; s < k->nstages; s++) {
        const stagekey *sk = &k->st[s];
        int n;  // компонент исходного вектора
        switch (sk->tcgen) {
        case 1:
            put(b, "  vec4 tc%d = vec4(%s, 1.0);\n", s, k->rhw ? "vec3(0.0)" : "normalize(En)");
            n = 3;
            break;
        case 2:
            put(b, "  vec4 tc%d = vec4(epos.xyz, 1.0);\n", s);
            n = 3;
            break;
        case 3:
            put(b, "  vec4 tc%d = vec4(%s, 1.0);\n", s, k->rhw ? "vec3(0.0)" : "reflect(normalize(epos.xyz), normalize(En))");
            n = 3;
            break;
        default:
            n = k->tcsize[sk->tci];
            if (!(k->inputs & (1u << (7 + sk->tci))))
                put(b, "  vec4 tc%d = vec4(0.0, 0.0, 0.0, 1.0);\n", s), n = 4;
            else if (n == 1)
                put(b, "  vec4 tc%d = vec4(a_tc%d.x, 1.0, 0.0, 0.0);\n", s, sk->tci);
            else if (n == 2)
                put(b, "  vec4 tc%d = vec4(a_tc%d.xy, 1.0, 0.0);\n", s, sk->tci);
            else if (n == 3)
                put(b, "  vec4 tc%d = vec4(a_tc%d.xyz, 1.0);\n", s, sk->tci);
            else
                put(b, "  vec4 tc%d = a_tc%d;\n", s, sk->tci);
        }
        (void)n;
        if (sk->ttf_count)
            put(b, "  tc%d = u_texm[%d] * tc%d;\n", s, s, s);
        put(b, "  v_tc%d = tc%d;\n", s, s);
    }
    put(b, "}\n");
}

static const char *stage_arg(const ffkey *k, int s, u32 a, char *buf, int alpha)
{
    (void)k;
    const char *base;
    switch (a & 7) {
    case 0: base = "v_col0"; break;
    case 1: base = s == 0 ? "v_col0" : "cur"; break;
    case 2: base = "tex"; break;
    case 3: base = "u_tfactor"; break;
    case 4: base = "v_col1"; break;
    case 5: base = "tmp"; break;
    default: base = "cur";
    }
    const char *comp = alpha || (a & 0x20) ? ".aaaa" : "";
    if (a & 0x10)
        sprintf(buf, "(vec4(1.0) - %s%s)", base, comp);
    else
        sprintf(buf, "%s%s", base, comp);
    return buf;
}

// Операция стадии над 4-компонентными аргументами (результат обрезается)
static void stage_op(sbuf *b, const char *dst, u32 op, const char *a1, const char *a2, const char *a0, int s)
{
    switch (op) {
    case 2: put(b, "%s = %s;\n", dst, a1); break;
    case 3: put(b, "%s = %s;\n", dst, a2); break;
    case 4: put(b, "%s = %s * %s;\n", dst, a1, a2); break;
    case 5: put(b, "%s = 2.0 * %s * %s;\n", dst, a1, a2); break;
    case 6: put(b, "%s = 4.0 * %s * %s;\n", dst, a1, a2); break;
    case 7: put(b, "%s = %s + %s;\n", dst, a1, a2); break;
    case 8: put(b, "%s = %s + %s - 0.5;\n", dst, a1, a2); break;
    case 9: put(b, "%s = 2.0 * (%s + %s - 0.5);\n", dst, a1, a2); break;
    case 10: put(b, "%s = %s - %s;\n", dst, a1, a2); break;
    case 11: put(b, "%s = %s + %s - %s * %s;\n", dst, a1, a2, a1, a2); break;
    case 12: put(b, "%s = mix(%s, %s, v_col0.a);\n", dst, a2, a1); break;
    case 13: put(b, "%s = mix(%s, %s, tex.a);\n", dst, a2, a1); break;
    case 14: put(b, "%s = mix(%s, %s, u_tfactor.a);\n", dst, a2, a1); break;
    case 15: put(b, "%s = %s + %s * (1.0 - tex.a);\n", dst, a1, a2); break;
    case 16: put(b, "%s = mix(%s, %s, %s.a);\n", dst, a2, a1, s == 0 ? "v_col0" : "cur"); break;
    case 17: put(b, "%s = %s;\n", dst, a1); break;  // PREMODULATE: модуляция следующей стадией не делается
    case 18: put(b, "%s = %s + %s.a * %s;\n", dst, a1, a1, a2); break;
    case 19: put(b, "%s = %s * %s + %s.a;\n", dst, a1, a2, a1); break;
    case 20: put(b, "%s = (1.0 - %s.a) * %s + %s;\n", dst, a1, a2, a1); break;
    case 21: put(b, "%s = (vec4(1.0) - %s) * %s + %s.a;\n", dst, a1, a2, a1); break;
    case 22: case 23: put(b, "%s = %s;\n", dst, a2); break;  // BUMPENVMAP*: смещение считает следующая стадия
    case 24: put(b, "%s = vec4(clamp(4.0 * dot(%s.rgb - 0.5, %s.rgb - 0.5), 0.0, 1.0));\n", dst, a1, a2); break;
    case 25: put(b, "%s = %s + %s * %s;\n", dst, a0, a1, a2); break;
    case 26: put(b, "%s = mix(%s, %s, %s);\n", dst, a2, a1, a0); break;
    default: put(b, "%s = %s;\n", dst, a1);
    }
}

static void gen_fs(const ffkey *k, sbuf *b)
{
    put(b, SNX_GLSL_HEADER);
    put(b, SNX_GLSL_PRECISION);
    put(b, "in vec4 v_col0, v_col1; in float v_fog;\n");
    if (clip_vary(k))
        put(b, "in float v_clip[6];\n");
    for (int s = 0; s < k->nstages; s++)
        put(b, "in vec4 v_tc%d;\n", s);
    for (int s = 0; s < k->nstages; s++)
        if (k->st[s].tex || k->ps)
            put(b, "uniform %s u_tex%d;\n", k->st[s].tex == 2 ? "samplerCube" : "sampler2D", s);
    put(b, "uniform vec4 u_tfactor, u_fogcolor, u_fog, u_psc[8], u_bump[8]; uniform float u_aref;\n"
           "out vec4 o_color;\n"
           "void main() {\n"
           "  vec4 cur = v_col0, tmp = vec4(0.0), tex = vec4(1.0);\n");
    if (clip_vary(k))  // плоскости отсечения D3D (ES без GL_EXT_clip_cull_distance)
        for (int i = 0; i < 6; i++)
            if (k->clipmask & (1u << i))
                put(b, "  if (v_clip[%d] < 0.0) discard;\n", i);
    if (k->ps) {
        // интерфейс тела ps.1.x (d3d8_ps.c): v0, v1, tc0..tc5, u_tex0..5, u_psc[]; результат — в cur
        put(b, "  vec4 v0 = v_col0, v1 = v_col1;\n");
        for (int s = 0; s < 6; s++) {
            if (s < k->nstages)
                put(b, "  vec4 tc%d = v_tc%d;\n", s, s);
            else
                put(b, "  vec4 tc%d = vec4(0.0);\n", s);
        }
        const char *body = d3d8_ps_glsl(k->ps);
        put(b, "%s\n", body ? body : "  cur = v_col0;\n");
    } else {
        for (int s = 0; s < k->nstages; s++) {
            const stagekey *sk = &k->st[s];
            put(b, "  // stage %d\n", s);
            int uses_tex = (sk->ca1 & 7) == 2 || (sk->ca2 & 7) == 2 || (sk->ca0 & 7) == 2 || (sk->aa1 & 7) == 2 ||
                           (sk->aa2 & 7) == 2 || (sk->aa0 & 7) == 2 || sk->cop == 13 || sk->cop == 15 ||
                           sk->aop == 13 || sk->aop == 15 || sk->cop == 22 || sk->cop == 23;
            if (uses_tex && sk->tex) {
                char uv[96];
                int n = sk->ttf_count ? sk->ttf_count : 2;
                if (sk->tex == 2)
                    snprintf(uv, sizeof uv, "v_tc%d.xyz", s);
                else if (sk->ttf_proj && n >= 2)
                    snprintf(uv, sizeof uv, "v_tc%d.xy / v_tc%d.%c", s, s, "xyzw"[n - 1]);
                else
                    snprintf(uv, sizeof uv, "v_tc%d.xy", s);
                // BUMPENVMAP на предыдущей стадии: смещение (du, dv) её текстуры через матрицу BUMPENVMAT
                if (s > 0 && (k->st[s - 1].cop == 22 || k->st[s - 1].cop == 23) && sk->tex == 1)
                    put(b, "  tex = texture(u_tex%d, %s + vec2(dot(bump.xy, u_bump[%d].xz), dot(bump.xy, u_bump[%d].yw)));\n",
                        s, uv, s - 1, s - 1);
                else
                    put(b, "  tex = texture(u_tex%d, %s);\n", s, uv);
            } else if (uses_tex)
                put(b, "  tex = vec4(1.0);\n");
            if (sk->cop == 22 || sk->cop == 23)
                put(b, "  vec4 bump = tex;\n");
            char a1[64], a2[64], a0[64];
            put(b, "  { vec4 rc, ra;\n    ");
            stage_op(b, "rc", sk->cop, stage_arg(k, s, sk->ca1, a1, 0), stage_arg(k, s, sk->ca2, a2, 0),
                     stage_arg(k, s, sk->ca0, a0, 0), s);
            if (sk->cop == 24)  // DOTPRODUCT3 — во все каналы, и в альфу
                put(b, "    ra = rc;\n");
            else {
                put(b, "    ");
                stage_op(b, "ra", sk->aop, stage_arg(k, s, sk->aa1, a1, 1), stage_arg(k, s, sk->aa2, a2, 1),
                         stage_arg(k, s, sk->aa0, a0, 1), s);
            }
            put(b, "    %s = clamp(vec4(rc.rgb, ra.a), 0.0, 1.0); }\n", sk->res ? "tmp" : "cur");
        }
    }
    if (k->specular)
        put(b, "  cur.rgb = clamp(cur.rgb + v_col1.rgb, 0.0, 1.0);\n");
    if (k->fog) {
        if (k->fog == FOG_PIXEL) {
            if (k->fogmode == 3)
                put(b, "  float f = (u_fog.y - v_fog) * u_fog.w;\n");
            else if (k->fogmode == 1)
                put(b, "  float f = exp(-u_fog.z * v_fog);\n");
            else
                put(b, "  float f = exp(-(u_fog.z * v_fog) * (u_fog.z * v_fog));\n");
        } else
            put(b, "  float f = v_fog;\n");
        put(b, "  cur.rgb = mix(u_fogcolor.rgb, cur.rgb, clamp(f, 0.0, 1.0));\n");
    }
    if (k->alphafunc) {
        static const char *cmp[9] = {"", "false", "<", "==", "<=", ">", "!=", ">=", "true"};
        if (k->alphafunc == 1)
            put(b, "  discard;\n");
        else if (k->alphafunc <= 7)
            put(b, "  if (!(floor(cur.a * 255.0 + 0.5) %s u_aref)) discard;\n", cmp[k->alphafunc]);
    }
    put(b, "  o_color = cur;\n}\n");
}

static GLuint compile(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "[d3d8] shader compile failed:\n%s\n--- source ---\n%s\n", log, src);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

typedef struct {
    ffprog *p;
    const char *vs, *fs;
} link_args;

// программа и расположение униформ; prog = 0 — ошибка (рисование пропускается)
static void link_body(link_args *a)
{
    ffprog *p = a->p;
    GLint ok = 0;
#ifdef SNX_GLES
    // собранная в прошлый раз программа (glProgramBinary): без компиляции шейдеров драйвером
    p->prog = gles_program_load(a->vs, a->fs);
    ok = p->prog != 0;
#endif
    if (!ok) {
        GLuint v = compile(GL_VERTEX_SHADER, a->vs), f = compile(GL_FRAGMENT_SHADER, a->fs);
        if (!v || !f)
            return;
        p->prog = glCreateProgram();
        glAttachShader(p->prog, v);
        glAttachShader(p->prog, f);
#ifdef SNX_GLES
        gles_program_hint(p->prog);
#endif
        glLinkProgram(p->prog);
        glDeleteShader(v);
        glDeleteShader(f);
        glGetProgramiv(p->prog, GL_LINK_STATUS, &ok);
#ifdef SNX_GLES
        if (ok)
            gles_program_store(p->prog, a->vs, a->fs);
#endif
    }
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(p->prog, sizeof log, NULL, log);
        fprintf(stderr, "[d3d8] program link failed: %s\n", log);
        glDeleteProgram(p->prog);
        p->prog = 0;
        return;
    }
    glUseProgram(p->prog);
#define LOC(n) p->n = glGetUniformLocation(p->prog, #n)
    LOC(u_wv); LOC(u_proj); LOC(u_world); LOC(u_vp); LOC(u_depth); LOC(u_mat); LOC(u_power); LOC(u_ambient);
    LOC(u_ld); LOC(u_ls); LOC(u_la); LOC(u_lpos); LOC(u_ldir); LOC(u_latt); LOC(u_fog); LOC(u_fogcolor);
    LOC(u_clip); LOC(u_tfactor); LOC(u_aref); LOC(u_psc); LOC(u_bump);
#undef LOC
    p->u_texm = glGetUniformLocation(p->prog, "u_texm");
    for (int s = 0; s < MAX_STAGES; s++) {
        char name[16];
        snprintf(name, sizeof name, "u_tex%d", s);
        GLint l = glGetUniformLocation(p->prog, name);
        if (l >= 0)
            glUniform1i(l, s);
    }
}

static void link_program(void *arg)
{
    link_args *a = arg;
    u64 t0 = rt_ticks_ns();
    link_body(a);
    g_hitch.links++;  // строка [hitch] (d3d8_present)
    g_hitch.link_ns += rt_ticks_ns() - t0;
    atomic_store_explicit(&a->p->ready, 1, memory_order_release);
}

// Программы прошлых запусков (d3d8_prog_preload): исходники в куче, освобождает поток рендера
static void link_program_bg(void *arg)
{
    link_args *a = arg;
    link_program(a);
    free((void *)a->vs);
    free((void *)a->fs);
}

// Ключи программ между запусками: <user>/System/SnxPrograms.bin — заголовок и ключи подряд в порядке первого
// использования. Новая программа — сборка в потоке рендера, а гость ждёт её целиком (шейдеры Mesa на A57 — десятки
// мс: фризы на новых эффектах, взрывах грибов); при создании устройства программы из файла ставятся в очередь
// потока рендера заранее и собираются в фоне (меню, ролики), а не посреди игры. Ключи с пиксельным шейдером не
// пишутся: в ключе — его описатель, от запуска к запуску другой.
#define PROG_FILE "System/SnxPrograms.bin"
#define PROG_MAGIC 0x50584E53u  // "SNXP"
#define PROG_VERSION 1u
static char g_prog_path[1100];

static ffprog *prog_insert(const ffkey *k, u32 h, u32 i)
{
    if (g_nprogs >= PROG_CACHE * 3 / 4) {
        fprintf(stderr, "[d3d8] program cache full\n");
        return NULL;
    }
    ffprog *p = &g_progs[i];
    memset(p, 0, sizeof *p);
    p->used = 1;
    p->key = *k;
    p->hash = h;
    p->gen_xf = p->gen_light = p->gen_mat = p->gen_misc = 0xFFFFFFFFu;
    g_nprogs++;
    return p;
}

static ffprog *prog_find(const ffkey *k, u32 *h, u32 *slot)
{
    *h = fnv(k, sizeof *k);
    u32 i = *h & (PROG_CACHE - 1);
    while (g_progs[i].used) {
        if (g_progs[i].hash == *h && !memcmp(&g_progs[i].key, k, sizeof *k))
            return &g_progs[i];
        i = (i + 1) & (PROG_CACHE - 1);
    }
    *slot = i;
    return NULL;
}

void d3d8_prog_preload(void)
{
    char host[1100];
    u32 hdr[3] = {0}, n = 0;
    if (vfs_find(PROG_FILE, host, sizeof host)) {
        FILE *f = fopen(host, "rb");
        if (f && fread(hdr, 4, 3, f) == 3 && hdr[0] == PROG_MAGIC && hdr[1] == PROG_VERSION &&
            hdr[2] == sizeof(ffkey)) {
            ffkey k;
            while (fread(&k, sizeof k, 1, f) == 1) {
                u32 h, i;
                if (k.ps || prog_find(&k, &h, &i))
                    continue;
                ffprog *p = prog_insert(&k, h, i);
                if (!p)
                    break;
                sbuf vs = {0}, fs = {0};
                gen_vs(&k, &vs);
                gen_fs(&k, &fs);
                link_args a = {p, vs.p, fs.p};
                cs_async(link_program_bg, &a, sizeof a);
                n++;
            }
        } else if (f) {
            hdr[0] = 0;  // старый формат ключа — файл заново
        }
        if (f)
            fclose(f);
    }
    if (!vfs_for_write(PROG_FILE, g_prog_path, sizeof g_prog_path, 0)) {
        g_prog_path[0] = 0;
        return;
    }
    if (hdr[0] != PROG_MAGIC) {
        FILE *f = fopen(g_prog_path, "wb");
        u32 h[3] = {PROG_MAGIC, PROG_VERSION, (u32)sizeof(ffkey)};
        if (!f || fwrite(h, 4, 3, f) != 3)
            g_prog_path[0] = 0;
        if (f)
            fclose(f);
        vfs_changed();
    }
    fprintf(stderr, "[d3d8] %u programs from earlier runs queued for background build\n", n);
}

static ffprog *program_for(d3ddev *d, const ffkey *k)
{
    (void)d;
    u32 h, i;
    ffprog *p = prog_find(k, &h, &i);
    if (p) {
        if (!atomic_load_explicit(&p->ready, memory_order_acquire))
            cs_finish();  // ещё в фоновой очереди: дождаться (только сразу после запуска)
        return p;
    }
    p = prog_insert(k, h, i);
    if (!p)
        return NULL;
    sbuf vs = {0}, fs = {0};
    gen_vs(k, &vs);
    gen_fs(k, &fs);
    link_args a = {p, vs.p, fs.p};
    cs_sync(link_program, &a);  // компиляция и сборка — в потоке рендера, одним ожиданием
    free(vs.p);
    free(fs.p);
    if (g_prog_path[0] && !k->ps) {  // в следующий раз — в фоне при создании устройства
        FILE *f = fopen(g_prog_path, "ab");
        if (f) {
            fwrite(k, sizeof *k, 1, f);
            fclose(f);
        }
    }
    return p;
}

static void color4(float *o, u32 c)
{
    o[0] = (float)((c >> 16) & 0xFF) / 255.0f;
    o[1] = (float)((c >> 8) & 0xFF) / 255.0f;
    o[2] = (float)(c & 0xFF) / 255.0f;
    o[3] = (float)(c >> 24) / 255.0f;
}

static void upload_uniforms(d3ddev *d, ffprog *p)
{
    if (p->gen_xf != d->gen_xf || p->gen_misc != d->gen_misc) {
        float wv[16];
        mat_mul(wv, d->world, d->view);
        glUniformMatrix4fv(p->u_wv, 1, GL_FALSE, wv);
        glUniformMatrix4fv(p->u_proj, 1, GL_FALSE, d->proj);
        glUniformMatrix4fv(p->u_world, 1, GL_FALSE, d->world);
        if (p->u_texm >= 0)
            glUniformMatrix4fv(p->u_texm, MAX_STAGES, GL_FALSE, &d->texm[0][0]);
        glUniform4f(p->u_vp, (float)d->vp[0], (float)d->vp[1], (float)d->vp[2], (float)d->vp[3]);
        glUniform2f(p->u_depth, d->vpz[0], d->vpz[1]);
        if (p->u_clip >= 0)
            glUniform4fv(p->u_clip, 6, &d->clip[0][0]);
    }
    if (p->key.lighting && (p->gen_light != d->gen_light || p->gen_xf != d->gen_xf)) {
        float ld[MAX_LIGHTS][4], ls[MAX_LIGHTS][4], la[MAX_LIGHTS][4], lpos[MAX_LIGHTS][4], ldir[MAX_LIGHTS][4],
            latt[MAX_LIGHTS][4];
        const float *v = d->view;
        for (int i = 0; i < MAX_LIGHTS; i++) {
            const d3dlight *l = &d->lights[i];
            memcpy(ld[i], l->diffuse, 12);
            memcpy(ls[i], l->specular, 12);
            memcpy(la[i], l->ambient, 12);
            ld[i][3] = cosf(l->theta * 0.5f);
            ls[i][3] = cosf(l->phi * 0.5f);
            la[i][3] = 0.0f;
            for (int j = 0; j < 3; j++) {
                lpos[i][j] = l->pos[0] * v[j] + l->pos[1] * v[4 + j] + l->pos[2] * v[8 + j] + v[12 + j];
                ldir[i][j] = l->dir[0] * v[j] + l->dir[1] * v[4 + j] + l->dir[2] * v[8 + j];
            }
            float n = sqrtf(ldir[i][0] * ldir[i][0] + ldir[i][1] * ldir[i][1] + ldir[i][2] * ldir[i][2]);
            if (n > 0)
                for (int j = 0; j < 3; j++)
                    ldir[i][j] /= n;
            lpos[i][3] = l->range > 0 ? l->range : 1e30f;
            ldir[i][3] = l->falloff;
            latt[i][0] = l->att0;
            latt[i][1] = l->att1;
            latt[i][2] = l->att2;
            latt[i][3] = 0.0f;
        }
        glUniform4fv(p->u_ld, MAX_LIGHTS, &ld[0][0]);
        glUniform4fv(p->u_ls, MAX_LIGHTS, &ls[0][0]);
        glUniform4fv(p->u_la, MAX_LIGHTS, &la[0][0]);
        glUniform4fv(p->u_lpos, MAX_LIGHTS, &lpos[0][0]);
        glUniform4fv(p->u_ldir, MAX_LIGHTS, &ldir[0][0]);
        glUniform4fv(p->u_latt, MAX_LIGHTS, &latt[0][0]);
    }
    if (p->key.lighting && p->gen_mat != d->gen_mat) {
        glUniform4fv(p->u_mat, 4, d->material);
        glUniform1f(p->u_power, d->material[16]);
    }
    if (p->gen_misc != d->gen_misc) {
        float c[4];
        color4(c, d->rs[60]);
        glUniform4fv(p->u_tfactor, 1, c);
        color4(c, d->rs[34]);
        glUniform4fv(p->u_fogcolor, 1, c);
        color4(c, d->rs[139]);
        glUniform4fv(p->u_ambient, 1, c);
        float fs = u2f(d->rs[36]), fe = u2f(d->rs[37]);
        glUniform4f(p->u_fog, fs, fe, u2f(d->rs[38]), fe != fs ? 1.0f / (fe - fs) : 0.0f);
        glUniform1f(p->u_aref, (float)(d->rs[24] & 0xFF));
        if (p->u_psc >= 0)
            glUniform4fv(p->u_psc, 8, &d->psc[0][0]);
        if (p->u_bump >= 0) {
            float bm[MAX_STAGES][4];
            for (int s = 0; s < MAX_STAGES; s++)
                for (int j = 0; j < 4; j++)
                    bm[s][j] = u2f(d->tss[s][7 + j]);
            glUniform4fv(p->u_bump, MAX_STAGES, &bm[0][0]);
        }
    }
    p->gen_xf = d->gen_xf;
    p->gen_light = d->gen_light;
    p->gen_mat = d->gen_mat;
    p->gen_misc = d->gen_misc;
}

// ================================================================ состояние GL
static GLenum gl_blend(u32 b)
{
    static const GLenum t[14] = {GL_ONE, GL_ZERO, GL_ONE, GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR, GL_SRC_ALPHA,
                                 GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA, GL_DST_COLOR,
                                 GL_ONE_MINUS_DST_COLOR, GL_SRC_ALPHA_SATURATE, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA};
    return b < 14 ? t[b] : GL_ONE;
}

static GLenum gl_cmp(u32 f)
{
    static const GLenum t[9] = {GL_ALWAYS, GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER, GL_NOTEQUAL, GL_GEQUAL,
                                GL_ALWAYS};
    return f < 9 ? t[f] : GL_ALWAYS;
}

static GLenum gl_stencil_op(u32 o)
{
    static const GLenum t[9] = {GL_KEEP, GL_KEEP, GL_ZERO, GL_REPLACE, GL_INCR, GL_DECR, GL_INVERT, GL_INCR_WRAP,
                                GL_DECR_WRAP};
    return o < 9 ? t[o] : GL_KEEP;
}

static u32 g_applied_gen = 0xFFFFFFFFu;

static void apply_state(d3ddev *d)
{
    if (g_applied_gen == d->gen_misc)
        return;
    g_applied_gen = d->gen_misc;
    const u32 *rs = d->rs;
    glViewport((GLint)d->vp[0], (GLint)d->vp[1], (GLsizei)d->vp[2], (GLsizei)d->vp[3]);
    glDepthRange(d->vpz[0], d->vpz[1]);
    glDisable(GL_SCISSOR_TEST);
    if (rs[7] && d->ds) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(gl_cmp(rs[23]));
    } else
        glDisable(GL_DEPTH_TEST);
    glDepthMask(rs[14] && d->ds ? GL_TRUE : GL_FALSE);
    if (rs[27]) {
        glEnable(GL_BLEND);
        u32 src = rs[19], dst = rs[20];
        if (src == 12) {  // BOTHSRCALPHA
            src = 5;
            dst = 6;
        } else if (src == 13) {  // BOTHINVSRCALPHA
            src = 6;
            dst = 5;
        }
        glBlendFunc(gl_blend(src), gl_blend(dst));
        static const GLenum ops[6] = {GL_FUNC_ADD, GL_FUNC_ADD, GL_FUNC_SUBTRACT, GL_FUNC_REVERSE_SUBTRACT, GL_MIN, GL_MAX};
        glBlendEquation(rs[171] < 6 ? ops[rs[171]] : GL_FUNC_ADD);
    } else
        glDisable(GL_BLEND);
    // D3D «по часовой» = GL «против часовой» (кадр хранится сверху вниз)
    glFrontFace(GL_CCW);
    if (rs[22] == 2) {
        glEnable(GL_CULL_FACE);
        glCullFace(GL_FRONT);
    } else if (rs[22] == 3) {
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
    } else
        glDisable(GL_CULL_FACE);
    if (rs[52] && d->ds) {
        glEnable(GL_STENCIL_TEST);
        glStencilFunc(gl_cmp(rs[56]), (GLint)rs[57], rs[58]);
        glStencilOp(gl_stencil_op(rs[53]), gl_stencil_op(rs[54]), gl_stencil_op(rs[55]));
        glStencilMask(rs[59]);
    } else
        glDisable(GL_STENCIL_TEST);
    u32 cw = rs[168];
    glColorMask(cw & 1, (cw >> 1) & 1, (cw >> 2) & 1, (cw >> 3) & 1);
    glPolygonMode(GL_FRONT_AND_BACK, rs[8] == 1 ? GL_POINT : rs[8] == 2 ? GL_LINE : GL_FILL);
    if (rs[47]) {  // ZBIAS 0..16: больше — ближе
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(-1.0f, -(float)rs[47]);
    } else
        glDisable(GL_POLYGON_OFFSET_FILL);
    for (int i = 0; i < 6; i++) {
        if (rs[152] & (1u << i))
            glEnable(GL_CLIP_DISTANCE0 + i);
        else
            glDisable(GL_CLIP_DISTANCE0 + i);
    }
}

static GLint gl_address(u32 a)
{
    switch (a) {
    case 2: case 5: return GL_MIRRORED_REPEAT;
    case 3: return GL_CLAMP_TO_EDGE;
    case 4: return GL_CLAMP_TO_BORDER;
    default: return GL_REPEAT;
    }
}

// анизотропная фильтрация (GL_EXT_texture_filter_anisotropic) — проверяется один раз при создании устройства
static int g_aniso;

static void probe_caps(void *arg)
{
    d3ddev *d = arg;
    glGetError();
    glSamplerParameterf(d->samplers[0], GL_TEXTURE_MAX_ANISOTROPY_EXT, 1.0f);
    g_aniso = glGetError() == GL_NO_ERROR;
}

void d3d8_caps_probe(d3ddev *d) { cs_sync(probe_caps, d); }

static void update_sampler(d3ddev *d, int s)
{
    const u32 *t = d->tss[s];
    GLuint sm = d->samplers[s];
    glSamplerParameteri(sm, GL_TEXTURE_WRAP_S, gl_address(t[13]));
    glSamplerParameteri(sm, GL_TEXTURE_WRAP_T, gl_address(t[14]));
    glSamplerParameteri(sm, GL_TEXTURE_WRAP_R, gl_address(t[25]));
    float bc[4];
    color4(bc, t[15]);
    glSamplerParameterfv(sm, GL_TEXTURE_BORDER_COLOR, bc);
    u32 mag = t[16], min = t[17], mipf = t[18];
    glSamplerParameteri(sm, GL_TEXTURE_MAG_FILTER, mag == 1 ? GL_NEAREST : GL_LINEAR);
    GLint mf;
    int lin = min != 1;
    if (mipf == 0)
        mf = lin ? GL_LINEAR : GL_NEAREST;
    else if (mipf == 1)
        mf = lin ? GL_LINEAR_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_NEAREST;
    else
        mf = lin ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_LINEAR;
    glSamplerParameteri(sm, GL_TEXTURE_MIN_FILTER, mf);
    float aniso = (min == 3 || mag == 3) && t[21] > 1 ? (float)t[21] : 1.0f;
    if (g_aniso)
        glSamplerParameterf(sm, GL_TEXTURE_MAX_ANISOTROPY_EXT, aniso);
    glSamplerParameterf(sm, GL_TEXTURE_LOD_BIAS, u2f(t[19]));
    glSamplerParameterf(sm, GL_TEXTURE_MIN_LOD, (float)t[20]);
}

// ================================================================ вершины и отрисовка
static void vertex_count(u32 prim, u32 count, GLenum *mode, u32 *n)
{
    switch (prim) {
    case 1: *mode = GL_POINTS; *n = count; break;
    case 2: *mode = GL_LINES; *n = count * 2; break;
    case 3: *mode = GL_LINE_STRIP; *n = count + 1; break;
    case 5: *mode = GL_TRIANGLE_STRIP; *n = count + 2; break;
    case 6: *mode = GL_TRIANGLE_FAN; *n = count + 2; break;
    default: *mode = GL_TRIANGLES; *n = count * 3;
    }
}

static u32 g_attr_enabled;

// ES без glDrawElementsBaseVertex: базовая вершина — сдвигом указателей атрибутов (вершин от начала буфера)
static u32 g_attr_base;

static void setup_attribs(d3ddev *d, const vdecl *decl, const void *up, u32 up_stride, u32 up_bytes)
{
    u32 want = 0;
    if (up) {
        glBindBuffer(GL_ARRAY_BUFFER, d->up_vbo);
        glBufferData(GL_ARRAY_BUFFER, up_bytes, up, GL_STREAM_DRAW);
    }
    for (int i = 0; i < decl->n; i++) {
        const velem *e = &decl->e[i];
        if (e->reg >= 16)
            continue;
        u32 stride;
        if (up) {
            if (e->stream != 0)
                continue;
            stride = up_stride;
        } else {
            d3dbuf *b = d->stream[e->stream];
            if (!b)
                continue;
            d3d8_buf_sync(b);
            glBindBuffer(GL_ARRAY_BUFFER, b->gl);
            stride = d->stride[e->stream];
        }
        const void *off = (const void *)(uintptr_t)(e->offset + (up ? 0 : g_attr_base * stride));
        switch (e->type) {
        case 0: case 1: case 2: case 3:
            glVertexAttribPointer(e->reg, e->type + 1, GL_FLOAT, GL_FALSE, (GLsizei)stride, off);
            break;
        case 4:
            glVertexAttribPointer(e->reg, GL_BGRA, GL_UNSIGNED_BYTE, GL_TRUE, (GLsizei)stride, off);
            break;
        case 5:
            glVertexAttribPointer(e->reg, 4, GL_UNSIGNED_BYTE, GL_FALSE, (GLsizei)stride, off);
            break;
        case 6:
            glVertexAttribPointer(e->reg, 2, GL_SHORT, GL_FALSE, (GLsizei)stride, off);
            break;
        default:
            glVertexAttribPointer(e->reg, 4, GL_SHORT, GL_FALSE, (GLsizei)stride, off);
        }
        want |= 1u << e->reg;
    }
    for (u32 r = 0; r < 16; r++) {
        if ((want ^ g_attr_enabled) & (1u << r)) {
            if (want & (1u << r))
                glEnableVertexAttribArray(r);
            else
                glDisableVertexAttribArray(r);
        }
    }
    g_attr_enabled = want;
}

// SNX_DUMP_DRAWS=N: состояние каждой отрисовки кадра N (стадии, смешивание, матрицы текстур) — в stderr
int d3d8_dumping(const d3ddev *d)
{
    static int frame = -2;
    if (frame == -2) {
        const char *e = getenv("SNX_DUMP_DRAWS");
        frame = e ? atoi(e) : -1;
    }
    return d->frame == frame;
}

static void dump_draw(d3ddev *d, const ffkey *k, u32 prim, u32 count, int indexed)
{
    const u32 *rs = d->rs;
    fprintf(stderr, "[dump] draw %u: prim %u n %u %s rhw %u light %u | blend %u %u/%u z %u/%u/%u atest %u/%u/%u "
                    "cull %u clip %02x fog %u\n",
            d->draws, prim, count, indexed ? "idx" : "arr", k->rhw, k->lighting, rs[27], rs[19], rs[20], rs[7],
            rs[14], rs[23], rs[15], rs[25], rs[24], rs[22], k->clipmask, k->fog);
    for (int s = 0; s < k->nstages; s++) {
        const stagekey *sk = &k->st[s];
        const u32 *t = d->tss[s];
        d3dtex *tx = d->tex[s];
        fprintf(stderr, "[dump]   st%d: c %u(%x,%x) a %u(%x,%x) tex %ux%u fmt %x tci %u gen %u ttf %u%s addr %u/%u "
                        "border %08x\n",
                s, sk->cop, sk->ca1, sk->ca2, sk->aop, sk->aa1, sk->aa2, tx ? tx->w : 0, tx ? tx->h : 0,
                tx ? tx->fmt : 0, sk->tci, sk->tcgen, sk->ttf_count, sk->ttf_proj ? "P" : "", t[13], t[14], t[15]);
        if (sk->ttf_count) {
            const float *m = d->texm[s];
            fprintf(stderr, "[dump]     texm");
            for (int i = 0; i < 16; i++)
                fprintf(stderr, " %.3g", m[i]);
            fprintf(stderr, "\n");
        }
    }
}

void d3d8_draw(d3ddev *d, u32 prim, u32 count, int indexed, u32 first, u32 min_index, u32 num_vertices,
               const void *up_vertices, u32 up_stride, const void *up_indices, u32 up_index_fmt)
{
    if (!count)
        return;
    d->draws++;
    vdecl decl;
    if (!d3d8_vdecl_for(d, &decl)) {
        hle_todo("d3d8: draw with an unknown vertex format");
        return;
    }
    GLenum mode;
    u32 n;
    vertex_count(prim, count, &mode, &n);
    ffkey key;
    make_key(d, &decl, &key);
    if (d3d8_dumping(d))
        dump_draw(d, &key, prim, count, indexed);
    ffprog *p = program_for(d, &key);
    if (!p || !p->prog)
        return;
    apply_state(d);
    glUseProgram(p->prog);
    upload_uniforms(d, p);
    for (int s = 0; s < key.nstages; s++) {
        d3dtex *t = d->tex[s];
        if (!t)
            continue;
        glActiveTexture(GL_TEXTURE0 + (GLenum)s);
        glBindTexture(t->cube ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D, t->gl);
        if (d->sampler_dirty & (1u << s)) {
            update_sampler(d, s);
            d->sampler_dirty &= ~(1u << s);
        }
        glBindSampler((GLuint)s, d->samplers[s]);
    }
    glBindVertexArray(d->vao);
    if (!indexed) {
        if (up_vertices) {
            setup_attribs(d, &decl, up_vertices, up_stride, n * up_stride);
            glDrawArrays(mode, 0, (GLsizei)n);
        } else {
            setup_attribs(d, &decl, NULL, 0, 0);
            glDrawArrays(mode, (GLint)first, (GLsizei)n);
        }
    } else if (up_vertices) {
        u32 isize = up_index_fmt == FMT_INDEX32 ? 4 : 2;
        setup_attribs(d, &decl, up_vertices, up_stride, (min_index + num_vertices) * up_stride);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, d->up_ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, n * isize, up_indices, GL_STREAM_DRAW);
        glDrawElements(mode, (GLsizei)n, isize == 4 ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT, 0);
    } else {
        d3dbuf *ib = d->indices;
        if (!ib)
            return;
        u32 isize = ib->fmt == FMT_INDEX32 ? 4 : 2;
#ifdef SNX_GLES
        if (!gles_caps.base_vertex && d->base_vertex) {
            g_attr_base = d->base_vertex;
            setup_attribs(d, &decl, NULL, 0, 0);
            g_attr_base = 0;  // указатели атрибутов ставятся заново при каждой отрисовке
            d3d8_buf_sync(ib);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ib->gl);
            glDrawElements(mode, (GLsizei)n, isize == 4 ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT,
                           (const void *)(uintptr_t)(first * isize));
            return;
        }
#endif
        setup_attribs(d, &decl, NULL, 0, 0);
        d3d8_buf_sync(ib);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ib->gl);
        glDrawElementsBaseVertex(mode, (GLsizei)n, isize == 4 ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT,
                                 (const void *)(uintptr_t)(first * isize), (GLint)d->base_vertex);
    }
}

// ================================================================ снимки кадров (отладка)
// SNX_SHOT=N[,K]: снимок заднего буфера в кадре N (и далее каждые K кадров) -> shot_<кадр>.png в текущем каталоге
// или в SNX_SHOT_DIR. PNG без сжатия (deflate stored), чтобы не тащить zlib.
static u32 crc_tab[256];
static u32 png_crc(u32 c, const u8 *p, size_t n)
{
    if (!crc_tab[1])
        for (u32 i = 0; i < 256; i++) {
            u32 x = i;
            for (int k = 0; k < 8; k++)
                x = (x & 1) ? 0xEDB88320u ^ (x >> 1) : x >> 1;
            crc_tab[i] = x;
        }
    for (size_t i = 0; i < n; i++)
        c = crc_tab[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c;
}

static void be32(u8 *p, u32 v)
{
    p[0] = (u8)(v >> 24);
    p[1] = (u8)(v >> 16);
    p[2] = (u8)(v >> 8);
    p[3] = (u8)v;
}

static void png_chunk(FILE *f, const char *type, const u8 *data, u32 n)
{
    u8 hdr[8];
    be32(hdr, n);
    memcpy(hdr + 4, type, 4);
    fwrite(hdr, 1, 8, f);
    if (n)
        fwrite(data, 1, n, f);
    u32 c = png_crc(0xFFFFFFFFu, hdr + 4, 4);
    c = png_crc(c, data, n) ^ 0xFFFFFFFFu;
    be32(hdr, c);
    fwrite(hdr, 1, 4, f);
}

static void write_png(const char *path, const u8 *rgba, u32 w, u32 h)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    u8 ihdr[13];
    be32(ihdr, w);
    be32(ihdr + 4, h);
    ihdr[8] = 8;
    ihdr[9] = 2;  // RGB
    ihdr[10] = ihdr[11] = ihdr[12] = 0;
    png_chunk(f, "IHDR", ihdr, 13);
    size_t row = 1 + (size_t)w * 3, raw_n = row * h;
    u8 *raw = malloc(raw_n);
    for (u32 y = 0; y < h; y++) {
        raw[y * row] = 0;
        for (u32 x = 0; x < w; x++)
            memcpy(raw + y * row + 1 + x * 3, rgba + ((size_t)y * w + x) * 4, 3);
    }
    size_t nblk = (raw_n + 65534) / 65535, zn = 2 + raw_n + nblk * 5 + 4;
    u8 *z = malloc(zn), *q = z;
    *q++ = 0x78;
    *q++ = 0x01;
    u32 a = 1, b2 = 0;
    for (size_t off = 0; off < raw_n;) {
        u32 len = raw_n - off > 65535 ? 65535 : (u32)(raw_n - off);
        *q++ = off + len == raw_n;
        *q++ = (u8)len;
        *q++ = (u8)(len >> 8);
        *q++ = (u8)~len;
        *q++ = (u8)(~len >> 8);
        memcpy(q, raw + off, len);
        for (u32 i = 0; i < len; i++) {
            a = (a + raw[off + i]) % 65521;
            b2 = (b2 + a) % 65521;
        }
        q += len;
        off += len;
    }
    be32(q, b2 << 16 | a);
    q += 4;
    png_chunk(f, "IDAT", z, (u32)(q - z));
    png_chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(raw);
    free(z);
}

// кадр SNX_DUMP_DRAWS: содержимое текущей цели рендера (из её FBO) -> <SNX_SHOT_DIR>/dump_<n>.png
void d3d8_dump_target(d3ddev *d)
{
    static int n;
    if (!d->rt)
        return;
    u32 w = d->rt->w, h = d->rt->h;
    u8 *px = malloc((size_t)w * h * 4);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, d->fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glReadPixels(0, 0, (GLsizei)w, (GLsizei)h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glBindFramebuffer(GL_FRAMEBUFFER, d->fbo);
    char path[512];
    const char *dir = getenv("SNX_SHOT_DIR");
    snprintf(path, sizeof path, "%s%sdump_%02d.png", dir ? dir : "", dir ? "/" : "", n++);
    write_png(path, px, w, h);
    free(px);
    fprintf(stderr, "[dump] target %ux%u -> %s\n", w, h, path);
}

static void maybe_shot(d3ddev *d)
{
    static int first = -2, step;
    if (first == -2) {
        const char *e = getenv("SNX_SHOT");
        first = e ? atoi(e) : -1;
        const char *c = e ? strchr(e, ',') : NULL;
        step = c ? atoi(c + 1) : 0;
    }
    if (first < 0 || d->frame < first || (d->frame != first && (!step || (d->frame - first) % step)))
        return;
    u8 *px = malloc((size_t)d->bb_w * d->bb_h * 4);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, d->blit_fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glReadPixels(0, 0, (GLsizei)d->bb_w, (GLsizei)d->bb_h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    char path[512];
    const char *dir = getenv("SNX_SHOT_DIR");
    snprintf(path, sizeof path, "%s%sshot_%05d.png", dir ? dir : "", dir ? "/" : "", d->frame);
    write_png(path, px, d->bb_w, d->bb_h);  // кадр хранится сверху вниз, как и строки PNG
    free(px);
    fprintf(stderr, "[d3d8] frame %d: %u draws -> %s\n", d->frame, d->draws, path);
}

// ================================================================ Present
// Ограничение частоты кадров (SNX_FPS_CAP=30 и т. п.; лаунчер Android — «Ограничение FPS»: меньше нагрев и
// троттлинг телефона; если частота экрана равна ему — только vsync, platform/window.c plat_set_display) и подсказки производительности Android (ADPF): длительность работы кадра потоком гостя —
// без ожиданий потока рендера и ограничителя.
static void frame_pace(void)
{
    static int cap = -1;
    static u64 next, last;
#ifdef __ANDROID__
    static u64 last_wait;
#endif
    if (cap < 0) {
        const char *e = getenv("SNX_FPS_CAP");
        cap = e ? atoi(e) : 0;
        cap = cap < 0 ? 0 : cap;
#ifdef __ANDROID__
        android_perf_target(cap > 0 ? 1000000000ull / (u64)cap : 16666667ull);
#endif
    }
    u64 now = rt_ticks_ns();
#ifdef __ANDROID__
    u64 wait = cs_wait_total_ns();
    if (last) {
        u64 dt = now - last, idle = wait - last_wait;
        android_perf_frame(dt > idle ? dt - idle : 0);
    }
    last_wait = wait;
#endif
    if (cap > 0 && !plat_vsync_paced()) {  // частота экрана равна ограничению — кадры отмеряет vsync
        u64 period = 1000000000ull / (u64)cap;
        next = next && now < next + period ? next + period : now + period;
        if (next > now) {
            struct timespec ts = {(time_t)((next - now) / 1000000000ull), (long)((next - now) % 1000000000ull)};
            nanosleep(&ts, NULL);
        }
    }
    last = rt_ticks_ns();
}

// счётчик кадров: семисегментные цифры прямоугольниками glClear с ножницами (без шейдеров и текстур)
static _Atomic int g_fps_overlay;
void d3d8_set_fps_overlay(int on) { g_fps_overlay = on; }
int d3d8_fps_overlay(void) { return g_fps_overlay; }

static void fill(int x, int y, int w, int h)
{
    glScissor(x, y, w, h);
    glClear(GL_COLOR_BUFFER_BIT);
}

// число n в левом верхнем углу окна w x h (у GL начало — внизу)
static void draw_fps(int n, int w, int h)
{
    static const u8 seg[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};  // биты: a b c d e f g
    char s[8];
    int len = snprintf(s, sizeof s, "%d", n < 0 ? 0 : n > 999 ? 999 : n);
    int dh = h / 24, dw = dh * 11 / 20, t = dh / 7 > 1 ? dh / 7 : 1, gap = dh / 3, m = h / 60;
    int x0 = m + gap / 2, top = h - m - gap / 2;
    glEnable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 1);
    fill(m, top - dh - gap / 2, len * (dw + gap) + gap / 2, dh + gap);
    glClearColor(1.0f, 0.85f, 0.1f, 1);
    for (int i = 0; i < len; i++) {
        int x = x0 + i * (dw + gap), b = seg[s[i] - '0'], mid = top - dh / 2 - t / 2;
        if (b & 0x01) fill(x, top - t, dw, t);                    // a
        if (b & 0x02) fill(x + dw - t, mid, t, top - mid);        // b
        if (b & 0x04) fill(x + dw - t, top - dh, t, mid - (top - dh) + t);  // c
        if (b & 0x08) fill(x, top - dh, dw, t);                   // d
        if (b & 0x10) fill(x, top - dh, t, mid - (top - dh) + t); // e
        if (b & 0x20) fill(x, mid, t, top - mid);                 // f
        if (b & 0x40) fill(x, mid, dw, t);                        // g
    }
    glDisable(GL_SCISSOR_TEST);
}

// Долгий кадр (дольше 40 мс и в полтора раза дольше обычного): что в нём было — сборка программ GL, чтения файлов
// с диска, заливка текстур, ожидание гостем потока рендера (фризы на новых эффектах и в отдельных местах карты)
static void hitch_check(d3ddev *d)
{
    typedef struct {
        unsigned long long reads, read_bytes, read_ns, links, link_ns, uploads, upload_bytes, wait_ns;
    } snap;
    static snap prev;
    static u64 t_prev, usual;
    snap cur = {g_hitch.reads, g_hitch.read_bytes, g_hitch.read_ns, g_hitch.links, g_hitch.link_ns,
                g_hitch.uploads, g_hitch.upload_bytes, cs_wait_total_ns()};
    u64 now = rt_ticks_ns(), dt = t_prev ? now - t_prev : 0;
    t_prev = now;
    if (dt && usual && dt > 40000000ull && dt * 2 > usual * 3)
        fprintf(stderr, "[hitch] frame %d: %.0f ms (usual %.0f), %u draws: programs %llu (%.0f ms), disk reads %llu "
                        "(%.0f KB, %.0f ms), texture uploads %llu (%.0f KB), waited for render %.0f ms\n",
                d->frame, (double)dt / 1e6, (double)usual / 1e6, d->draws, cur.links - prev.links,
                (double)(cur.link_ns - prev.link_ns) / 1e6, cur.reads - prev.reads,
                (double)(cur.read_bytes - prev.read_bytes) / 1024, (double)(cur.read_ns - prev.read_ns) / 1e6,
                cur.uploads - prev.uploads, (double)(cur.upload_bytes - prev.upload_bytes) / 1024,
                (double)(cur.wait_ns - prev.wait_ns) / 1e6);
    else if (dt)
        usual = usual ? (usual * 15 + dt) / 16 : dt;  // среднее без самих долгих кадров
    prev = cur;
}

// Ролик, кадр которого игра не вывела в задний буфер (больше кадра: 800x600 в 1200x540 и т. п. — D3DDrv тогда
// кадр не копирует, остаются звук и чёрный экран): порт рисует текущий кадр ролика сам, вписанным в задний буфер с
// чёрными полями, как write_movie (d3d8_res.c). Если игра сама выводит кадры ролика (write_movie за последние
// 250 мс) — ничего.
static void movie_overlay(d3ddev *d)
{
    const u8 *px;
    u32 mw, mh;
    if (rt_ticks_ns() - g_movie_drawn_ns < 250000000ull || !d->backbuffer || !d->backbuffer->rb ||
        !bink_movie_overlay(&px, &mw, &mh))
        return;
    static GLuint tex;
    static u32 tw, th;
    static int logged;
    if (!logged++)
        fprintf(stderr, "[bink] movie %ux%u is not in the %ux%u frame: drawn by the port\n", mw, mh, d->bb_w, d->bb_h);
    if (!tex || tw != mw || th != mh) {
        if (!tex)
            glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        cs_TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)mw, (GLsizei)mh, GL_RGBA, GL_UNSIGNED_BYTE, NULL, 0);
        tw = mw;
        th = mh;
    } else
        glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    cs_TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)mw, (GLsizei)mh, GL_RGBA, GL_UNSIGNED_BYTE, px, (size_t)mw * 4,
                     (size_t)mw * 4);
    glBindTexture(GL_TEXTURE_2D, 0);
    GLuint fb[2];
    glGenFramebuffers(2, fb);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fb[0]);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fb[1]);
    glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, d->backbuffer->rb);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    GLint W = (GLint)d->bb_w, H = (GLint)d->bb_h;
    float sc = fminf((float)W / (float)mw, (float)H / (float)mh);
    GLint w = (GLint)((float)mw * sc), h = (GLint)((float)mh * sc);
    glBlitFramebuffer(0, 0, (GLint)mw, (GLint)mh, (W - w) / 2, (H - h) / 2, (W - w) / 2 + w, (H - h) / 2 + h,
                      GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(2, fb);
    d3d8_bind_target(d);
    g_movie_overlay_ns = rt_ticks_ns();
}

void d3d8_present(d3ddev *d)
{
    game_menu_frame(d->bb_w, d->bb_h);  // состояние меню игры — для ввода хоста
    script_fix_scan(1);                 // запасной поиск неисправленного байткода (runtime/script_fix.c)
    movie_overlay(d);
    int w, h;
    plat_drawable_size(&w, &h);
    if (!d->blit_fbo)
        glGenFramebuffers(1, &d->blit_fbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, d->blit_fbo);
    glFramebufferRenderbuffer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, d->backbuffer->rb);
    ov_flush(d->blit_fbo, (int)d->bb_w, (int)d->bb_h);  // слой интерфейса порта (game_menu_frame) — в задний буфер
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    // вписать с сохранением пропорций; кадр хранится сверху вниз — переворот при копировании
    float sx = (float)w / (float)d->bb_w, sy = (float)h / (float)d->bb_h, sc = sx < sy ? sx : sy;
    int dw = (int)((float)d->bb_w * sc), dh = (int)((float)d->bb_h * sc), dx = (w - dw) / 2, dy = (h - dh) / 2;
    glBlitFramebuffer(0, 0, (GLint)d->bb_w, (GLint)d->bb_h, dx, dy + dh, dx + dw, dy, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    maybe_shot(d);
    // счётчик на экране: кадров за последние полсекунды
    static u64 ov_t0;
    static int ov_frame0, ov_fps;
    u64 ov_now = rt_ticks_ns();
    if (!ov_t0 || ov_now - ov_t0 >= 500000000ull) {
        if (ov_t0)
            ov_fps = (int)((double)(d->frame - ov_frame0) * 1e9 / (double)(ov_now - ov_t0) + 0.5);
        ov_t0 = ov_now;
        ov_frame0 = d->frame;
    }
    if (g_fps_overlay)
        draw_fps(ov_fps, w, h);
    hitch_check(d);
    if (hle_frame_hook)
        hle_frame_hook(d->frame);
    if (d->frame < 5 || d->frame % 600 == 0)
        fprintf(stderr, "[d3d8] frame %d: %u draws, %d programs\n", d->frame, d->draws, g_nprogs);
    // частота кадров раз в 5 с (на Switch — в терминал nxlink)
    static u64 fps_t0;
    static int fps_frame0;
    u64 now = rt_ticks_ns();
    if (!fps_t0 || now - fps_t0 >= 5000000000ull) {
        cs_stats st;
        cs_stats_take(&st);
        if (fps_t0) {
            double dt = (double)(now - fps_t0), frames = d->frame - fps_frame0 ? d->frame - fps_frame0 : 1;
            fprintf(stderr, "[d3d8] %.1f fps (%u draws in the last frame)\n", frames * 1e9 / dt, d->draws);
#ifdef __APPLE__
            // SNX_PERF=1: инструкции и такты процесса на кадр (на маке без вентилятора частота плавает — для
            // сравнения сборок это надёжнее кадров в секунду)
            static int perf = -1;
            static u64 ins0, cyc0;
            if (perf < 0)
                perf = getenv("SNX_PERF") != NULL;
            struct rusage_info_v4 ri;
            if (perf && proc_pid_rusage(getpid(), RUSAGE_INFO_V4, (rusage_info_t *)&ri) == 0) {
                if (ins0)
                    fprintf(stderr, "[perf] %.2f M instructions, %.2f M cycles per frame\n",
                            (double)(ri.ri_instructions - ins0) / frames / 1e6, (double)(ri.ri_cycles - cyc0) / frames / 1e6);
                ins0 = ri.ri_instructions;
                cyc0 = ri.ri_cycles;
                // сборка с SNX_TRACE: входов в функции гостя на кадр (rt_trace_pos считает ENTER)
                extern u32 rt_trace_pos;
                static u32 ent0;
                if (rt_trace_pos != ent0 && ent0)
                    fprintf(stderr, "[perf] %.0f K guest function entries per frame\n",
                            (double)(u32)(rt_trace_pos - ent0) / frames / 1e3);
                ent0 = rt_trace_pos;
            }
#endif
            // поток рендера: команд/КБ/ожиданий cs_sync на кадр; доля времени: гость ждёт, рендер занят, из них показ
            if (cs_enabled())
                fprintf(stderr, "[cs] %.0f cmds %.0f KB %.1f syncs per frame; guest waits %.0f%%, render busy %.0f%% "
                                "(swap %.0f%%)\n",
                        (double)st.recs / frames, (double)st.bytes / 1024 / frames, (double)st.syncs / frames,
                        100 * (double)st.wait_ns / dt, 100 * (double)st.busy_ns / dt, 100 * (double)st.swap_ns / dt);
        }
        fps_t0 = now;
        fps_frame0 = d->frame;
    }
    d->draws = 0;
    cs_swap();
    frame_pace();
    glBindFramebuffer(GL_FRAMEBUFFER, d->fbo);
    g_applied_gen = 0xFFFFFFFFu;
    d->frame++;
    // glGetError ждёт драйвер (с glthread/потоком рендера — всю очередь): не каждый кадр
    if (d->frame < 5 || d->frame % 300 == 0) {
        GLenum err = glGetError();
        if (err)
            fprintf(stderr, "[d3d8] GL error 0x%x by frame %d\n", err, d->frame);
    }
}

int d3d8_window_to_target(float u, float v, int *x, int *y)
{
    d3ddev *d = d3d8_dev();
    int w, h;
    plat_drawable_size(&w, &h);
    if (!d || !w || !h)
        return 0;
    float sx = (float)w / (float)d->bb_w, sy = (float)h / (float)d->bb_h, sc = sx < sy ? sx : sy;
    float dx = ((float)w - (float)d->bb_w * sc) / 2, dy = ((float)h - (float)d->bb_h * sc) / 2;
    *x = (int)((u * (float)w - dx) / sc);
    *y = (int)((v * (float)h - dy) / sc);
    return 1;
}

int d3d8_target_to_window(float x, float y, float *u, float *v)
{
    d3ddev *d = d3d8_dev();
    int w, h;
    plat_drawable_size(&w, &h);
    if (!d || !w || !h)
        return 0;
    float sx = (float)w / (float)d->bb_w, sy = (float)h / (float)d->bb_h, sc = sx < sy ? sx : sy;
    float dx = ((float)w - (float)d->bb_w * sc) / 2, dy = ((float)h - (float)d->bb_h * sc) / 2;
    *u = (x * sc + dx) / (float)w;
    *v = (y * sc + dy) / (float)h;
    return 1;
}
