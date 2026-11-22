// HLE Direct3D 8: ресурсы — текстуры (2D, кубические), поверхности, вершинные и индексные буферы.
// Текстура — объект GL; блокировка уровня выделяет гостевую память (с прежним содержимым, если уровень уже
// загружен и блокировка не DISCARD), разблокировка загружает уровень целиком. Буферы держат постоянную
// гостевую копию: Lock отдаёт указатель в неё, изменённый диапазон догружается в GL перед отрисовкой.
#include <math.h>
#include <stdlib.h>

#include "runtime/heap.h"
#include "runtime/rt.h"
#include "runtime/hle/d3d8_int.h"

#ifndef GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x83F1
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT 0x83F2
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#endif

// ================================================================ форматы
#define SW(r, g, b, a) {GL_##r, GL_##g, GL_##b, GL_##a}
static const d3dfmt g_fmts[] = {
    {FMT_A8R8G8B8, GL_RGBA8, GL_BGRA, GL_UNSIGNED_BYTE, 4, 0, 0, {0}},
    {FMT_X8R8G8B8, GL_RGBA8, GL_BGRA, GL_UNSIGNED_BYTE, 4, 0, 0, SW(RED, GREEN, BLUE, ONE)},
    {FMT_R8G8B8, GL_RGB8, GL_BGR, GL_UNSIGNED_BYTE, 3, 0, 0, {0}},
    {FMT_R5G6B5, GL_RGB565, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, 2, 0, 0, {0}},
    {FMT_X1R5G5B5, GL_RGB5_A1, GL_BGRA, GL_UNSIGNED_SHORT_1_5_5_5_REV, 2, 0, 0, SW(RED, GREEN, BLUE, ONE)},
    {FMT_A1R5G5B5, GL_RGB5_A1, GL_BGRA, GL_UNSIGNED_SHORT_1_5_5_5_REV, 2, 0, 0, {0}},
    {FMT_A4R4G4B4, GL_RGBA4, GL_BGRA, GL_UNSIGNED_SHORT_4_4_4_4_REV, 2, 0, 0, {0}},
    {FMT_X4R4G4B4, GL_RGBA4, GL_BGRA, GL_UNSIGNED_SHORT_4_4_4_4_REV, 2, 0, 0, SW(RED, GREEN, BLUE, ONE)},
    {FMT_A8, GL_R8, GL_RED, GL_UNSIGNED_BYTE, 1, 0, 0, SW(ZERO, ZERO, ZERO, RED)},
    {FMT_L8, GL_R8, GL_RED, GL_UNSIGNED_BYTE, 1, 0, 0, SW(RED, RED, RED, ONE)},
    {FMT_A8L8, GL_RG8, GL_RG, GL_UNSIGNED_BYTE, 2, 0, 0, SW(RED, RED, RED, GREEN)},
    {FMT_V8U8, GL_RG8_SNORM, GL_RG, GL_BYTE, 2, 0, 0, SW(RED, GREEN, ONE, ONE)},
    {FMT_DXT1, GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, 0, 0, 8, 0, 0, {0}},
    {FMT_DXT2, GL_COMPRESSED_RGBA_S3TC_DXT3_EXT, 0, 0, 16, 0, 0, {0}},
    {FMT_DXT3, GL_COMPRESSED_RGBA_S3TC_DXT3_EXT, 0, 0, 16, 0, 0, {0}},
    {FMT_DXT4, GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, 0, 0, 16, 0, 0, {0}},
    {FMT_DXT5, GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, 0, 0, 16, 0, 0, {0}},
    {FMT_D16, GL_DEPTH_COMPONENT16, GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT, 2, 1, 0, {0}},
    {FMT_D16_LOCKABLE, GL_DEPTH_COMPONENT16, GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT, 2, 1, 0, {0}},
    {FMT_D24X8, GL_DEPTH_COMPONENT24, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, 4, 1, 0, {0}},
    {FMT_D32, GL_DEPTH_COMPONENT32, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, 4, 1, 0, {0}},
    {FMT_D24S8, GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, 4, 1, 1, {0}},
    {FMT_D24X4S4, GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, 4, 1, 1, {0}},
    {FMT_D15S1, GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, 4, 1, 1, {0}},
};
#undef SW

const d3dfmt *d3d8_format(u32 fmt)
{
    for (size_t i = 0; i < sizeof g_fmts / sizeof g_fmts[0]; i++)
        if (g_fmts[i].fmt == fmt)
            return &g_fmts[i];
    return NULL;
}

