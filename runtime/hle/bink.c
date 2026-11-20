// HLE binkw32 (RAD Bink 1.x): ролики (Movies/*.bik) и звук в формате Bink внутри пакетов (диалоги — Engine
// перегоняет их в WAV через BinkGetTrackData). Контейнер разбирается полностью: кадры, частота, дорожки,
// размеры PCM каждого кадра (первый dword аудиопакета). Декодирование звука — bink_audio.c (если нет —
// тишина той же длины; у звуков в пакетах весь PCM — в кадре 0, у роликов — покадрово, декодированное
// совпадает с заявленным до последнего кадра, где хвост блока лишний). Видео — bink_video.c (libavcodec):
// BinkDoFrame декодирует текущий кадр (пропущенные — догоняются от ключевого), BinkCopyToBuffer переводит
// его в формат поверхности; время и номера кадров — как у SDK.
// Структуры SDK в гостевой памяти: BINK (Width, Height, Frames, FrameNum (с 1), LastFrameNum, FrameRate,
// FrameRateDiv, ...), BINKTRACK (Frequency, Bits, Channels, MaxSize, bink, ...), BINKSUMMARY.
#include <stdio.h>
#include <stdlib.h>

#include "runtime/heap.h"
#include "runtime/hle/bink.h"
#include "runtime/hle/d3d8.h"
#include "runtime/hle/hle.h"
#include "runtime/hle/vfs.h"

#define BINKFROMMEMORY 0x04000000u
#define MAX_BINK 64

typedef struct {
    u32 obj;               // гостевой BINK
    u8 *data;              // копия файла
    u32 size;
    u32 frames, w, h, fps, fpsdiv, ntracks;
    u32 rate[4], flags[4], maxsize[4];
    u32 *offs;             // frames + 1 смещений
    u8 *key;               // ключевые кадры (бит 0 смещения)
    u32 frame;             // текущий, с 0
    u32 tracks[4];         // гостевые BINKTRACK
    bink_audio *dec[4];
    u32 dec_next[4];       // кадр, который декодер ждёт следующим (иначе — сброс хвоста перекрытия)
    bink_video *vid;
    u32 vid_frame;         // последний декодированный кадр видео (~0 — нет)
} bink;

static bink *g_bink[MAX_BINK];

static bink *by_obj(u32 obj)
{
    for (int i = 0; i < MAX_BINK; i++)
        if (g_bink[i] && g_bink[i]->obj == obj)
            return g_bink[i];
    return NULL;
}

static bink *by_track(u32 tr, int *index)
{
    for (int i = 0; i < MAX_BINK; i++)
        for (int t = 0; g_bink[i] && t < 4; t++)
            if (g_bink[i]->tracks[t] == tr) {
                *index = t;
                return g_bink[i];
            }
    return NULL;
}

static u32 le32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }

static void sync_obj(bink *b)
{
    wr32(g_mem, b->obj + 12, b->frame + 1);  // FrameNum
}

static bink *parse(u8 *d, u32 size)
{
    if (size < 44 || memcmp(d, "BIK", 3))
        return NULL;
    bink *b = calloc(1, sizeof *b);
    b->data = d;
    b->size = size;
    b->frames = le32(d + 8);
    b->w = le32(d + 20);
    b->h = le32(d + 24);
    b->fps = le32(d + 28);
    b->fpsdiv = le32(d + 32);
    b->ntracks = le32(d + 40);
    if (b->ntracks > 4 || !b->frames || 44 + b->ntracks * 12 + (b->frames + 1) * 4 > size) {
        free(b);
        return NULL;
    }
    u32 o = 44;
    for (u32 t = 0; t < b->ntracks; t++, o += 4)
        b->maxsize[t] = le32(d + o);
    for (u32 t = 0; t < b->ntracks; t++, o += 4) {
        b->rate[t] = (u32)d[o] | (u32)d[o + 1] << 8;
        b->flags[t] = (u32)d[o + 2] | (u32)d[o + 3] << 8;
    }
    o += 4 * b->ntracks;  // идентификаторы дорожек
    b->offs = malloc((b->frames + 1) * 4);
    b->key = malloc(b->frames + 1);
    for (u32 i = 0; i <= b->frames; i++) {
        b->offs[i] = le32(d + o + 4 * i) & ~1u;
        b->key[i] = (u8)(le32(d + o + 4 * i) & 1);
    }
    b->key[0] = 1;
    if (!b->fpsdiv)
        b->fpsdiv = 1;
    return b;
}

