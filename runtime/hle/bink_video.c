// Видео Bink: кадры декодирует libavcodec (декодер bink), здесь — подача пакетов и перевод YUV 4:2:0
// (ограниченный диапазон, BT.601; у файлов с альфой — плоскость A) в формат поверхности BinkCopyToBuffer.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

#include <libavcodec/avcodec.h>
#include <libavutil/mem.h>

#include "runtime/hle/bink.h"

// Декодирование — в своём потоке на ролик: кадр, следующий за показанным, декодируется заранее
// (bink_video_prefetch), пока игра выводит текущий; bink_video_decode того же пакета только забирает готовый кадр.
// Контекст декодера в каждый момент у одного потока: основной ждёт фоновое задание перед любым своим вызовом
// libavcodec. На полноэкранном ролике с движением (реклама зелья Крёстной Феи, 800x600) декодирование на телефоне —
// десятки мс на кадр, и игра в том же потоке шла по 10 кадров/с.
struct bink_video {
    AVCodecContext *ctx;
    AVPacket *pkt;
    AVFrame *frame;  // показываемый кадр (bink_video_copy)
    AVFrame *next;   // кадр фонового задания
    int have;        // в frame есть декодированный кадр
    pthread_t th;
    int th_ok;
    pthread_mutex_t m;
    pthread_cond_t cv;
    int job;  // 0 — нет, 1 — ждёт, 2 — идёт, 3 — готово
    int quit;
    const u8 *jdata;
    u32 jsize;
    int jkey, jres;
    u64 jns;  // время фонового декодирования
    u64 dec_ns, dec_n, wait_ns;  // статистика для bink.c (мс на кадр)
};

static u64 now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
}

// декодировать пакет в out (контекст — у вызывающего потока)
static int decode_into(bink_video *v, AVPacket *pkt, AVFrame *out, const u8 *data, u32 size, int key)
{
    if (av_new_packet(pkt, (int)size) < 0)
        return -1;
    memcpy(pkt->data, data, size);
    if (key)
        pkt->flags |= AV_PKT_FLAG_KEY;
    int r = avcodec_send_packet(v->ctx, pkt);
    av_packet_unref(pkt);
    if (r >= 0) {
        av_frame_unref(out);
        r = avcodec_receive_frame(v->ctx, out);
    }
    return r;
}

static void *worker(void *arg)
{
    bink_video *v = arg;
    AVPacket *pkt = av_packet_alloc();
    pthread_mutex_lock(&v->m);
    for (;;) {
        while (!v->quit && v->job != 1)
            pthread_cond_wait(&v->cv, &v->m);
        if (v->quit)
            break;
        v->job = 2;
        const u8 *d = v->jdata;
        u32 n = v->jsize;
        int key = v->jkey;
        pthread_mutex_unlock(&v->m);
        u64 t0 = now_ns();
        int r = pkt ? decode_into(v, pkt, v->next, d, n, key) : -1;
        u64 dt = now_ns() - t0;
        pthread_mutex_lock(&v->m);
        v->jres = r;
        v->jns = dt;
        v->job = 3;
        pthread_cond_broadcast(&v->cv);
    }
    pthread_mutex_unlock(&v->m);
    av_packet_free(&pkt);
    return NULL;
}

// дождаться фонового задания (под v->m); -> 1, если готов кадр пакета (data, size)
static int wait_job(bink_video *v, const u8 *data, u32 size)
{
    u64 t0 = 0;
    while (v->job == 1 || v->job == 2) {
        if (!t0)
            t0 = now_ns();
        pthread_cond_wait(&v->cv, &v->m);
    }
    if (t0)
        v->wait_ns += now_ns() - t0;
    if (v->job != 3)
        return 0;
    v->job = 0;
    return v->jdata == data && v->jsize == size;
}