u32 d3d8_pitch(const d3dfmt *f, u32 w) { return f->format ? w * f->bpp : ((w + 3) / 4) * f->bpp; }
u32 d3d8_rows(const d3dfmt *f, u32 h) { return f->format ? h : (h + 3) / 4; }

static u32 mip(u32 v, u32 level)
{
    v >>= level;
    return v ? v : 1;
}

static u32 g_dev_obj;
static u32 g_vt_tex, g_vt_cube, g_vt_surf, g_vt_vb, g_vt_ib;

static u32 galloc(u32 size) { return heap_alloc(heap_process(), 0, size ? size : 1); }
static void gfree(u32 p)
{
    if (p)
        heap_free(heap_process(), p);
}

// ================================================================ текстуры: GL
static GLenum face_target(const d3dtex *t, int face)
{
    return t->cube ? (GLenum)(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face) : GL_TEXTURE_2D;
}

static GLenum tex_target(const d3dtex *t) { return t->cube ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D; }

static void upload_level(d3dtex *t, int face, u32 level, const void *data)
{
    const d3dfmt *f = d3d8_format(t->fmt);
    u32 w = mip(t->w, level), h = mip(t->h, level);
    g_hitch.uploads++;  // строка [hitch] (d3d8_present)
    g_hitch.upload_bytes += (u64)d3d8_pitch(f, w) * d3d8_rows(f, h);
    glBindTexture(tex_target(t), t->gl);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    if (f->format)  // размер известен — с потоком рендера данные копируются в очередь
        cs_TexImage2D(face_target(t, face), (GLint)level, (GLint)f->internal, (GLsizei)w, (GLsizei)h, f->format,
                      f->type, data, (size_t)d3d8_pitch(f, w) * d3d8_rows(f, h));
    else
        glCompressedTexImage2D(face_target(t, face), (GLint)level, f->internal, (GLsizei)w, (GLsizei)h, 0,
                               (GLsizei)(d3d8_pitch(f, w) * d3d8_rows(f, h)), data);
}

static void read_level(d3dtex *t, int face, u32 level, void *out)
{
    const d3dfmt *f = d3d8_format(t->fmt);
    glBindTexture(tex_target(t), t->gl);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    if (f->format)
        glGetTexImage(face_target(t, face), (GLint)level, f->format, f->type, out);
    else
        glGetCompressedTexImage(face_target(t, face), (GLint)level, out);
}

static void tex_destroy(void *p);

