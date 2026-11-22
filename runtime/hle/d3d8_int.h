// HLE Direct3D 8 поверх OpenGL 4.1 core: общее для d3d8*.c (форматы, ресурсы, состояние устройства).
// Соглашения отрисовки: все изображения GL хранятся «сверху вниз», как в D3D (строка 0 — верх): текстуры
// загружаются как есть, рендер идёт в FBO с перевёрнутой осью y клипа, и только Present переворачивает кадр
// при копировании в окно. Поэтому координаты окна GL численно равны экранным D3D (+0,5 пикселя: центры
// пикселей D3D целые), а D3D-«по часовой» — это GL-«против часовой».
#pragma once
#include "platform/gl.h"
#include "runtime/hle/com.h"
#include "runtime/hle/gl_cs.h"  // вызовы GL — через поток рендера (если включён)

#ifdef __cplusplus
extern "C" {
#endif

#define D3D_OK 0u
#define D3DERR_NOTAVAILABLE 0x8876086Au
#define D3DERR_INVALIDCALL 0x8876086Cu
#define D3DERR_OUTOFVIDEOMEMORY 0x8876017Cu

enum {
    FMT_UNKNOWN = 0, FMT_R8G8B8 = 20, FMT_A8R8G8B8 = 21, FMT_X8R8G8B8 = 22, FMT_R5G6B5 = 23, FMT_X1R5G5B5 = 24,
    FMT_A1R5G5B5 = 25, FMT_A4R4G4B4 = 26, FMT_A8 = 28, FMT_X4R4G4B4 = 30, FMT_P8 = 41, FMT_L8 = 50, FMT_A8L8 = 51,
    FMT_V8U8 = 60, FMT_D16_LOCKABLE = 70, FMT_D32 = 71, FMT_D15S1 = 73, FMT_D24S8 = 75, FMT_D24X8 = 77,
    FMT_D24X4S4 = 79, FMT_D16 = 80, FMT_VERTEXDATA = 100, FMT_INDEX16 = 101, FMT_INDEX32 = 102,
};
#define FOURCC(a, b, c, d) ((u32)(a) | (u32)(b) << 8 | (u32)(c) << 16 | (u32)(d) << 24)
#define FMT_DXT1 FOURCC('D', 'X', 'T', '1')
#define FMT_DXT2 FOURCC('D', 'X', 'T', '2')
#define FMT_DXT3 FOURCC('D', 'X', 'T', '3')
#define FMT_DXT4 FOURCC('D', 'X', 'T', '4')
#define FMT_DXT5 FOURCC('D', 'X', 'T', '5')

#define USAGE_RENDERTARGET 1u
#define USAGE_DEPTHSTENCIL 2u
#define USAGE_WRITEONLY 8u
#define USAGE_DYNAMIC 0x200u
enum { POOL_DEFAULT = 0, POOL_MANAGED = 1, POOL_SYSTEMMEM = 2, POOL_SCRATCH = 3 };
enum { RTYPE_SURFACE = 1, RTYPE_VOLUME, RTYPE_TEXTURE, RTYPE_VOLUMETEXTURE, RTYPE_CUBETEXTURE, RTYPE_VERTEXBUFFER,
       RTYPE_INDEXBUFFER };
#define LOCK_READONLY 0x10u
#define LOCK_NOOVERWRITE 0x1000u
#define LOCK_DISCARD 0x2000u

// ---------------------------------------------------------------- форматы
typedef struct {
    u32 fmt;
    GLenum internal, format, type;  // format == 0 — сжатый (internal — формат S3TC)
    u8 bpp;                         // байт на пиксель; у сжатых — байт на блок 4x4
    u8 depth, stencil;              // формат глубины/трафарета
    GLint swizzle[4];               // GL_RED.. / GL_ZERO / GL_ONE (0 — без перестановки)
} d3dfmt;
const d3dfmt *d3d8_format(u32 fmt);      // NULL — формат не поддержан
u32 d3d8_pitch(const d3dfmt *f, u32 w);  // байт в строке (строке блоков у сжатых)
u32 d3d8_rows(const d3dfmt *f, u32 h);   // строк (строк блоков)

// ---------------------------------------------------------------- ресурсы
typedef struct d3dtex d3dtex;
typedef struct d3dsurf {
    u32 obj;               // гостевой COM-объект
    d3dtex *tex;           // контейнер (уровень текстуры) или NULL
    int face, level;
    u32 w, h, fmt, usage, pool;
    GLuint rb;             // свой renderbuffer (цели рендера/глубина вне текстур)
    u32 mem, pitch;        // гостевая память блокировки (у SYSTEMMEM-поверхностей — постоянная)
    u32 rb_mem;            // память блокировки цели рендера: заводится один раз (ReduceMouseLag — каждый кадр)
    int locked, own_mem;
    u32 lock_flags, lock_rect[4];
    u32 movie[4], has_movie; // кадр ролика в залоченной цели (x, y, w, h): растянуть при Unlock
    int movie_mode;        // прошлый Unlock был кадром ролика: Lock не читает цель из GPU
    GLuint movie_tex;      // постоянная текстура кадра ролика
    u32 movie_tw, movie_th;
    int backbuffer;        // неявный задний буфер устройства
} d3dsurf;

struct d3dtex {
    u32 obj;
    int cube;              // 0 — 2D, 1 — кубическая
    u32 w, h, levels, fmt, usage, pool;
    GLuint gl;
    d3dsurf **surf;        // [грань * levels + уровень]
    u32 lod;
    u32 filled;            // битовая маска загруженных уровней грани 0 (для дампа/отладки)
};

typedef struct d3dbuf {
    u32 obj;
    int index;             // 0 — вершинный, 1 — индексный
    u32 size, usage, fvf, fmt, pool;
    u32 mem;               // постоянная гостевая копия (Lock отдаёт указатель в неё)
    GLuint gl;
    u32 dirty_lo, dirty_hi;  // ещё не загруженный в GL диапазон
    int orphan;            // был Lock без NOOVERWRITE: при догрузке — новое хранилище GL (без ожидания GPU)
} d3dbuf;

d3dtex *d3d8_tex_create(u32 w, u32 h, u32 levels, u32 usage, u32 fmt, u32 pool, int cube);
d3dsurf *d3d8_surf_create(u32 w, u32 h, u32 fmt, u32 usage, u32 pool);  // самостоятельная поверхность
d3dbuf *d3d8_buf_create(int index, u32 size, u32 usage, u32 fvf_or_fmt, u32 pool);
d3dtex *d3d8_tex_from(u32 obj);    // гостевой объект -> текстура (NULL — не текстура)
d3dsurf *d3d8_surf_from(u32 obj);
d3dbuf *d3d8_buf_from(u32 obj);
void d3d8_buf_sync(d3dbuf *b);     // догрузить изменённое в GL
void d3d8_res_init(u32 device_obj);  // vtables ресурсов; GetDevice возвращает device_obj

// ---------------------------------------------------------------- устройство
#define MAX_STAGES 8
#define MAX_STREAMS 16
#define MAX_LIGHTS 8

typedef struct {
    u32 type;
    float diffuse[4], specular[4], ambient[4], pos[3], dir[3];
    float range, falloff, att0, att1, att2, theta, phi;
} d3dlight;  // D3DLIGHT8, 104 байта

typedef struct { u8 stream, reg, type, offset; } velem;  // reg — D3DVSDE_*, type — D3DVSDT_*
typedef struct {
    int n, rhw;
    velem e[16];
} vdecl;

typedef struct d3ddev {
    u32 obj;
    u32 rs[256];
    u32 tss[MAX_STAGES][32];
    float view[16], proj[16], world[16], texm[MAX_STAGES][16];
    float material[17];
    d3dlight lights[MAX_LIGHTS];
    u8 light_on[MAX_LIGHTS], light_set[MAX_LIGHTS];
    u32 vp[4];             // X, Y, Width, Height
    float vpz[2];          // MinZ, MaxZ
    float clip[6][4];
    d3dtex *tex[MAX_STAGES];
    d3dbuf *stream[MAX_STREAMS];
    u32 stride[MAX_STREAMS];
    d3dbuf *indices;
    u32 base_vertex;
    u32 vs, ps;            // FVF или дескриптор шейдера; дескриптор пиксельного шейдера
    float vsc[96][4], psc[8][4];
    d3dsurf *rt, *ds;      // текущие цели
    d3dsurf *backbuffer, *autodepth;
    u32 bb_w, bb_h, bb_fmt, ds_fmt, windowed;
    u32 hwnd;
    // генерации групп состояния: программа догружает униформы, если её копия устарела
    u32 gen_xf, gen_light, gen_mat, gen_misc;
    // GL
    GLuint vao, up_vbo, up_ibo, fbo, blit_fbo;
    d3dsurf *fbo_rt, *fbo_ds;  // что сейчас прикреплено к fbo
    GLuint samplers[MAX_STAGES];
    u32 sampler_dirty;
    int frame;
    u32 draws;  // за текущий кадр
} d3ddev;

d3ddev *d3d8_dev(void);  // единственное устройство (или NULL)
void d3d8_set_d3d_object(u32 obj);                  // IDirect3D8 для GetDirect3D
u32 d3d8_device_create(CPU *c, u32 hwnd, u32 pp);  // гостевой объект или 0
int d3d8_vdecl_for(d3ddev *d, vdecl *out);          // объявление текущего вершинного формата; 0 — нет

// d3d8_ff.c: отрисовка
void d3d8_draw(d3ddev *d, u32 prim, u32 count, int indexed, u32 first, u32 min_index, u32 num_vertices,
               const void *up_vertices, u32 up_stride, const void *up_indices, u32 up_index_fmt);
void d3d8_bind_target(d3ddev *d);  // FBO текущих rt/ds, viewport/scissor
extern u64 g_movie_drawn_ns;  // когда кадр ролика Bink последний раз залит в цель игрой (d3d8_res.c write_movie)
extern u64 g_movie_overlay_ns;  // когда порт последний раз сам нарисовал кадр ролика (d3d8_ff.c movie_overlay)
void d3d8_present(d3ddev *d);
void d3d8_caps_probe(d3ddev *d);
void d3d8_prog_preload(void);  // программы прошлых запусков — в фоновую сборку (d3d8_ff.c)
int d3d8_dumping(const d3ddev *d);  // кадр SNX_DUMP_DRAWS: печатать состояние отрисовок
void d3d8_dump_target(d3ddev *d);   // текущая цель рендера -> PNG (отладка)   // возможности GL (анизотропия) — после glGenSamplers

// d3d8_ps.c: пиксельные шейдеры ps.1.x
u32 d3d8_ps_create(const u32 *tokens, u32 max_tokens);  // дескриптор (>0) или 0
void d3d8_ps_delete(u32 handle);
const char *d3d8_ps_glsl(u32 handle);  // тело main() фрагментного шейдера (см. d3d8_ps.c)

#ifdef __cplusplus
}
#endif
