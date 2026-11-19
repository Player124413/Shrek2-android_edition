// Макросы и хелперы, которые использует сгенерированный рекомпилятором C.
#pragma once

// Закреплённые регистры (SNX_PIN, Switch/GCC; runtime/abi.h): регистры гостя — глобальные регистровые переменные,
// общие для всех функций гостя, а не локальные копии из CPU. Объявление — раньше любого определения функции в
// файле (иначе GCC — ошибка), поэтому до include и на встроенных типах. Порядок — как в трамплине rt_pin_enter
// (runtime/rt.c). G_(...) — обмен с CPU, нужный только без закрепления (вход/выход функции, вокруг вызова
// функции гостя), P_(...) — только с ним (вокруг вызова рантайма, который зовёт код гостя сам).
#ifdef SNX_PIN
register unsigned int eax __asm__("x19");
register unsigned int ecx __asm__("x20");
register unsigned int edx __asm__("x21");
register unsigned int ebx __asm__("x22");
register unsigned long long esp __asm__("x23");
register unsigned int ebp __asm__("x24");
register unsigned int esi __asm__("x25");
register unsigned int edi __asm__("x26");
register unsigned char *m __asm__("x27");
#define G_(...)
#define P_(...) __VA_ARGS__
#else
#define G_(...) __VA_ARGS__
#define P_(...)
#endif

#include "runtime/crt_math.h"

// Доступ к памяти гостя по указателю хоста: m + адрес (recomp/emit.py: ha). [рег ± поле] — m + рег ± d без
// заворота через 2^32, чтобы m + рег считался раз на значение регистра.
static inline u16 ld16(const u8 *p) { u16 v; memcpy(&v, p, 2); return v; }
static inline u32 ld32(const u8 *p) { u32 v; memcpy(&v, p, 4); return v; }
static inline u64 ld64(const u8 *p) { u64 v; memcpy(&v, p, 8); return v; }
static inline float ldf32(const u8 *p) { float v; memcpy(&v, p, 4); return v; }
static inline double ldf64(const u8 *p) { double v; memcpy(&v, p, 8); return v; }
static inline void st16(u8 *p, u16 v) { memcpy(p, &v, 2); }
static inline void st32(u8 *p, u32 v) { memcpy(p, &v, 4); }
static inline void st64(u8 *p, u64 v) { memcpy(p, &v, 8); }
static inline void stf32(u8 *p, float v) { memcpy(p, &v, 4); }
static inline void stf64(u8 *p, double v) { memcpy(p, &v, 8); }

// rep movs/stos (recomp/emit.py i_string, регистры после — там же). Длина-константа (копия/обнуление структуры:
// mov ecx, N; rep movsd) — на месте, по элементу, как сам rep; иначе рантайм
#define REP_INLINE_MAX 16
static inline __attribute__((always_inline)) void REP_MOVS(u8 *m, u32 s, u32 d, u32 n, int size, int df)
{
    if (__builtin_constant_p(n) && __builtin_constant_p(df) && !df && n <= REP_INLINE_MAX) {
        for (u32 i = 0; i < n; i++, s += (u32)size, d += (u32)size)
            size == 4 ? st32(m + d, ld32(m + s)) : size == 2 ? st16(m + d, ld16(m + s)) : (void)(m[d] = m[s]);
    } else {
        rt_rep_movs(m, s, d, n, size, df);
    }
}
static inline __attribute__((always_inline)) void REP_STOS(u8 *m, u32 d, u32 n, u32 v, int size, int df)
{
    if (__builtin_constant_p(n) && __builtin_constant_p(df) && !df && n <= REP_INLINE_MAX) {
        for (u32 i = 0; i < n; i++, d += (u32)size)
            size == 4 ? st32(m + d, v) : size == 2 ? st16(m + d, (u16)v) : (void)(m[d] = (u8)v);
    } else {
        rt_rep_stos(m, d, n, v, size, df);
    }
}

// fist/fistp (FIST) и fisttp/_ftol (FTRUNC) — на месте, как rt_fist/rt_ftrunc (runtime/rt.c): вне диапазона и NaN —
// «неопределённое целое». FIST при округлении не к ближайшему (fldcw гостя) — рантайм. Диапазон — через &, а не &&:
// одна проверка (fccmpe), и GCC ставит обычный случай на прямой путь
static inline __attribute__((always_inline)) s64 FIST(CPU *restrict c, double v, int bits)
{
    if (__builtin_expect(!(c->fpucw & 0xC00u), 1)) {
        double r = __builtin_nearbyint(v);  // к ближайшему чётному, как хост по умолчанию
        if (bits == 16)
            return __builtin_expect((r >= -32768.0) & (r <= 32767.0), 1) ? (s64)(s32)r : (s64)(s16)0x8000;
        if (bits == 32)
            return __builtin_expect((r >= -2147483648.0) & (r <= 2147483647.0), 1) ? (s64)(s32)r
                                                                                  : (s64)(s32)0x80000000u;
        return __builtin_expect((r >= -9223372036854775808.0) & (r < 9223372036854775808.0), 1) ? (s64)r : INT64_MIN;
    }
    return rt_fist(c, v, bits);
}
static inline __attribute__((always_inline)) s64 FTRUNC(double v, int bits)
{
    if (bits == 16)
        return __builtin_expect((v > -32769.0) & (v < 32768.0), 1) ? (s64)(s32)v : (s64)(s16)0x8000;
    if (bits == 32)
        return __builtin_expect((v > -2147483649.0) & (v < 2147483648.0), 1) ? (s64)(s32)v : (s64)(s32)0x80000000u;
    return __builtin_expect((v >= -9223372036854775808.0) & (v < 9223372036854775808.0), 1) ? (s64)v : INT64_MIN;
}