d3dtex *d3d8_tex_create(u32 w, u32 h, u32 levels, u32 usage, u32 fmt, u32 pool, int cube)
{
    const d3dfmt *f = d3d8_format(fmt);
    if (!f || !w || !h)
        return NULL;
    u32 full = 1;
    for (u32 m = w > h ? w : h; m > 1; m >>= 1)
        full++;
    if (!levels || levels > full)
        levels = full;
    d3dtex *t = calloc(1, sizeof *t);
    t->cube = cube;
    t->w = w;
    t->h = h;
    t->levels = levels;
    t->fmt = fmt;
    t->usage = usage;
    t->pool = pool;
    t->surf = calloc((size_t)(cube ? 6 : 1) * levels, sizeof *t->surf);
    glGenTextures(1, &t->gl);
    GLenum tg = tex_target(t);
    glBindTexture(tg, t->gl);
    glTexParameteri(tg, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(tg, GL_TEXTURE_MAX_LEVEL, (GLint)levels - 1);
    if (f->swizzle[0]) {
        glTexParameteri(tg, GL_TEXTURE_SWIZZLE_R, f->swizzle[0]);
        glTexParameteri(tg, GL_TEXTURE_SWIZZLE_G, f->swizzle[1]);
        glTexParameteri(tg, GL_TEXTURE_SWIZZLE_B, f->swizzle[2]);
        glTexParameteri(tg, GL_TEXTURE_SWIZZLE_A, f->swizzle[3]);
    }
    for (int face = 0; face < (cube ? 6 : 1); face++)
        for (u32 l = 0; l < levels; l++)
            upload_level(t, face, l, NULL);
    t->obj = com_create(cube ? g_vt_cube : g_vt_tex, t, tex_destroy);
    return t;
}

// ================================================================ поверхности
static void surf_destroy(void *p)
{
    d3dsurf *s = p;
    if (s->rb)
        glDeleteRenderbuffers(1, &s->rb);
    if (s->movie_tex)
        glDeleteTextures(1, &s->movie_tex);
    if (s->own_mem)
        gfree(s->mem);
    gfree(s->rb_mem);
    free(s);
}

static d3dsurf *surf_new(u32 w, u32 h, u32 fmt, u32 usage, u32 pool)
{
    d3dsurf *s = calloc(1, sizeof *s);
    s->w = w;
    s->h = h;
    s->fmt = fmt;
    s->usage = usage;
    s->pool = pool;
    s->obj = com_create(g_vt_surf, s, surf_destroy);
    return s;
}

d3dsurf *d3d8_surf_create(u32 w, u32 h, u32 fmt, u32 usage, u32 pool)
{
    const d3dfmt *f = d3d8_format(fmt);
    if (!f)
        return NULL;
    d3dsurf *s = surf_new(w, h, fmt, usage, pool);
    if (usage & (USAGE_RENDERTARGET | USAGE_DEPTHSTENCIL)) {
        glGenRenderbuffers(1, &s->rb);
        glBindRenderbuffer(GL_RENDERBUFFER, s->rb);
        glRenderbufferStorage(GL_RENDERBUFFER, f->internal, (GLsizei)w, (GLsizei)h);
    } else {  // поверхность в памяти (CreateImageSurface): только гостевая копия
        s->pitch = d3d8_pitch(f, w);
        s->mem = galloc(s->pitch * d3d8_rows(f, h));
        s->own_mem = 1;
        memset(g_mem + s->mem, 0, s->pitch * d3d8_rows(f, h));
    }
    return s;
}

static void tex_destroy(void *p)
{
    d3dtex *t = p;
    for (u32 i = 0; i < (t->cube ? 6u : 1u) * t->levels; i++)
        if (t->surf[i]) {
            gfree(t->surf[i]->locked ? t->surf[i]->mem : 0);
            com_release(t->surf[i]->obj);  // собственный счётчик поверхности уровня всегда 1
        }
    free(t->surf);
    glDeleteTextures(1, &t->gl);
    free(t);
}

static d3dsurf *tex_surface(d3dtex *t, int face, u32 level)
{
    d3dsurf **sp = &t->surf[(u32)face * t->levels + level];
    if (!*sp) {
        d3dsurf *s = surf_new(mip(t->w, level), mip(t->h, level), t->fmt, t->usage, t->pool);
        s->tex = t;
        s->face = face;
        s->level = (int)level;
        *sp = s;
    }
    return *sp;
}

d3dtex *d3d8_tex_from(u32 obj)
{
    if (!obj)
        return NULL;
    u32 vt = rd32(g_mem, obj);
    return vt == g_vt_tex || vt == g_vt_cube ? com_host(obj) : NULL;
}

d3dsurf *d3d8_surf_from(u32 obj) { return obj && rd32(g_mem, obj) == g_vt_surf ? com_host(obj) : NULL; }

d3dbuf *d3d8_buf_from(u32 obj)
{
    if (!obj)
        return NULL;
    u32 vt = rd32(g_mem, obj);
    return vt == g_vt_vb || vt == g_vt_ib ? com_host(obj) : NULL;
}

// Ролик рисует порт (movie_overlay: кадр больше заднего буфера): игра всё равно блокирует задний буфер на каждом
// кадре ролика (копия Bink мимо), и чтение FBO при Lock (glReadPixels — ожидание всего ГП) и заливка при Unlock
// стоили бы десятков мс на кадр. Задний буфер в это время целиком перекрывает кадр ролика — не читаем и не заливаем.
static int overlay_target(const d3dsurf *s)
{
    d3ddev *d = d3d8_dev();
    return d && s == d->backbuffer && g_movie_overlay_ns && rt_ticks_ns() - g_movie_overlay_ns < 250000000ull;
}

// Блокировка поверхности: уровень текстуры, самостоятельная поверхность в памяти или цель рендера
static u32 surf_lock(d3dsurf *s, u32 locked_rect, u32 rect, u32 flags)
{
    const d3dfmt *f = d3d8_format(s->fmt);
    if (s->locked)
        return D3DERR_INVALIDCALL;
    u32 pitch = d3d8_pitch(f, s->w), size = pitch * d3d8_rows(f, s->h);
    if (rect) {
        for (int i = 0; i < 4; i++)
            s->lock_rect[i] = rd32(g_mem, rect + 4u * (u32)i);
    } else {
        s->lock_rect[0] = s->lock_rect[1] = 0;
        s->lock_rect[2] = s->w;
        s->lock_rect[3] = s->h;
    }
    if (!s->own_mem) {
        if (s->rb && !s->rb_mem)
            s->rb_mem = galloc(size);
        s->mem = s->rb ? s->rb_mem : galloc(size);
        s->pitch = pitch;
        if (!s->mem)
            return D3DERR_OUTOFVIDEOMEMORY;
        int keep = !(flags & LOCK_DISCARD);
        if (s->tex && keep && (s->tex->filled & (1u << s->level)))
            read_level(s->tex, s->face, (u32)s->level, g_mem + s->mem);
        else if (s->rb && keep && !s->movie_mode && !overlay_target(s)) {
            // цель рендера: прочитать заблокированный прямоугольник из её FBO (снимок экрана; ReduceMouseLag
            // блокирует 1x1 каждый кадр — ради ожидания GPU). Во время ролика не читаем: заливается только
            // кадр ролика (write_movie), остальное в памяти блокировки не нужно
            GLint l = (GLint)s->lock_rect[0], t = (GLint)s->lock_rect[1];
            GLuint fb;
            glGenFramebuffers(1, &fb);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, fb);
            glFramebufferRenderbuffer(GL_READ_FRAMEBUFFER, f->depth ? GL_DEPTH_ATTACHMENT : GL_COLOR_ATTACHMENT0,
                                      GL_RENDERBUFFER, s->rb);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glPixelStorei(GL_PACK_ROW_LENGTH, (GLint)s->w);
            glReadPixels(l, t, (GLsizei)s->lock_rect[2] - l, (GLsizei)s->lock_rect[3] - t, f->format, f->type,
                         g_mem + s->mem + (u32)t * pitch + (u32)l * f->bpp);
            glPixelStorei(GL_PACK_ROW_LENGTH, 0);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
            glDeleteFramebuffers(1, &fb);
            d3d8_bind_target(d3d8_dev());
        } else if (!s->rb)
            memset(g_mem + s->mem, 0, size);
    }
    u32 off = 0;
    if (rect) {
        u32 l = s->lock_rect[0], t = s->lock_rect[1];
        off = f->format ? t * pitch + l * f->bpp : (t / 4) * pitch + (l / 4) * f->bpp;
    }
    s->locked = 1;
    s->lock_flags = flags;
    wr32(g_mem, locked_rect, s->pitch);
    wr32(g_mem, locked_rect + 4, s->mem + off);
    return D3D_OK;
}