// Аудиопакет дорожки t в кадре f: указатель и размер (NULL — нет)
static const u8 *audio_packet(bink *b, u32 f, u32 t, u32 *len)
{
    if (f >= b->frames)
        return NULL;
    u32 p = b->offs[f], end = b->offs[f + 1];
    for (u32 i = 0; i < b->ntracks; i++) {
        if (p + 4 > end || p + 4 > b->size)
            return NULL;
        u32 n = le32(b->data + p);
        if (i == t) {
            if (p + 4 + n > b->size)
                return NULL;
            *len = n;
            return b->data + p + 4;
        }
        p += 4 + n;
    }
    return NULL;
}

// Видеопакет кадра f: всё после аудиопакетов дорожек
static const u8 *video_packet(bink *b, u32 f, u32 *len)
{
    u32 p = b->offs[f], end = b->offs[f + 1];
    for (u32 i = 0; i < b->ntracks && p + 4 <= end; i++)
        p += 4 + le32(b->data + p);
    if (p > end || end > b->size)
        return NULL;
    *len = end - p;
    return b->data + p;
}

// Декодировать кадр f; если он не следующий — с ближайшего ключевого (или с последнего декодированного)
static void video_decode_to(bink *b, u32 f)
{
    if (!b->vid || f >= b->frames || f == b->vid_frame)
        return;
    u32 start = f;
    while (start > 0 && !b->key[start] && start != b->vid_frame + 1)
        start--;
    if (start != b->vid_frame + 1)
        bink_video_flush(b->vid);
    for (u32 i = start; i <= f; i++) {
        u32 n;
        const u8 *p = video_packet(b, i, &n);
        if (p && n)
            bink_video_decode(b->vid, p, n, b->key[i]);
    }
    b->vid_frame = f;
}

// BinkOpen(name, flags): имя файла гостя или (BINKFROMMEMORY) указатель на данные
void hle_binkw32__BinkOpen_8(CPU *c)
{
    HLE_ENTER("BinkOpen", 2);
    u32 src = ARG(0), flags = ARG(1);
    u8 *d = NULL;
    u32 size = 0;
    if (flags & BINKFROMMEMORY) {
        size = le32(g_mem + src + 4) + 8;
        if (memcmp(g_mem + src, "BIK", 3) || size > 256u << 20)
            RET(2, 0);
        d = malloc(size);
        memcpy(d, g_mem + src, size);
    } else {
        char rel[512], host[1024];
        const char *name = hle_cstr(src);
        if (!vfs_rel(name, rel, sizeof rel) || !vfs_find(rel, host, sizeof host)) {
            fprintf(stderr, "[bink] BinkOpen(%s): not found\n", name);
            RET(2, 0);
        }
        FILE *f = fopen(host, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            size = (u32)ftell(f);
            fseek(f, 0, SEEK_SET);
            d = malloc(size);
            if (fread(d, 1, size, f) != size)
                size = 0;
            fclose(f);
        }
        fprintf(stderr, "[bink] BinkOpen(%s) %u bytes\n", name, size);
    }
    bink *b = d ? parse(d, size) : NULL;
    int slot = -1;
    for (int i = 0; i < MAX_BINK && slot < 0; i++)
        if (!g_bink[i])
            slot = i;
    if (!b || slot < 0) {
        free(b);
        free(d);
        RET(2, 0);
    }
    b->obj = heap_alloc(heap_process(), HEAP_ZERO_MEMORY, 0x200);
    u32 o = b->obj;
    wr32(g_mem, o + 0, b->w);
    wr32(g_mem, o + 4, b->h);
    wr32(g_mem, o + 8, b->frames);
    wr32(g_mem, o + 16, 0);  // LastFrameNum
    wr32(g_mem, o + 20, b->fps);
    wr32(g_mem, o + 24, b->fpsdiv);
    wr32(g_mem, o + 32, flags);
    wr32(g_mem, o + 40, size);
    g_bink[slot] = b;
    b->vid_frame = ~0u;
    if (b->w && b->h) {
        b->vid = bink_video_new(le32(d), le32(d + 36), b->w, b->h);
        if (!b->vid)
            fprintf(stderr, "[bink] no video decoder: frames will be black\n");
    }
    sync_obj(b);
    RET(2, b->obj);
}

