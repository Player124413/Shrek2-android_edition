// HLE Direct3D 8: пиксельные шейдеры ps.1.1–1.4 (байткод) -> тело GLSL. D3DDrv собирает свои шейдеры
// (смешивание слоёв террейна) встроенным ассемблером D3DX8; нужное подмножество: def, tex/texld,
// texcoord/texcrd, mov, add, sub, mul, mad, dp3, dp4, lrp, cnd, cmp, модификаторы и маски.
// Интерфейс тела (d3d8_ff.c): входы v0, v1, tc0..tc5, сэмплеры u_tex0..5, константы u_psc[8]; результат — cur.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "runtime/hle/d3d8_int.h"

#define MAX_PS 64
static char *g_ps[MAX_PS];

typedef struct {
    char *p;
    size_t n, cap;
} sb;

static void out(sb *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void out(sb *b, const char *fmt, ...)
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
        b->cap = b->cap ? b->cap * 2 : 4096;
        b->p = realloc(b->p, b->cap);
    }
}

enum { R_TEMP = 0, R_INPUT = 1, R_CONST = 2, R_TEXTURE = 3 };

static int g_ver;  // 0x101..0x104

static void reg_name(u32 tok, char *o, size_t cap)
{
    u32 type = (tok >> 28) & 7, n = tok & 0x7FF;
    switch (type) {
    case R_TEMP: snprintf(o, cap, "r%u", n); break;
    case R_INPUT: snprintf(o, cap, "v%u", n); break;
    case R_CONST: snprintf(o, cap, "c%u", n); break;
    case R_TEXTURE: snprintf(o, cap, g_ver >= 0x104 ? "tc%u" : "t%u", n); break;
    default: snprintf(o, cap, "r0");
    }
}

static void src_expr(u32 tok, char *o, size_t cap)
{
    char r[16], v[64];
    reg_name(tok, r, sizeof r);
    u32 sw = (tok >> 16) & 0xFF;
    if (sw == 0xE4)
        snprintf(v, sizeof v, "%s", r);
    else
        snprintf(v, sizeof v, "%s.%c%c%c%c", r, "xyzw"[sw & 3], "xyzw"[(sw >> 2) & 3], "xyzw"[(sw >> 4) & 3],
                 "xyzw"[(sw >> 6) & 3]);
    switch ((tok >> 24) & 0xF) {
    case 1: snprintf(o, cap, "(-%s)", v); break;
    case 2: snprintf(o, cap, "(%s - 0.5)", v); break;
    case 3: snprintf(o, cap, "(0.5 - %s)", v); break;
    case 4: snprintf(o, cap, "(2.0 * (%s - 0.5))", v); break;
    case 5: snprintf(o, cap, "(-2.0 * (%s - 0.5))", v); break;
    case 6: snprintf(o, cap, "(1.0 - %s)", v); break;
    case 7: snprintf(o, cap, "(2.0 * %s)", v); break;
    case 8: snprintf(o, cap, "(-2.0 * %s)", v); break;
    case 11: snprintf(o, cap, "abs(%s)", v); break;
    default: snprintf(o, cap, "%s", v);
    }
}

// запись результата: сдвиг (_x2/_x4/_x8/_d2/_d4/_d8), насыщение, маска
static void dst_write(sb *b, u32 tok, const char *expr)
{
    char r[16], mask[8] = ".", e[256];
    reg_name(tok, r, sizeof r);
    u32 wm = (tok >> 16) & 0xF, mod = (tok >> 20) & 0xF;
    int shift = (int)((tok >> 24) & 0xF);
    if (shift >= 8)
        shift -= 16;
    float scale = shift >= 0 ? (float)(1 << shift) : 1.0f / (float)(1 << -shift);
    if (shift)
        snprintf(e, sizeof e, "(%s) * %#g", expr, scale);  // с точкой: в GLSL ES нет неявного int -> float
    else
        snprintf(e, sizeof e, "%s", expr);
    const char *lo = (mod & 1) ? "0.0" : "-8.0", *hi = (mod & 1) ? "1.0" : "8.0";
    if (wm == 0xF || wm == 0)
        out(b, "  %s = clamp(%s, %s, %s);\n", r, e, lo, hi);
    else {
        int k = 1;
        for (int i = 0; i < 4; i++)
            if (wm & (1u << i))
                mask[k++] = "xyzw"[i];
        mask[k] = 0;
        out(b, "  %s%s = clamp(vec4(%s)%s, %s, %s);\n", r, mask, e, mask, lo, hi);
    }
}