u64 g_movie_drawn_ns, g_movie_overlay_ns;

// Кадр ролика Bink (d3d8_movie_frame: D3DDrv выводит его прямо в залоченный задний буфер): только его
// прямоугольник — в постоянную текстуру и blit в цель, растянутый с сохранением пропорций (на ПК 640x480/800x600
// почти занимали экран, а в 1920x1080 были бы окошком), поля вокруг — чёрные.
// 0 — прямоугольник вне блокировки (тогда — обычный путь)
static int write_movie(d3dsurf *s, const d3dfmt *f)
{
    GLint mx = (GLint)s->movie[0], my = (GLint)s->movie[1], mw = (GLint)s->movie[2], mh = (GLint)s->movie[3],
          W = (GLint)s->w, H = (GLint)s->h;
    if (mx < (GLint)s->lock_rect[0] || my < (GLint)s->lock_rect[1] || mx + mw > (GLint)s->lock_rect[2] ||
        my + mh > (GLint)s->lock_rect[3])
        return 0;
    if (!s->movie_tex || s->movie_tw != (u32)mw || s->movie_th != (u32)mh) {
        if (!s->movie_tex)
            glGenTextures(1, &s->movie_tex);
        glBindTexture(GL_TEXTURE_2D, s->movie_tex);
        cs_TexImage2D(GL_TEXTURE_2D, 0, (GLint)f->internal, mw, mh, f->format, f->type, NULL, 0);
        s->movie_tw = (u32)mw;
        s->movie_th = (u32)mh;
    } else
        glBindTexture(GL_TEXTURE_2D, s->movie_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    cs_TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, mw, mh, f->format, f->type,
                     g_mem + s->mem + (u32)my * s->pitch + (u32)mx * f->bpp, (size_t)mw * f->bpp, s->pitch);
    glBindTexture(GL_TEXTURE_2D, 0);
    GLuint fb[2];
    glGenFramebuffers(2, fb);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fb[0]);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s->movie_tex, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fb[1]);
    glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, s->rb);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    float sc = fminf((float)W / (float)mw, (float)H / (float)mh);
    if (sc > 1.05f) {
        GLint w = (GLint)((float)mw * sc), h = (GLint)((float)mh * sc);
        if (w < W || h < H) {
            // поля вокруг — чёрные, как на ПК в 4:3, где ролик закрывал экран (иначе на 16:9 по бокам — мир под ним)
            glClearColor(0, 0, 0, 1);
            glClear(GL_COLOR_BUFFER_BIT);
        }
        glBlitFramebuffer(0, 0, mw, mh, (W - w) / 2, (H - h) / 2, (W - w) / 2 + w, (H - h) / 2 + h,
                          GL_COLOR_BUFFER_BIT, GL_LINEAR);
    } else
        glBlitFramebuffer(0, 0, mw, mh, mx, my, mx + mw, my + mh, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(2, fb);
    d3d8_bind_target(d3d8_dev());  // и маски/scissor — заново перед следующей отрисовкой
    g_movie_drawn_ns = rt_ticks_ns();
    return 1;
}

