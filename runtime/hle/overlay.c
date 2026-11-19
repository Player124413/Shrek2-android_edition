// Слой интерфейса порта поверх кадра (см. overlay.h). Атлас R8 1024x1024: вверху — формы значков (круг,
// кольцо, «таблетка», крестовина; сглаживание по расстоянию до края), ниже — глифы ASCII шрифта кеглем 48.
// Значок кнопки — светлая форма с тёмной надписью поверх, как на экранах Switch.
#include "runtime/hle/overlay.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#ifdef __SWITCH__
#include <switch.h>
#endif

#include "platform/gl.h"
#include "runtime/hle/gl_cs.h"

#define ATLAS 1024
#define FONT_PX 48
#define MAX_VERTS (6 * 4096)

typedef struct {
    float u0, v0, u1, v1;  // в атласе (доли)
    float w, h, bx, by;    // размер глифа, смещение от пера вправо и от базовой линии вверх (пиксели кегля 48)
    float adv;
} ov_glyph;

typedef struct {
    float x, y, u, v;
    u32 c;
} ov_vert;

enum { SH_WHITE, SH_DISC, SH_RING, SH_PILL, SH_CROSS, SH_N };
static const int g_sh_rect[SH_N][4] = {  // x, y, w, h в атласе
    {0, 0, 4, 4}, {8, 0, 96, 96}, {112, 0, 96, 96}, {216, 0, 176, 96}, {400, 0, 96, 96},
};

static u8 *g_atlas;
static int g_state;  // 0 — не готов, 1 — атлас есть, -1 — не вышло
static int g_have_font;
static ov_glyph g_gl[128];
static float g_ascent, g_cap;  // высота над базовой линией строки и заглавных (пиксели кегля 48)
static ov_vert g_v[MAX_VERTS];
static int g_nv;

// ---------------------------------------------------------------- атлас
static float clamp01(float v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; }

// форма по расстоянию до края d (пиксели, < 0 — внутри)
static void shape(int id)
{
    const int *r = g_sh_rect[id];
    for (int y = 0; y < r[3]; y++)
        for (int x = 0; x < r[2]; x++) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f, d = -1.0f;
            float cx = (float)r[2] / 2, cy = (float)r[3] / 2, R = (float)r[3] / 2 - 2;
            switch (id) {
            case SH_DISC: d = hypotf(px - cx, py - cy) - R; break;
            case SH_RING: d = fabsf(hypotf(px - cx, py - cy) - (R - 5)) - 5; break;
            case SH_PILL: {  // прямоугольник со скруглением на всю высоту
                float hx = (float)r[2] / 2 - 2 - R, qx = fabsf(px - cx) - hx;
                d = (qx > 0 ? hypotf(qx, py - cy) : fabsf(py - cy)) - R;
                break;
            }
            case SH_CROSS: {  // два скруглённых бруска
                float a = R, b = R * 0.36f, rr = b * 0.5f;
                float dx1 = fabsf(px - cx) - (a - rr), dy1 = fabsf(py - cy) - (b - rr);
                float dx2 = fabsf(px - cx) - (b - rr), dy2 = fabsf(py - cy) - (a - rr);
                float d1 = hypotf(fmaxf(dx1, 0), fmaxf(dy1, 0)) + fminf(fmaxf(dx1, dy1), 0) - rr;
                float d2 = hypotf(fmaxf(dx2, 0), fmaxf(dy2, 0)) + fminf(fmaxf(dx2, dy2), 0) - rr;
                d = fminf(d1, d2);
                break;
            }
            default: break;
            }
            g_atlas[(r[1] + y) * ATLAS + r[0] + x] = (u8)(clamp01(0.5f - d) * 255.0f + 0.5f);
        }
}