bink_video *bink_video_new(u32 signature, u32 video_flags, u32 w, u32 h)
{
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_BINKVIDEO);
    if (!codec)
        return NULL;
    bink_video *v = calloc(1, sizeof *v);
    v->ctx = avcodec_alloc_context3(codec);
    v->ctx->width = (int)w;
    v->ctx->height = (int)h;
    v->ctx->codec_tag = signature;  // 4-й байт — ревизия формата
    v->ctx->extradata = av_mallocz(4 + AV_INPUT_BUFFER_PADDING_SIZE);
    memcpy(v->ctx->extradata, &video_flags, 4);
    v->ctx->extradata_size = 4;
    v->ctx->thread_count = 1;
    if (avcodec_open2(v->ctx, codec, NULL) < 0) {
        avcodec_free_context(&v->ctx);
        free(v);
        return NULL;
    }
    v->pkt = av_packet_alloc();
    v->frame = av_frame_alloc();
    v->next = av_frame_alloc();
    pthread_mutex_init(&v->m, NULL);
    pthread_cond_init(&v->cv, NULL);
    v->th_ok = pthread_create(&v->th, NULL, worker, v) == 0;
    return v;
}

void bink_video_free(bink_video *v)
{
    if (!v)
        return;
    if (v->th_ok) {
        pthread_mutex_lock(&v->m);
        v->quit = 1;
        pthread_cond_broadcast(&v->cv);
        pthread_mutex_unlock(&v->m);
        pthread_join(v->th, NULL);
    }
    pthread_cond_destroy(&v->cv);
    pthread_mutex_destroy(&v->m);
    av_frame_free(&v->next);
    av_frame_free(&v->frame);
    av_packet_free(&v->pkt);
    avcodec_free_context(&v->ctx);
    free(v);
}

void bink_video_flush(bink_video *v)
{
    if (!v)
        return;
    pthread_mutex_lock(&v->m);
    wait_job(v, NULL, 0);  // заранее декодированный кадр больше не нужен
    pthread_mutex_unlock(&v->m);
    avcodec_flush_buffers(v->ctx);
}

int bink_video_decode(bink_video *v, const u8 *data, u32 size, int key)
{
    if (!v)
        return -1;
    pthread_mutex_lock(&v->m);
    int ready = wait_job(v, data, size);
    int r = v->jres;
    u64 dt = v->jns;
    pthread_mutex_unlock(&v->m);
    if (ready) {  // этот пакет уже декодирован заранее
        AVFrame *t = v->frame;
        v->frame = v->next;
        v->next = t;
    } else {
        u64 t0 = now_ns();
        r = decode_into(v, v->pkt, v->frame, data, size, key);
        dt = now_ns() - t0;
    }
    v->dec_ns += dt;
    v->dec_n++;
    if (r < 0) {
        static int reported;
        if (reported++ < 4)
            fprintf(stderr, "[bink] video decode error %d (%u bytes)\n", r, size);
        if (ready)
            v->have = 0;  // показываемый кадр отдан заданию
        return r;
    }
    v->have = 1;
    return 0;
}

void bink_video_prefetch(bink_video *v, const u8 *data, u32 size, int key)
{
    if (!v || !v->th_ok || !data || !size)
        return;
    pthread_mutex_lock(&v->m);
    if (!v->job) {
        v->jdata = data;
        v->jsize = size;
        v->jkey = key;
        v->job = 1;
        pthread_cond_broadcast(&v->cv);
    }
    pthread_mutex_unlock(&v->m);
}

void bink_video_stats(bink_video *v, double *dec_ms, double *wait_ms, u32 *frames)
{
    *dec_ms = *wait_ms = 0;
    *frames = 0;
    if (!v)
        return;
    pthread_mutex_lock(&v->m);
    *frames = (u32)v->dec_n;
    if (v->dec_n) {
        *dec_ms = (double)v->dec_ns / 1e6 / (double)v->dec_n;
        *wait_ms = (double)v->wait_ns / 1e6 / (double)v->dec_n;
    }
    v->dec_ns = v->dec_n = v->wait_ns = 0;
    pthread_mutex_unlock(&v->m);
}

static u8 clamp8(int x) { return (u8)(x < 0 ? 0 : x > 255 ? 255 : x); }

// таблицы BT.601 (ограниченный диапазон): Y -> 298*(Y-16), U/V -> вклады в R, G, B (с округлением +128)
static int g_ty[256], g_rv[256], g_gu[256], g_gv[256], g_bu[256];
static pthread_once_t g_tab_once = PTHREAD_ONCE_INIT;