// Запись гостя в залоченную цель рендера: данные — во временную текстуру, затем blit заблокированного
// прямоугольника в renderbuffer (оба «сверху вниз»); кадр ролика — write_movie.
static void write_target(d3dsurf *s, const d3dfmt *f)
{
    GLint l = (GLint)s->lock_rect[0], t = (GLint)s->lock_rect[1], r = (GLint)s->lock_rect[2],
          b = (GLint)s->lock_rect[3];
    int was_movie = s->movie_mode, movie = s->has_movie;
    s->has_movie = 0;
    s->movie_mode = movie && write_movie(s, f);
    if (s->movie_mode || was_movie || overlay_target(s))
        return;  // кадр ролика залит; или цель не читалась при Lock (ролик кончился, ролик рисует порт)
    if (r <= l || b <= t)
        return;
    GLuint tex, fb[2];
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, (GLint)s->w);  // только заблокированный прямоугольник
    cs_TexImage2D(GL_TEXTURE_2D, 0, (GLint)f->internal, r - l, b - t, f->format, f->type,
                  g_mem + s->mem + (u32)t * s->pitch + (u32)l * f->bpp,
                  (size_t)(b - t - 1) * s->pitch + (size_t)(r - l) * f->bpp);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glGenFramebuffers(2, fb);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fb[0]);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fb[1]);
    glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, s->rb);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBlitFramebuffer(0, 0, r - l, b - t, l, t, r, b, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(2, fb);
    glDeleteTextures(1, &tex);
    d3d8_bind_target(d3d8_dev());  // и маски/scissor — заново перед следующей отрисовкой
}

void d3d8_movie_frame(u32 dest, u32 pitch, u32 x, u32 y, u32 w, u32 h)
{
    d3ddev *d = d3d8_dev();
    d3dsurf *s = d ? d->backbuffer : NULL;
    if (!s || !s->locked || !s->mem || s->pitch != pitch || dest < s->mem || !w || !h)
        return;
    const d3dfmt *f = d3d8_format(s->fmt);
    u32 off = dest - s->mem;
    s->movie[0] = (off % pitch) / f->bpp + x;
    s->movie[1] = off / pitch + y;
    s->movie[2] = w;
    s->movie[3] = h;
    s->has_movie = 1;
}

static u32 surf_unlock(d3dsurf *s)
{
    if (!s->locked)
        return D3DERR_INVALIDCALL;
    s->locked = 0;
    if (s->own_mem)
        return D3D_OK;
    if (!(s->lock_flags & LOCK_READONLY)) {
        if (s->tex) {
            upload_level(s->tex, s->face, (u32)s->level, g_mem + s->mem);
            if (s->face == 0 && s->level < 32)
                s->tex->filled |= 1u << s->level;
        } else if (s->rb) {
            const d3dfmt *f = d3d8_format(s->fmt);
            if (f->depth)
                hle_todo("d3d8: writing into a locked depth buffer is ignored");
            else
                write_target(s, f);
        }
    }
    if (s->mem != s->rb_mem)
        gfree(s->mem);
    s->mem = 0;
    return D3D_OK;
}

// ================================================================ COM: общее для ресурсов
static void res_QueryInterface(CPU *c)
{
    wr32(g_mem, ARG(2), 0);
    RET(3, E_NOINTERFACE);
}