static FT_Face open_font(FT_Library ft)
{
    FT_Face face = NULL;
    const char *env = getenv("SNX_FONT");
    if (env && !FT_New_Face(ft, env, 0, &face))
        return face;
#ifdef __SWITCH__
    PlFontData fd;
    if (R_SUCCEEDED(plInitialize(PlServiceType_User)) &&
        R_SUCCEEDED(plGetSharedFontByType(&fd, PlSharedFontType_Standard)) &&
        !FT_New_Memory_Face(ft, (const FT_Byte *)fd.address, (FT_Long)fd.size, 0, &face))
        return face;
#elif defined(__ANDROID__)
    // системные шрифты Android разных лет: Roboto (5+, на 12+ — переменный и RobotoStatic), Droid Sans (старые)
    static const char *const paths[] = {"/system/fonts/Roboto-Bold.ttf", "/system/fonts/RobotoStatic-Regular.ttf",
                                        "/system/fonts/Roboto-Regular.ttf", "/system/fonts/DroidSans-Bold.ttf",
                                        "/system/fonts/DroidSans.ttf", "/system/fonts/NotoSans-Regular.ttf"};
    for (size_t i = 0; i < sizeof paths / sizeof paths[0]; i++)
        if (!FT_New_Face(ft, paths[i], 0, &face))
            return face;
#else
    static const char *const paths[] = {"/System/Library/Fonts/Supplemental/Arial Rounded Bold.ttf",
                                        "/System/Library/Fonts/Supplemental/Arial Bold.ttf",
                                        "/System/Library/Fonts/Helvetica.ttc"};
    for (size_t i = 0; i < sizeof paths / sizeof paths[0]; i++)
        if (!FT_New_Face(ft, paths[i], 0, &face))
            return face;
#endif
    return NULL;
}

static void build_atlas(void)
{
    g_atlas = calloc(ATLAS, ATLAS);
    if (!g_atlas) {
        g_state = -1;
        return;
    }
    for (int i = 0; i < 4 * 4; i++)
        g_atlas[(i / 4) * ATLAS + i % 4] = 255;
    for (int s = SH_DISC; s < SH_N; s++)
        shape(s);
    FT_Library ft;
    FT_Face face = NULL;
    if (!FT_Init_FreeType(&ft) && (face = open_font(ft)) && !FT_Set_Pixel_Sizes(face, 0, FONT_PX)) {
        g_ascent = (float)(face->size->metrics.ascender >> 6);
        int px = 0, py = 104, row = 0;
        for (int ch = 32; ch < 127; ch++) {
            if (FT_Load_Char(face, (FT_ULong)ch, FT_LOAD_RENDER))
                continue;
            FT_GlyphSlot g = face->glyph;
            int w = (int)g->bitmap.width, h = (int)g->bitmap.rows;
            if (px + w + 2 > ATLAS) {
                px = 0;
                py += row + 2;
                row = 0;
            }
            if (py + h > ATLAS)
                break;
            for (int y = 0; y < h; y++)
                memcpy(g_atlas + (py + y) * ATLAS + px, g->bitmap.buffer + y * g->bitmap.pitch, (size_t)w);
            ov_glyph *o = &g_gl[ch];
            o->u0 = (float)px / ATLAS;
            o->v0 = (float)py / ATLAS;
            o->u1 = (float)(px + w) / ATLAS;
            o->v1 = (float)(py + h) / ATLAS;
            o->w = (float)w;
            o->h = (float)h;
            o->bx = (float)g->bitmap_left;
            o->by = (float)g->bitmap_top;
            o->adv = (float)(g->advance.x >> 6);
            px += w + 2;
            row = h > row ? h : row;
        }
        g_cap = g_gl['H'].by > 0 ? g_gl['H'].by : g_ascent * 0.7f;
        g_have_font = 1;
        fprintf(stderr, "[overlay] font %s %s\n", face->family_name ? face->family_name : "?",
                face->style_name ? face->style_name : "");
    } else
        fprintf(stderr, "[overlay] no font: icons without letters, no text\n");
    if (face)
        FT_Done_Face(face);
    g_state = 1;
}