u32 d3d8_ps_create(const u32 *t, u32 max)
{
    if (!max || (t[0] & 0xFFFF0000u) != 0xFFFF0000u) {
        fprintf(stderr, "[d3d8] pixel shader: bad version token %08x\n", max ? t[0] : 0);
        return 0;
    }
    g_ver = (int)(t[0] & 0xFFFF);
    sb b = {0};
    out(&b, "  // ps.%d.%d\n", g_ver >> 8, g_ver & 0xFF);
    for (int i = 0; i < 6; i++)
        out(&b, "  vec4 r%d = vec4(0.0), t%d = vec4(0.0);\n", i, i);
    for (int i = 0; i < 8; i++)
        out(&b, "  vec4 c%d = u_psc[%d];\n", i, i);
    for (u32 i = 1; i < max;) {
        u32 tok = t[i], op = tok & 0xFFFF;
        if (tok == 0x0000FFFFu)
            break;
        if (op == 0xFFFE) {  // комментарий
            i += 1 + ((tok >> 16) & 0x7FFF);
            continue;
        }
        if (op == 0xFFFD || op == 0) {  // phase, nop
            i++;
            continue;
        }
        const u32 *a = &t[i + 1];
        char s0[96], s1[96], s2[96], e[512];
        int n;  // операндов (с назначением)
        switch (op) {
        case 81:  // def c#, x, y, z, w
        {
            float f[4];
            memcpy(f, a + 1, 16);
            char r[16];
            reg_name(a[0], r, sizeof r);
            out(&b, "  %s = vec4(%g, %g, %g, %g);\n", r, f[0], f[1], f[2], f[3]);
            i += 6;
            continue;
        }
        case 66:  // tex
            if (g_ver >= 0x104) {  // texld rN, tM|rM: сэмплер N
                char src[16];
                reg_name(a[1], src, sizeof src);
                snprintf(e, sizeof e, "texture(u_tex%u, %s.xy)", a[0] & 0x7FF, src);
                dst_write(&b, a[0], e);
                n = 2;
            } else {
                snprintf(e, sizeof e, "texture(u_tex%u, tc%u.xy)", a[0] & 0x7FF, a[0] & 0x7FF);
                dst_write(&b, a[0], e);
                n = 1;
            }
            break;
        case 64:  // texcoord / texcrd
            if (g_ver >= 0x104) {
                src_expr(a[1], s0, sizeof s0);
                dst_write(&b, a[0], s0);
                n = 2;
            } else {
                snprintf(e, sizeof e, "clamp(tc%u, 0.0, 1.0)", a[0] & 0x7FF);
                dst_write(&b, a[0], e);
                n = 1;
            }
            break;
        case 65:  // texkill
        {
            char r[16];
            reg_name(a[0], r, sizeof r);
            out(&b, "  if (any(lessThan(%s.xyz, vec3(0.0)))) discard;\n", r);
            n = 1;
            break;
        }
        case 1:
            src_expr(a[1], s0, sizeof s0);
            dst_write(&b, a[0], s0);
            n = 2;
            break;
        case 2: case 3: case 5: case 8: case 9:
            src_expr(a[1], s0, sizeof s0);
            src_expr(a[2], s1, sizeof s1);
            if (op == 2)
                snprintf(e, sizeof e, "%s + %s", s0, s1);
            else if (op == 3)
                snprintf(e, sizeof e, "%s - %s", s0, s1);
            else if (op == 5)
                snprintf(e, sizeof e, "%s * %s", s0, s1);
            else if (op == 8)
                snprintf(e, sizeof e, "vec4(dot(vec3(%s), vec3(%s)))", s0, s1);
            else
                snprintf(e, sizeof e, "vec4(dot(vec4(%s), vec4(%s)))", s0, s1);
            dst_write(&b, a[0], e);
            n = 3;
            break;
        case 4: case 18: case 80: case 88:
            src_expr(a[1], s0, sizeof s0);
            src_expr(a[2], s1, sizeof s1);
            src_expr(a[3], s2, sizeof s2);
            if (op == 4)
                snprintf(e, sizeof e, "%s * %s + %s", s0, s1, s2);
            else if (op == 18)
                snprintf(e, sizeof e, "mix(%s, %s, %s)", s2, s1, s0);
            else if (op == 80 && g_ver >= 0x104)  // cnd: src0 > 0.5 ? src1 : src2 (ps.1.4 — покомпонентно)
                snprintf(e, sizeof e, "mix(%s, %s, vec4(greaterThan(vec4(%s), vec4(0.5))))", s2, s1, s0);
            else if (op == 80)  // ps.1.1–1.3: по альфе src0
                snprintf(e, sizeof e, "(vec4(%s).a > 0.5 ? vec4(%s) : vec4(%s))", s0, s1, s2);
            else  // cmp: src0 >= 0 ? src1 : src2
                snprintf(e, sizeof e, "mix(%s, %s, vec4(greaterThanEqual(vec4(%s), vec4(0.0))))", s2, s1, s0);
            dst_write(&b, a[0], e);
            n = 4;
            break;
        default:
            fprintf(stderr, "[d3d8] pixel shader ps.%d.%d: opcode %u not supported\n", g_ver >> 8, g_ver & 0xFF, op);
            free(b.p);
            return 0;
        }
        i += 1 + (u32)n;
    }
    out(&b, "  cur = r0;\n");
    for (u32 h = 0; h < MAX_PS; h++)
        if (!g_ps[h]) {
            g_ps[h] = b.p;
            return h + 1;
        }
    free(b.p);
    return 0;
}

void d3d8_ps_delete(u32 h)
{
    if (h && h <= MAX_PS) {
        free(g_ps[h - 1]);
        g_ps[h - 1] = NULL;
    }
}

const char *d3d8_ps_glsl(u32 h) { return h && h <= MAX_PS ? g_ps[h - 1] : NULL; }