#define SPILL() (c->eax = eax, c->ecx = ecx, c->edx = edx, c->ebx = ebx, c->esp = esp, c->ebp = ebp, \
                 c->esi = esi, c->edi = edi)
#define RELOAD() (eax = c->eax, ecx = c->ecx, edx = c->edx, ebx = c->ebx, esp = c->esp, ebp = c->ebp, \
                  esi = c->esi, edi = c->edi)

#ifdef SNX_PIN
// кеш косвенного вызова (abi.h) с закреплёнными регистрами: функция гостя — прямо с места вызова; функция хоста
// (бит 0 смещения) и промах — в rt_icall_slow, одной копии на файл (с SPILL/RELOAD на каждом месте функции D3DDrv с
// десятками вызовов COM вырастали на треть). Промах места сначала смотрит общий кеш rt_ic2: функция гостя оттуда —
// тоже прямым вызовом, без CPU и трамплина rt_pin_enter (диспетчер байт-кода UnrealScript промахивается почти
// всегда); с регистрами через CPU — только функция хоста и промах общего кеша (rt_call_ic)
static __attribute__((noinline)) void rt_icall_slow(CPU *restrict c, u32 t, u64 *ic, u64 v)
{
    if ((u32)v != t) {
        v = __atomic_load_n(&rt_ic2[RT_IC2_IDX(t)], __ATOMIC_RELAXED);
        if ((u32)v != t || !t) {
            SPILL();
            rt_call_ic(c, t, ic);
            RELOAD();
            return;
        }
        __atomic_store_n(ic, v, __ATOMIC_RELAXED);
    }
    s32 o = (s32)(u32)(v >> 32);
    guest_fn fn = (guest_fn)((uintptr_t)rt_ic_base + (uintptr_t)(intptr_t)(o & ~1));
    if (o & 1) {
        SPILL();
        fn(c);
        RELOAD();
    } else {
        fn(c);
    }
}
#undef RT_ICALL
#define RT_ICALL(c, t)                                                                                     \
    do {                                                                                                   \
        static u64 ic_;                                                                                    \
        u64 v_ = __atomic_load_n(&ic_, __ATOMIC_RELAXED);                                                  \
        s32 o_ = (s32)(u32)(v_ >> 32);                                                                     \
        if ((u32)v_ == (t) && !(o_ & 1))                                                                   \
            ((guest_fn)((uintptr_t)rt_ic_base + (uintptr_t)(intptr_t)o_))(c);                              \
        else                                                                                               \
            rt_icall_slow((c), (t), &ic_, v_);                                                             \
    } while (0)
#endif

// setjmp гостя (см. recomp/gen.py SETJMP_EXPORTS): хостовый setjmp — прямо в кадре вызывающей функции
#if defined(__APPLE__) || defined(__ANDROID__)
#define RT_SETJMP(j) _setjmp((j)->buf)  // без сохранения маски сигналов (setjmp на маке и в bionic — системный вызов)
#else
#define RT_SETJMP(j) setjmp((j)->buf)
#endif

#ifdef SNX_TRACE
extern u32 rt_trace_ring[256];
extern u32 rt_trace_pos;
extern u32 rt_trace_watch;
void rt_trace_watch_hit(CPU *c, u32 a);
extern int rt_trace_blocks;
void rt_trace_block(u32 a, u32 eax, u32 ecx, u32 edx, u32 ebx, u32 esp, u32 ebp, u32 esi, u32 edi);
extern int rt_trace_counts;
void rt_trace_count(u32 a);
#define ENTER(a) (rt_trace_ring[rt_trace_pos++ & 255] = (a), (a) == rt_trace_watch ? rt_trace_watch_hit(c, (a)) : (void)0, \
                  rt_trace_counts ? rt_trace_count(a) : (void)0,                                                           \
                  rt_trace_blocks ? rt_trace_block((a), eax, ecx, edx, ebx, esp, ebp, esi, edi) : (void)0)
#define BLK(a) (rt_trace_blocks ? rt_trace_block((a), eax, ecx, edx, ebx, esp, ebp, esi, edi) : (void)0)
#else
#define ENTER(a) ((void)0)
#define BLK(a) ((void)0)
#endif

// PF = 1, если в младшем байте чётное число единиц.
#define PARITY(x) ((u32)!__builtin_parity((u32)(x) & 0xFFu))

// Условные биты x87 после FCOM: C0 = 0x100, C2 = 0x400, C3 = 0x4000.
static inline u32 FCOM(double x, double y)
{
    if (x != x || y != y)
        return 0x4500u;
    if (x < y)
        return 0x0100u;
    if (x == y)
        return 0x4000u;
    return 0;
}

// fsin/fcos: при |x| >= 2^63 x87 оставляет операнд как есть (C2 = 1, «неполная редукция»),
// бесконечность — недопустимая операция; NaN-операнды и результаты — как у x87.
static inline double X87_TRIG(double x, double r)
{
    if (isinf(x))
        return crt_nan(NAN, x, x);
    if (!(fabs(x) < 0x1p63))
        return x;
    return crt_nan(r, x, x);
}

u32 rt_fxam(double v);
