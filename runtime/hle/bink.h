// HLE binkw32: декодер звука Bink (дорожки роликов и звуки в пакетах) — bink_audio.c, видео — bink_video.c.
#pragma once
#include "runtime/abi.h"

typedef struct bink_audio bink_audio;
// rate — частота дорожки, channels — 1/2, dct — флаг 0x1000 (иначе RDFT), revision — 4-й байт сигнатуры ('i').
// NULL — вариант не поддержан (RDFT и ревизия 'b' в данных игры не встречаются)
bink_audio *bink_audio_new(u32 rate, u32 channels, int dct, int revision);
void bink_audio_free(bink_audio *a);
// забыть хвост предыдущего блока (переход не на следующий кадр)
void bink_audio_reset(bink_audio *a);
// Пакет кадра без первого dword (размер PCM) -> не больше max сэмплов s16 (с чередованием каналов); сколько
// записано. 0 — декодирования нет (вызывающий дополняет тишиной).
u32 bink_audio_decode(bink_audio *a, const u8 *packet, u32 bytes, s16 *out, u32 max);

// Идёт ролик: видео Bink декодировалось меньше 0,3 с назад (сенсорный экран: кнопка «Пропустить», касание —
// пропуск; host/boot/input.c)
int bink_movie_playing(void);

typedef struct bink_video bink_video;
// signature — первые 4 байта файла ("BIKi"), video_flags — dword заголовка по смещению 36 (альфа и т. п.)
bink_video *bink_video_new(u32 signature, u32 video_flags, u32 w, u32 h);
void bink_video_free(bink_video *v);
void bink_video_flush(bink_video *v);  // перед декодированием не следующего кадра (с ключевого)
int bink_video_decode(bink_video *v, const u8 *packet, u32 size, int key);  // 0 — кадр готов
// последний декодированный кадр -> поверхность (тип BINKSURFACE* в младших 4 битах flags), с (x, y), не больше rows строк
void bink_video_copy(bink_video *v, u8 *dst, u32 pitch, u32 rows, u32 x, u32 y, u32 flags);