// ---------------------------------------------------------------- список
static void quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, u32 c)
{
    if (g_nv + 6 > MAX_VERTS)
        return;
    ov_vert *v = g_v + g_nv;
    v[0] = (ov_vert){x0, y0, u0, v0, c};
    v[1] = (ov_vert){x1, y0, u1, v0, c};
    v[2] = (ov_vert){x0, y1, u0, v1, c};
    v[3] = v[1];
    v[4] = (ov_vert){x1, y1, u1, v1, c};
    v[5] = v[2];
    g_nv += 6;
}

static void sprite(int id, float x, float y, float w, float h, u32 c)
{
    const int *r = g_sh_rect[id];
    if (id == SH_WHITE)  // середина сплошного квадрата
        quad(x, y, x + w, y + h, 2.0f / ATLAS, 2.0f / ATLAS, 2.0f / ATLAS, 2.0f / ATLAS, c);
    else
        quad(x, y, x + w, y + h, (float)r[0] / ATLAS, (float)r[1] / ATLAS, (float)(r[0] + r[2]) / ATLAS,
             (float)(r[1] + r[3]) / ATLAS, c);
}

void ov_begin(void)
{
    if (!g_state)
        build_atlas();
    g_nv = 0;
}

void ov_rect(float x, float y, float w, float h, u32 rgba)
{
    if (g_state > 0)
        sprite(SH_WHITE, x, y, w, h, rgba);
}

float ov_text_width(float h, const char *s)
{
    float k = h / FONT_PX, w = 0;
    for (; g_have_font && *s; s++)
        if ((unsigned char)*s < 128)
            w += g_gl[(unsigned char)*s].adv * k;
    return w;
}

float ov_text(float x, float y, float h, u32 rgba, const char *s)
{
    if (g_state <= 0 || !g_have_font)
        return 0;
    float k = h / FONT_PX, pen = x, base = y + g_ascent * k;
    for (; *s; s++) {
        unsigned ch = (unsigned char)*s;
        if (ch >= 128)
            continue;
        const ov_glyph *g = &g_gl[ch];
        if (g->w > 0)
            quad(pen + g->bx * k, base - g->by * k, pen + (g->bx + g->w) * k, base + (g->h - g->by) * k, g->u0, g->v0,
                 g->u1, g->v1, rgba);
        pen += g->adv * k;
    }
    return pen - x;
}

// надпись по центру (cx, cy): по ширине и по высоте заглавных
static void text_centered(float cx, float cy, float h, u32 rgba, const char *s)
{
    float k = h / FONT_PX, base = cy + g_cap * k / 2;
    ov_text(cx - ov_text_width(h, s) / 2, base - g_ascent * k, h, rgba, s);
}

static const struct {
    const char *label;
    float w;  // ширина в долях высоты
} g_btn[OV_BUTTON_N] = {
    [OV_A] = {"A", 1}, [OV_B] = {"B", 1}, [OV_X] = {"X", 1}, [OV_Y] = {"Y", 1}, [OV_L] = {"L", 1.6f},
    [OV_R] = {"R", 1.6f}, [OV_ZL] = {"ZL", 1.9f}, [OV_ZR] = {"ZR", 1.9f}, [OV_PLUS] = {"", 1}, [OV_MINUS] = {"", 1},
    [OV_LSTICK] = {"L", 1}, [OV_RSTICK] = {"R", 1}, [OV_DPAD] = {"", 1}, [OV_DPAD_UP] = {"", 1},
    [OV_DPAD_DOWN] = {"", 1}, [OV_DPAD_LEFT] = {"", 1}, [OV_DPAD_RIGHT] = {"", 1},
};

float ov_icon_width(float h, ov_button b) { return b < OV_BUTTON_N ? g_btn[b].w * h : 0; }