static void res_GetDevice(CPU *c)
{
    com_addref(g_dev_obj);
    wr32(g_mem, ARG(1), g_dev_obj);
    RET(2, D3D_OK);
}

static void res_SetPrivateData(CPU *c) { RET(5, D3D_OK); }
static void res_GetPrivateData(CPU *c) { RET(4, 0x88760866u); }  // D3DERR_NOTFOUND
static void res_FreePrivateData(CPU *c) { RET(2, D3D_OK); }
static void res_SetPriority(CPU *c) { RET(2, 0); }
static void res_GetPriority(CPU *c) { RET(1, 0); }
static void res_PreLoad(CPU *c) { RET(1, 0); }

// ================================================================ IDirect3DTexture8 / IDirect3DCubeTexture8
#define TEX(c) ((d3dtex *)com_host(ARG(0)))

static void tex_GetType(CPU *c) { RET(1, TEX(c)->cube ? RTYPE_CUBETEXTURE : RTYPE_TEXTURE); }

static void tex_SetLOD(CPU *c)
{
    d3dtex *t = TEX(c);
    u32 old = t->lod;
    t->lod = ARG(1) < t->levels ? ARG(1) : t->levels - 1;
    glBindTexture(tex_target(t), t->gl);
    glTexParameteri(tex_target(t), GL_TEXTURE_BASE_LEVEL, (GLint)t->lod);
    RET(2, old);
}

static void tex_GetLOD(CPU *c) { RET(1, TEX(c)->lod); }
static void tex_GetLevelCount(CPU *c) { RET(1, TEX(c)->levels); }

// D3DSURFACE_DESC: Format, Type, Usage, Pool, Size, MultiSampleType, Width, Height
static void put_desc(u32 p, const d3dsurf *s)
{
    const d3dfmt *f = d3d8_format(s->fmt);
    wr32(g_mem, p, s->fmt);
    wr32(g_mem, p + 4, RTYPE_SURFACE);
    wr32(g_mem, p + 8, s->usage);
    wr32(g_mem, p + 12, s->pool);
    wr32(g_mem, p + 16, d3d8_pitch(f, s->w) * d3d8_rows(f, s->h));
    wr32(g_mem, p + 20, 0);
    wr32(g_mem, p + 24, s->w);
    wr32(g_mem, p + 28, s->h);
}

static void tex_GetLevelDesc(CPU *c)
{
    d3dtex *t = TEX(c);
    if (ARG(1) >= t->levels)
        RET(3, D3DERR_INVALIDCALL);
    put_desc(ARG(2), tex_surface(t, 0, ARG(1)));
    RET(3, D3D_OK);
}

static void tex_GetSurfaceLevel(CPU *c)
{
    d3dtex *t = TEX(c);
    if (ARG(1) >= t->levels) {
        wr32(g_mem, ARG(2), 0);
        RET(3, D3DERR_INVALIDCALL);
    }
    com_addref(t->obj);  // поверхность уровня делит счётчик с контейнером
    wr32(g_mem, ARG(2), tex_surface(t, 0, ARG(1))->obj);
    RET(3, D3D_OK);
}

static void tex_LockRect(CPU *c)  // (Level, pLockedRect, pRect, Flags)
{
    d3dtex *t = TEX(c);
    if (ARG(1) >= t->levels)
        RET(5, D3DERR_INVALIDCALL);
    RET(5, surf_lock(tex_surface(t, 0, ARG(1)), ARG(2), ARG(3), ARG(4)));
}

static void tex_UnlockRect(CPU *c)
{
    d3dtex *t = TEX(c);
    if (ARG(1) >= t->levels)
        RET(2, D3DERR_INVALIDCALL);
    RET(2, surf_unlock(tex_surface(t, 0, ARG(1))));
}

static void tex_AddDirtyRect(CPU *c) { RET(2, D3D_OK); }

static void cube_GetCubeMapSurface(CPU *c)  // (FaceType, Level, pp)
{
    d3dtex *t = TEX(c);
    if (ARG(1) >= 6 || ARG(2) >= t->levels) {
        wr32(g_mem, ARG(3), 0);
        RET(4, D3DERR_INVALIDCALL);
    }
    com_addref(t->obj);
    wr32(g_mem, ARG(3), tex_surface(t, (int)ARG(1), ARG(2))->obj);
    RET(4, D3D_OK);
}