void hle_binkw32__BinkClose_4(CPU *c)
{
    HLE_ENTER("BinkClose", 1);
    for (int i = 0; i < MAX_BINK; i++) {
        bink *b = g_bink[i];
        if (b && b->obj == ARG(0)) {
            for (int t = 0; t < 4; t++) {
                if (b->tracks[t])
                    heap_free(heap_process(), b->tracks[t]);
                bink_audio_free(b->dec[t]);
            }
            heap_free(heap_process(), b->obj);
            bink_video_free(b->vid);
            free(b->key);
            free(b->offs);
            free(b->data);
            free(b);
            g_bink[i] = NULL;
        }
    }
    RET(1, 0);
}

// BinkOpenTrack(bink, trackindex) -> BINKTRACK: Frequency, Bits, Channels, MaxSize, bink, sndcomp, trackindex
void hle_binkw32__BinkOpenTrack_8(CPU *c)
{
    HLE_ENTER("BinkOpenTrack", 2);
    bink *b = by_obj(ARG(0));
    u32 t = ARG(1);
    if (!b || t >= b->ntracks)
        RET(2, 0);
    if (!b->tracks[t]) {
        u32 tr = heap_alloc(heap_process(), HEAP_ZERO_MEMORY, 32);
        u32 ch = (b->flags[t] & 0x2000) ? 2 : 1, bits = (b->flags[t] & 0x4000) ? 16 : 8;
        wr32(g_mem, tr, b->rate[t]);
        wr32(g_mem, tr + 4, bits);
        wr32(g_mem, tr + 8, ch);
        // MaxSize: наибольший PCM кадра (заголовок даёт его не всегда точно — проверим все кадры)
        u32 mx = b->maxsize[t];
        for (u32 f = 0; f < b->frames; f++) {
            u32 n;
            const u8 *p = audio_packet(b, f, t, &n);
            if (p && n >= 4 && le32(p) > mx)
                mx = le32(p);
        }
        wr32(g_mem, tr + 12, mx);
        wr32(g_mem, tr + 16, b->obj);
        wr32(g_mem, tr + 24, t);
        b->tracks[t] = tr;
        b->dec[t] = bink_audio_new(b->rate[t], ch, (b->flags[t] & 0x1000) != 0, b->data[3]);
    }
    RET(2, b->tracks[t]);
}

void hle_binkw32__BinkCloseTrack_4(CPU *c)
{
    HLE_ENTER("BinkCloseTrack", 1);
    RET(1, 0);  // освобождается вместе с BINK
}

// BinkGetTrackData(track, dest): PCM текущего кадра; байт записано
void hle_binkw32__BinkGetTrackData_8(CPU *c)
{
    HLE_ENTER("BinkGetTrackData", 2);
    int t;
    bink *b = by_track(ARG(0), &t);
    u32 n, dst = ARG(1);
    const u8 *p = b ? audio_packet(b, b->frame, (u32)t, &n) : NULL;
    if (!p || n < 4)
        RET(2, 0);
    u32 bytes = le32(p);
    u32 cap = rd32(g_mem, ARG(0) + 12);
    if (bytes > cap)
        bytes = cap;
    if (b->frame != b->dec_next[t])
        bink_audio_reset(b->dec[t]);
    b->dec_next[t] = b->frame + 1;
    u32 got, bits8 = rd32(g_mem, ARG(0) + 4) == 8;
    if (bits8) {  // 8 бит без знака
        s16 *tmp = malloc(bytes * 2 + 2);
        got = bink_audio_decode(b->dec[t], p + 4, n - 4, tmp, bytes);
        for (u32 i = 0; i < got; i++)
            g_mem[dst + i] = (u8)((tmp[i] >> 8) + 128);
        free(tmp);
    } else
        got = bink_audio_decode(b->dec[t], p + 4, n - 4, (s16 *)(g_mem + dst), bytes / 2) * 2;
    if (got < bytes)
        memset(g_mem + dst + got, bits8 ? 0x80 : 0, bytes - got);
    RET(2, bytes);
}