static void tab_init(void)
{
    for (int i = 0; i < 256; i++) {
        g_ty[i] = (i - 16) * 298 + 128;
        g_rv[i] = 409 * (i - 128);
        g_gu[i] = -100 * (i - 128);
        g_gv[i] = -208 * (i - 128);
        g_bu[i] = 516 * (i - 128);
    }
}

// Типы поверхностей bink.h: 1 BINKSURFACE24 (B,G,R), 2 24R (R,G,B), 3 32 (B,G,R,x), 4 32R (R,G,B,x),
// 5 32A (B,G,R,A), 6 32RA (R,G,B,A), 7 4444, 8 5551, 9 555, 10 565 (11 655 и 12 664 — как 565).
// Цикл — свой на каждую раскладку (без выбора на каждый пиксель), цвет — по таблицам: ролик 800x600 переводится
// за 1–2 мс вместо 5–8.
void bink_video_copy(bink_video *v, u8 *dst, u32 pitch, u32 rows, u32 x0, u32 y0, u32 surface)
{
    if (!v || !v->have)
        return;
    pthread_once(&g_tab_once, tab_init);
    const AVFrame *f = v->frame;
    int w = f->width, h = f->height;
    if ((u32)h > rows)
        h = (int)rows;
    const u8 *ap = f->format == AV_PIX_FMT_YUVA420P ? f->data[3] : NULL;
    u32 type = surface & 15, bpp = type == 1 || type == 2 ? 3 : type >= 7 && type <= 12 ? 2 : 4;
    if (x0 * bpp >= pitch)
        return;
    if ((u32)w > (pitch - x0 * bpp) / bpp)  // не шире строки поверхности
        w = (int)((pitch - x0 * bpp) / bpp);
    int rgb_order = type == 2 || type == 4 || type == 6;  // R,G,B в памяти (иначе B,G,R)
    for (int y = 0; y < h; y++) {
        const u8 *yp = f->data[0] + y * f->linesize[0];
        const u8 *up = f->data[1] + (y >> 1) * f->linesize[1];
        const u8 *vp = f->data[2] + (y >> 1) * f->linesize[2];
        const u8 *al = ap ? ap + y * f->linesize[3] : NULL;
        u8 *o = dst + (y0 + (u32)y) * pitch + x0 * bpp;
#define YUV(x)                                                                         \
    int c = g_ty[yp[x]], rv = g_rv[vp[(x) >> 1]], gg = g_gu[up[(x) >> 1]] + g_gv[vp[(x) >> 1]], \
        bu = g_bu[up[(x) >> 1]];                                                       \
    u8 r = clamp8((c + rv) >> 8), g = clamp8((c + gg) >> 8), b = clamp8((c + bu) >> 8)
        if (bpp == 4) {
            if (rgb_order)
                for (int x = 0; x < w; x++, o += 4) {
                    YUV(x);
                    o[0] = r, o[1] = g, o[2] = b, o[3] = al ? al[x] : 255;
                }
            else
                for (int x = 0; x < w; x++, o += 4) {
                    YUV(x);
                    o[0] = b, o[1] = g, o[2] = r, o[3] = al ? al[x] : 255;
                }
        } else if (bpp == 3) {
            for (int x = 0; x < w; x++, o += 3) {
                YUV(x);
                if (rgb_order)
                    o[0] = r, o[1] = g, o[2] = b;
                else
                    o[0] = b, o[1] = g, o[2] = r;
            }
        } else {
            for (int x = 0; x < w; x++, o += 2) {
                YUV(x);
                u8 a = al ? al[x] : 255;
                u16 px;
                switch (type) {
                case 9: px = (u16)((r >> 3) << 10 | (g >> 3) << 5 | b >> 3); break;
                case 8: px = (u16)((a >> 7) << 15 | (r >> 3) << 10 | (g >> 3) << 5 | b >> 3); break;
                case 7: px = (u16)((a >> 4) << 12 | (r >> 4) << 8 | (g >> 4) << 4 | b >> 4); break;
                default: px = (u16)((r >> 3) << 11 | (g >> 2) << 5 | b >> 3); break;
                }
                memcpy(o, &px, 2);
            }
        }
#undef YUV
    }
}