static void cube_LockRect(CPU *c)  // (FaceType, Level, pLockedRect, pRect, Flags)
{
    d3dtex *t = TEX(c);
    if (ARG(1) >= 6 || ARG(2) >= t->levels)
        RET(6, D3DERR_INVALIDCALL);
    RET(6, surf_lock(tex_surface(t, (int)ARG(1), ARG(2)), ARG(3), ARG(4), ARG(5)));
}

static void cube_UnlockRect(CPU *c)
{
    d3dtex *t = TEX(c);
    if (ARG(1) >= 6 || ARG(2) >= t->levels)
        RET(3, D3DERR_INVALIDCALL);
    RET(3, surf_unlock(tex_surface(t, (int)ARG(1), ARG(2))));
}

static void cube_AddDirtyRect(CPU *c) { RET(3, D3D_OK); }

// ================================================================ IDirect3DSurface8
#define SURF(c) ((d3dsurf *)com_host(ARG(0)))

static void surf_AddRef(CPU *c)
{
    d3dsurf *s = SURF(c);
    RET(1, s && s->tex ? com_addref(s->tex->obj) : com_addref(ARG(0)));
}

static void surf_Release(CPU *c)
{
    d3dsurf *s = SURF(c);
    RET(1, s && s->tex ? com_release(s->tex->obj) : com_release(ARG(0)));
}

static void surf_GetContainer(CPU *c)  // (riid, ppContainer)
{
    d3dsurf *s = SURF(c);
    u32 obj = s->tex ? s->tex->obj : g_dev_obj;
    com_addref(obj);
    wr32(g_mem, ARG(2), obj);
    RET(3, D3D_OK);
}

static void surf_GetDesc(CPU *c)
{
    put_desc(ARG(1), SURF(c));
    RET(2, D3D_OK);
}

static void surf_LockRect(CPU *c) { RET(4, surf_lock(SURF(c), ARG(1), ARG(2), ARG(3))); }
static void surf_UnlockRect(CPU *c) { RET(1, surf_unlock(SURF(c))); }

// ================================================================ буферы
static void buf_destroy(void *p)
{
    d3dbuf *b = p;
    d3ddev *d = d3d8_dev();
    if (d) {  // устройство держит ссылку на привязанные буферы, так что здесь они уже отвязаны
        for (int i = 0; i < MAX_STREAMS; i++)
            if (d->stream[i] == b)
                d->stream[i] = NULL;
        if (d->indices == b)
            d->indices = NULL;
    }
    glDeleteBuffers(1, &b->gl);
    gfree(b->mem);
    free(b);
}

d3dbuf *d3d8_buf_create(int index, u32 size, u32 usage, u32 fvf_or_fmt, u32 pool)
{
    d3dbuf *b = calloc(1, sizeof *b);
    b->index = index;
    b->size = size;
    b->usage = usage;
    b->pool = pool;
    if (index)
        b->fmt = fvf_or_fmt;
    else
        b->fvf = fvf_or_fmt;
    b->mem = galloc(size);
    if (!b->mem) {
        free(b);
        return NULL;
    }
    memset(g_mem + b->mem, 0, size);
    glGenBuffers(1, &b->gl);
    glBindBuffer(GL_ARRAY_BUFFER, b->gl);
    glBufferData(GL_ARRAY_BUFFER, size, NULL, (usage & USAGE_DYNAMIC) ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW);
    b->obj = com_create(index ? g_vt_ib : g_vt_vb, b, buf_destroy);
    return b;
}

// Догрузка перед отрисовкой. glBufferSubData в буфер, который GPU ещё читает, ждёт GPU (на маке это съедало
// ~70 % кадра: D3DDrv гоняет динамические буферы через Lock каждый кадр). Поэтому как в D3D: после Lock без
// NOOVERWRITE (в т. ч. DISCARD) — новое хранилище с полной гостевой копией, после NOOVERWRITE (приложение
// обещает не трогать то, что рисуется) — запись без синхронизации с GPU.
void d3d8_buf_sync(d3dbuf *b)
{
    if (b->dirty_hi <= b->dirty_lo)
        return;
    glBindBuffer(GL_ARRAY_BUFFER, b->gl);
    u32 lo = b->dirty_lo, len = b->dirty_hi - b->dirty_lo;
    static int glthread = -1;
    if (glthread < 0)
        glthread = getenv("SNX_GLTHREAD") != NULL;
    if (b->orphan) {
        glBufferData(GL_ARRAY_BUFFER, b->size, g_mem + b->mem,
                     (b->usage & USAGE_DYNAMIC) ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW);
    } else if (glthread) {
        // glthread Mesa: отображение буфера ждало бы поток драйвера — только копия в его очередь
        glBufferSubData(GL_ARRAY_BUFFER, lo, len, g_mem + b->mem + lo);
    } else
        cs_BufferWriteUnsync(GL_ARRAY_BUFFER, lo, len, g_mem + b->mem + lo);
    b->orphan = 0;
    b->dirty_lo = b->dirty_hi = 0;
}