static _Atomic u64 g_movie_ns;  // последнее декодирование кадра видео (bink_movie_playing)

int bink_movie_playing(void)
{
    u64 t = g_movie_ns;
    return t && rt_ticks_ns() - t < 300000000ull;
}

void hle_binkw32__BinkDoFrame_4(CPU *c)
{
    HLE_ENTER("BinkDoFrame", 1);
    bink *b = by_obj(ARG(0));
    if (b && b->vid)
        g_movie_ns = rt_ticks_ns();
    if (b)
        video_decode_to(b, b->frame);
    RET(1, 0);
}

void hle_binkw32__BinkNextFrame_4(CPU *c)
{
    HLE_ENTER("BinkNextFrame", 1);
    bink *b = by_obj(ARG(0));
    if (b) {
        wr32(g_mem, b->obj + 16, b->frame + 1);  // LastFrameNum
        b->frame = b->frame + 1 < b->frames ? b->frame + 1 : 0;  // как SDK: после последнего — первый
        sync_obj(b);
    }
    RET(1, 0);
}

// BinkCopyToBuffer(bink, dest, destpitch, destheight, destx, desty, flags): последний декодированный кадр
// (без декодера — чёрный)
void hle_binkw32__BinkCopyToBuffer_28(CPU *c)
{
    HLE_ENTER("BinkCopyToBuffer", 7);
    bink *b = by_obj(ARG(0));
    if (b) {
        u32 pitch = ARG(2), rows = ARG(3) < b->h ? ARG(3) : b->h;
        if (b->vid && b->vid_frame != ~0u) {
            bink_video_copy(b->vid, g_mem + ARG(1), pitch, rows, ARG(4), ARG(5), ARG(6));
            d3d8_movie_frame(ARG(1), pitch, ARG(4), ARG(5), b->w, rows);
        }
        else
            for (u32 y = 0; y < rows; y++)
                memset(g_mem + ARG(1) + (ARG(5) + y) * pitch, 0, pitch);
    }
    RET(7, 0);
}

void hle_binkw32__BinkGoto_12(CPU *c)
{
    HLE_ENTER("BinkGoto", 3);
    bink *b = by_obj(ARG(0));
    if (b && ARG(1) >= 1 && ARG(1) <= b->frames) {
        b->frame = ARG(1) - 1;
        sync_obj(b);
    }
    RET(3, 0);
}

void hle_binkw32__BinkPause_8(CPU *c)
{
    HLE_ENTER("BinkPause", 2);
    RET(2, ARG(1));
}

// BINKSUMMARY: Width, Height, TotalTime (мс), FileFrameRate, FileFrameRateDiv, FrameRate, FrameRateDiv,
// TotalOpenTime, TotalFrames, TotalPlayedFrames, ...
void hle_binkw32__BinkGetSummary_8(CPU *c)
{
    HLE_ENTER("BinkGetSummary", 2);
    bink *b = by_obj(ARG(0));
    u32 s = ARG(1);
    memset(g_mem + s, 0, 0x60);
    if (b) {
        wr32(g_mem, s, b->w);
        wr32(g_mem, s + 4, b->h);
        wr32(g_mem, s + 8, (u32)((u64)b->frames * b->fpsdiv * 1000 / (b->fps ? b->fps : 1)));
        wr32(g_mem, s + 12, b->fps);
        wr32(g_mem, s + 16, b->fpsdiv);
        wr32(g_mem, s + 20, b->fps);
        wr32(g_mem, s + 24, b->fpsdiv);
        wr32(g_mem, s + 32, b->frames);
        wr32(g_mem, s + 36, b->frame + 1);
    }
    RET(2, 0);
}