float ov_icon(float x, float y, float h, ov_button b)
{
    if (g_state <= 0 || b >= OV_BUTTON_N)
        return 0;
    const u32 face = OV_RGBA(240, 240, 240, 255), ink = OV_RGBA(30, 30, 30, 255), accent = OV_RGBA(255, 210, 40, 255);
    float w = g_btn[b].w * h, cx = x + w / 2, cy = y + h / 2;
    switch (b) {
    case OV_L: case OV_R: case OV_ZL: case OV_ZR:
        sprite(SH_PILL, x, y, w, h, face);
        text_centered(cx, cy, h * 0.6f, ink, g_btn[b].label);
        break;
    case OV_PLUS: case OV_MINUS:
        sprite(SH_DISC, x, y, h, h, face);
        ov_rect(cx - h * 0.25f, cy - h * 0.06f, h * 0.5f, h * 0.12f, ink);
        if (b == OV_PLUS)
            ov_rect(cx - h * 0.06f, cy - h * 0.25f, h * 0.12f, h * 0.5f, ink);
        break;
    case OV_LSTICK: case OV_RSTICK:
        sprite(SH_DISC, x, y, h, h, face);
        sprite(SH_RING, x + h * 0.14f, y + h * 0.14f, h * 0.72f, h * 0.72f, ink);
        text_centered(cx, cy, h * 0.42f, ink, g_btn[b].label);
        break;
    case OV_DPAD: case OV_DPAD_UP: case OV_DPAD_DOWN: case OV_DPAD_LEFT: case OV_DPAD_RIGHT: {
        sprite(SH_CROSS, x, y, h, h, face);
        static const float off[5][2] = {{0, 0}, {0, -0.3f}, {0, 0.3f}, {-0.3f, 0}, {0.3f, 0}};
        const float *o = off[b - OV_DPAD];
        if (b != OV_DPAD)
            sprite(SH_DISC, cx + o[0] * h - h * 0.11f, cy + o[1] * h - h * 0.11f, h * 0.22f, h * 0.22f, accent);
        break;
    }
    default:
        sprite(SH_DISC, x, y, h, h, face);
        text_centered(cx, cy, h * 0.62f, ink, g_btn[b].label);
        break;
    }
    return w;
}

// ---------------------------------------------------------------- рисование (поток рендера)
static GLuint g_prog, g_vao, g_vbo, g_tex;
static GLint g_loc_size;

static GLuint shader(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    const char *srcs[3] = {SNX_GLSL_HEADER, SNX_GLSL_PRECISION, src};
    glShaderSource(s, 3, srcs, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "[overlay] shader: %s\n", log);
    }
    return s;
}

static void gl_init(void)
{
    static const char vs[] = "layout(location = 0) in vec2 p;\n"
                             "layout(location = 1) in vec2 t;\n"
                             "layout(location = 2) in vec4 c;\n"
                             "uniform vec2 size;\n"
                             "out vec2 vt;\n"
                             "out vec4 vc;\n"
                             "void main() { vt = t; vc = c; gl_Position = vec4(p / size * 2.0 - 1.0, 0.0, 1.0); }\n";
    static const char fs[] = "in vec2 vt;\n"
                             "in vec4 vc;\n"
                             "uniform sampler2D tex;\n"
                             "out vec4 o;\n"
                             "void main() { o = vec4(vc.rgb, vc.a * texture(tex, vt).r); }\n";
    g_prog = glCreateProgram();
    GLuint v = shader(GL_VERTEX_SHADER, vs), f = shader(GL_FRAGMENT_SHADER, fs);
    glAttachShader(g_prog, v);
    glAttachShader(g_prog, f);
    glLinkProgram(g_prog);
    glDeleteShader(v);
    glDeleteShader(f);
    g_loc_size = glGetUniformLocation(g_prog, "size");
    glGenVertexArrays(1, &g_vao);
    glGenBuffers(1, &g_vbo);
    glBindVertexArray(g_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(ov_vert), (void *)0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(ov_vert), (void *)8);
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(ov_vert), (void *)16);
    glGenTextures(1, &g_tex);
    glBindTexture(GL_TEXTURE_2D, g_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, ATLAS, ATLAS, 0, GL_RED, GL_UNSIGNED_BYTE, g_atlas);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