#define BUF(c) ((d3dbuf *)com_host(ARG(0)))

static void buf_GetType(CPU *c) { RET(1, BUF(c)->index ? RTYPE_INDEXBUFFER : RTYPE_VERTEXBUFFER); }

static void buf_Lock(CPU *c)  // (OffsetToLock, SizeToLock, ppbData, Flags)
{
    d3dbuf *b = BUF(c);
    u32 off = ARG(1), size = ARG(2), flags = ARG(4);
    if (off > b->size)
        RET(5, D3DERR_INVALIDCALL);
    if (!size || off + size > b->size)
        size = b->size - off;
    if (!(flags & LOCK_READONLY)) {
        if (!(flags & LOCK_NOOVERWRITE))
            b->orphan = 1;
        if (b->dirty_hi <= b->dirty_lo) {
            b->dirty_lo = off;
            b->dirty_hi = off + size;
        } else {
            if (off < b->dirty_lo)
                b->dirty_lo = off;
            if (off + size > b->dirty_hi)
                b->dirty_hi = off + size;
        }
    }
    wr32(g_mem, ARG(3), b->mem + off);
    RET(5, D3D_OK);
}

static void buf_Unlock(CPU *c) { RET(1, D3D_OK); }

// D3DVERTEXBUFFER_DESC: Format, Type, Usage, Pool, Size, FVF; D3DINDEXBUFFER_DESC: Format, Type, Usage, Pool, Size
static void buf_GetDesc(CPU *c)
{
    d3dbuf *b = BUF(c);
    u32 p = ARG(1);
    wr32(g_mem, p, b->index ? b->fmt : FMT_VERTEXDATA);
    wr32(g_mem, p + 4, b->index ? RTYPE_INDEXBUFFER : RTYPE_VERTEXBUFFER);
    wr32(g_mem, p + 8, b->usage);
    wr32(g_mem, p + 12, b->pool);
    wr32(g_mem, p + 16, b->size);
    if (!b->index)
        wr32(g_mem, p + 20, b->fvf);
    RET(2, D3D_OK);
}

// ================================================================ vtables
void d3d8_res_init(u32 device_obj)
{
    g_dev_obj = device_obj;
    if (g_vt_tex)
        return;
#define RES_METHODS(get_type)                                                                                  \
    res_QueryInterface, com_AddRef, com_Release, res_GetDevice, res_SetPrivateData, res_GetPrivateData,        \
        res_FreePrivateData, res_SetPriority, res_GetPriority, res_PreLoad, get_type
    guest_fn tex[19] = {RES_METHODS(tex_GetType), tex_SetLOD, tex_GetLOD, tex_GetLevelCount, tex_GetLevelDesc,
                        tex_GetSurfaceLevel, tex_LockRect, tex_UnlockRect, tex_AddDirtyRect};
    g_vt_tex = com_vtable("IDirect3DTexture8", tex, 19);
    guest_fn cube[19] = {RES_METHODS(tex_GetType), tex_SetLOD, tex_GetLOD, tex_GetLevelCount, tex_GetLevelDesc,
                         cube_GetCubeMapSurface, cube_LockRect, cube_UnlockRect, cube_AddDirtyRect};
    g_vt_cube = com_vtable("IDirect3DCubeTexture8", cube, 19);
    guest_fn vb[14] = {RES_METHODS(buf_GetType), buf_Lock, buf_Unlock, buf_GetDesc};
    g_vt_vb = com_vtable("IDirect3DVertexBuffer8", vb, 14);
    g_vt_ib = com_vtable("IDirect3DIndexBuffer8", vb, 14);
#undef RES_METHODS
    guest_fn surf[11] = {res_QueryInterface, surf_AddRef,    surf_Release,  res_GetDevice,
                         res_SetPrivateData, res_GetPrivateData, res_FreePrivateData, surf_GetContainer,
                         surf_GetDesc,       surf_LockRect,  surf_UnlockRect};
    g_vt_surf = com_vtable("IDirect3DSurface8", surf, 11);
}