typedef struct {
    u32 fbo;
    int w, h, n;
} ov_batch;  // за ним — n вершин

// Состояние GL, которое трогает рисование: слой D3D8 держит его в своих кешах — вернуть как было.
static void x_draw(void *arg)
{
    const ov_batch *b = arg;
    GLint fb, vp[4], prog, vao, abuf, active, tex, samp, src_rgb, dst_rgb, src_a, dst_a, eq_rgb, eq_a, unpack_al, unpack_row;
    GLboolean blend, depth, stencil, cull, scissor, cmask[4];
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fb);
    glGetIntegerv(GL_VIEWPORT, vp);
    glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &abuf);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
    glGetIntegerv(GL_SAMPLER_BINDING, &samp);
    glGetIntegerv(GL_BLEND_SRC_RGB, &src_rgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &dst_rgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &src_a);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &dst_a);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &eq_rgb);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &eq_a);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpack_al);
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &unpack_row);
    glGetBooleanv(GL_COLOR_WRITEMASK, cmask);
    blend = glIsEnabled(GL_BLEND);
    depth = glIsEnabled(GL_DEPTH_TEST);
    stencil = glIsEnabled(GL_STENCIL_TEST);
    cull = glIsEnabled(GL_CULL_FACE);
    scissor = glIsEnabled(GL_SCISSOR_TEST);

    if (!g_prog)
        gl_init();
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, b->fbo);
    glViewport(0, 0, b->w, b->h);
    glUseProgram(g_prog);
    glUniform2f(g_loc_size, (float)b->w, (float)b->h);
    glBindVertexArray(g_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(sizeof(ov_vert) * (size_t)b->n), b + 1, GL_STREAM_DRAW);
    glBindTexture(GL_TEXTURE_2D, g_tex);
    glBindSampler(0, 0);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glBlendEquation(GL_FUNC_ADD);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDrawArrays(GL_TRIANGLES, 0, b->n);

    glColorMask(cmask[0], cmask[1], cmask[2], cmask[3]);
    (scissor ? glEnable : glDisable)(GL_SCISSOR_TEST);
    (cull ? glEnable : glDisable)(GL_CULL_FACE);
    (stencil ? glEnable : glDisable)(GL_STENCIL_TEST);
    (depth ? glEnable : glDisable)(GL_DEPTH_TEST);
    (blend ? glEnable : glDisable)(GL_BLEND);
    glBlendEquationSeparate((GLenum)eq_rgb, (GLenum)eq_a);
    glBlendFuncSeparate((GLenum)src_rgb, (GLenum)dst_rgb, (GLenum)src_a, (GLenum)dst_a);
    glPixelStorei(GL_UNPACK_ALIGNMENT, unpack_al);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, unpack_row);
    glBindSampler(0, (GLuint)samp);
    glBindTexture(GL_TEXTURE_2D, (GLuint)tex);
    glActiveTexture((GLenum)active);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)abuf);
    glBindVertexArray((GLuint)vao);
    glUseProgram((GLuint)prog);
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)fb);
}

void ov_flush(u32 fbo, int w, int h)
{
    if (g_state <= 0 || !g_nv)
        return;
    size_t size = sizeof(ov_batch) + sizeof(ov_vert) * (size_t)g_nv;
    ov_batch *b = malloc(size);
    if (!b)
        return;
    *b = (ov_batch){fbo, w, h, g_nv};
    memcpy(b + 1, g_v, sizeof(ov_vert) * (size_t)g_nv);
    cs_async(x_draw, b, size);
    free(b);
    g_nv = 0;
}
