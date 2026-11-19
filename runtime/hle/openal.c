// HLE OpenAL (OpenAL32.dll / DefOpenAL32.dll): ALAudio загружает DLL динамически и берёт все al*/alc* через
// GetProcAddress. Вызовы идут в OpenAL хоста (mac — OpenAL.framework, Switch — openal-soft). Соглашение — cdecl
// (снимается только адрес возврата). Массивы ALuint/ALint/ALfloat гостя имеют ту же раскладку, что у хоста,
// поэтому передаются указателями прямо в память гостя; строки результатов копируются в гостевую кучу.
// Устройство и контекст для гостя — непрозрачные описатели (блоки в гостевой куче). EAX нет — как у
// DefOpenAL32 без звуковой карты Creative. SNX_LOG_AUDIO=1 — буферы, запуски источников, ошибки AL.
#include <stdlib.h>
#include <string.h>

#ifdef __APPLE__
#define OPENAL_DEPRECATED
#include <OpenAL/al.h>
#include <OpenAL/alc.h>
#else
#include <AL/al.h>
#include <AL/alc.h>
#endif

#ifndef ALC_APIENTRY
#define ALC_APIENTRY
#endif

#include "platform/window.h"
#include "runtime/heap.h"
#include "runtime/hle/hle.h"

#define RETV() RET(0, 0)
#define P(i) (ARG(i) ? (void *)(g_mem + ARG(i)) : NULL)

static int log_audio(void)
{
    static int v = -1;
    if (v < 0)
        v = getenv("SNX_LOG_AUDIO") != NULL;
    return v;
}

static float argf(CPU *c, int i)
{
    u32 v = ARG(i);
    float f;
    memcpy(&f, &v, 4);
    return f;
}

// ---------------------------------------------------------------- описатели устройств и контекстов
#define MAX_H 8
static struct {
    u32 h;
    void *p;
} g_handles[MAX_H];

static u32 handle_of(void *p)
{
    if (!p)
        return 0;
    for (int i = 0; i < MAX_H; i++)
        if (g_handles[i].p == p)
            return g_handles[i].h;
    for (int i = 0; i < MAX_H; i++)
        if (!g_handles[i].p) {
            if (!g_handles[i].h)
                g_handles[i].h = heap_alloc(heap_process(), HEAP_ZERO_MEMORY, 16);
            g_handles[i].p = p;
            return g_handles[i].h;
        }
    return 0;
}

static void *host_of(u32 h)
{
    for (int i = 0; h && i < MAX_H; i++)
        if (g_handles[i].h == h)
            return g_handles[i].p;
    return NULL;
}

static void handle_drop(void *p)
{
    for (int i = 0; p && i < MAX_H; i++)
        if (g_handles[i].p == p)
            g_handles[i].p = NULL;  // блок гостя остаётся для следующего описателя
}

// строки результатов: одна копия в гостевой куче на каждое значение (+ два NUL)
static u32 guest_str(const char *s, size_t len)
{
    static struct {
        char *s;
        size_t len;
        u32 g;
    } cache[64];
    static int n;
    if (!s)
        return 0;
    for (int i = 0; i < n; i++)
        if (cache[i].len == len && !memcmp(cache[i].s, s, len))
            return cache[i].g;
    u32 g = heap_alloc(heap_process(), 0, (u32)len + 2);
    memcpy(g_mem + g, s, len);
    g_mem[g + len] = g_mem[g + len + 1] = 0;
    if (n < 64) {
        cache[n].s = malloc(len);
        memcpy(cache[n].s, s, len);
        cache[n].len = len;
        cache[n++].g = g;
    }
    return g;
}

// ---------------------------------------------------------------- ALC
// приложение в фоне (Android: свёрнуто) — вывод звука на паузе (ALC_SOFT_pause_device), иначе он играет дальше
static ALCdevice *g_pause_dev;

static void on_pause(int paused)
{
    ALCdevice *d = g_pause_dev;
    if (!d || !alcIsExtensionPresent(d, "ALC_SOFT_pause_device"))
        return;
    void(ALC_APIENTRY * fn)(ALCdevice *) =
        (void(ALC_APIENTRY *)(ALCdevice *))alcGetProcAddress(d, paused ? "alcDevicePauseSOFT" : "alcDeviceResumeSOFT");
    if (fn)
        fn(d);
}

static void alc_OpenDevice(CPU *c)
{
    const char *want = ARG(0) ? hle_cstr(ARG(0)) : "(default)";
    ALCdevice *d = alcOpenDevice(NULL);
    static int hooked;
    if (d) {
        g_pause_dev = d;
        if (!hooked) {
            hooked = 1;
            plat_add_pause_hook(on_pause);
        }
    }
    fprintf(stderr, "[openal] alcOpenDevice(%s) -> %s\n", want,
            d ? alcGetString(d, ALC_DEVICE_SPECIFIER) : "failed");
    RET(0, handle_of(d));
}

static void alc_CloseDevice(CPU *c)
{
    ALCdevice *d = host_of(ARG(0));
    if (d == g_pause_dev)
        g_pause_dev = NULL;
    ALCboolean r = d ? alcCloseDevice(d) : ALC_FALSE;
    handle_drop(d);
    RET(0, r);
}

static void alc_CreateContext(CPU *c)
{
    ALCint attrs[64];
    int n = 0;
    for (u32 a = ARG(1); a && n < 62; a += 8) {
        attrs[n] = (ALCint)rd32(g_mem, a);
        if (!attrs[n])
            break;
        attrs[n + 1] = (ALCint)rd32(g_mem, a + 4);
        n += 2;
    }
    attrs[n] = 0;
    ALCcontext *x = alcCreateContext(host_of(ARG(0)), ARG(1) ? attrs : NULL);
    RET(0, handle_of(x));
}

static void alc_DestroyContext(CPU *c)
{
    ALCcontext *x = host_of(ARG(0));
    if (x)
        alcDestroyContext(x);
    handle_drop(x);
    RETV();
}

static void alc_MakeContextCurrent(CPU *c) { RET(0, alcMakeContextCurrent(host_of(ARG(0)))); }
static void alc_GetCurrentContext(CPU *c) { RET(0, handle_of(alcGetCurrentContext())); }
static void alc_GetContextsDevice(CPU *c) { RET(0, handle_of(alcGetContextsDevice(host_of(ARG(0))))); }
static void alc_GetError(CPU *c) { RET(0, alcGetError(host_of(ARG(0)))); }
static void alc_ProcessContext(CPU *c) { alcProcessContext(host_of(ARG(0))); RETV(); }
static void alc_SuspendContext(CPU *c) { alcSuspendContext(host_of(ARG(0))); RETV(); }

static void alc_GetIntegerv(CPU *c)
{
    alcGetIntegerv(host_of(ARG(0)), (ALCenum)ARG(1), (ALCsizei)ARG(2), P(3));
    RETV();
}

static void alc_GetString(CPU *c)
{
    ALCdevice *d = host_of(ARG(0));
    ALCenum p = (ALCenum)ARG(1);
    const char *s = alcGetString(d, p);
    size_t len = 0;
    if (s && !d && (p == ALC_DEVICE_SPECIFIER || p == 0x310 /* ALC_CAPTURE_DEVICE_SPECIFIER */))
        while (s[len] || s[len + 1])  // список имён через NUL, конец — двойной NUL
            len++;
    else if (s)
        len = strlen(s);
    RET(0, guest_str(s, len));
}

// ---------------------------------------------------------------- AL: состояние
static void al_GetError(CPU *c)
{
    ALenum e = alGetError();
    if (e && log_audio())
        fprintf(stderr, "[openal] alGetError -> %#x\n", e);
    RET(0, e);
}
static void al_Enable(CPU *c) { alEnable((ALenum)ARG(0)); RETV(); }
static void al_Disable(CPU *c) { alDisable((ALenum)ARG(0)); RETV(); }
static void al_IsEnabled(CPU *c) { RET(0, alIsEnabled((ALenum)ARG(0))); }
static void al_DistanceModel(CPU *c) { alDistanceModel((ALenum)ARG(0)); RETV(); }
static void al_DopplerFactor(CPU *c) { alDopplerFactor(argf(c, 0)); RETV(); }
static void al_DopplerVelocity(CPU *c) { alDopplerVelocity(argf(c, 0)); RETV(); }
static void al_GetBoolean(CPU *c) { RET(0, alGetBoolean((ALenum)ARG(0))); }
static void al_GetBooleanv(CPU *c) { alGetBooleanv((ALenum)ARG(0), P(1)); RETV(); }
static void al_GetInteger(CPU *c) { RET(0, alGetInteger((ALenum)ARG(0))); }
static void al_GetIntegerv(CPU *c) { alGetIntegerv((ALenum)ARG(0), P(1)); RETV(); }
static void al_GetFloatv(CPU *c) { alGetFloatv((ALenum)ARG(0), P(1)); RETV(); }
static void al_GetDoublev(CPU *c) { alGetDoublev((ALenum)ARG(0), P(1)); RETV(); }
static void al_GetEnumValue(CPU *c) { RET(0, alGetEnumValue(hle_cstr(ARG(0)))); }

// результат float/double — в ST(0)
static void al_GetFloat(CPU *c)
{
    c->st_ret = alGetFloat((ALenum)ARG(0));
    RETV();
}

static void al_GetDouble(CPU *c)
{
    c->st_ret = alGetDouble((ALenum)ARG(0));
    RETV();
}

static void al_GetString(CPU *c)
{
    const char *s = alGetString((ALenum)ARG(0));
    RET(0, guest_str(s, s ? strlen(s) : 0));
}

static void al_IsExtensionPresent(CPU *c)
{
    const char *name = hle_cstr(ARG(0));
    RET(0, strncasecmp(name, "EAX", 3) ? alIsExtensionPresent(name) : AL_FALSE);
}

static void al_GetProcAddress(CPU *c);

// ---------------------------------------------------------------- AL: буферы и источники
static void al_GenBuffers(CPU *c) { alGenBuffers((ALsizei)ARG(0), P(1)); RETV(); }
static void al_DeleteBuffers(CPU *c) { alDeleteBuffers((ALsizei)ARG(0), P(1)); RETV(); }
static void al_IsBuffer(CPU *c) { RET(0, alIsBuffer(ARG(0))); }
static void al_GetBufferf(CPU *c) { alGetBufferf(ARG(0), (ALenum)ARG(1), P(2)); RETV(); }
static void al_GetBufferi(CPU *c) { alGetBufferi(ARG(0), (ALenum)ARG(1), P(2)); RETV(); }

static void al_BufferData(CPU *c)
{
    alBufferData(ARG(0), (ALenum)ARG(1), P(2), (ALsizei)ARG(3), (ALsizei)ARG(4));
    if (log_audio())
        fprintf(stderr, "[openal] alBufferData(%u, %#x, %u bytes, %u Hz)\n", ARG(0), ARG(1), ARG(3), ARG(4));
    RETV();
}

static void al_GenSources(CPU *c) { alGenSources((ALsizei)ARG(0), P(1)); RETV(); }
static void al_DeleteSources(CPU *c) { alDeleteSources((ALsizei)ARG(0), P(1)); RETV(); }
static void al_IsSource(CPU *c) { RET(0, alIsSource(ARG(0))); }
static void al_Sourcei(CPU *c) { alSourcei(ARG(0), (ALenum)ARG(1), (ALint)ARG(2)); RETV(); }
static void al_Sourcef(CPU *c) { alSourcef(ARG(0), (ALenum)ARG(1), argf(c, 2)); RETV(); }
static void al_Source3f(CPU *c) { alSource3f(ARG(0), (ALenum)ARG(1), argf(c, 2), argf(c, 3), argf(c, 4)); RETV(); }
static void al_Sourcefv(CPU *c) { alSourcefv(ARG(0), (ALenum)ARG(1), P(2)); RETV(); }
static void al_GetSourcei(CPU *c) { alGetSourcei(ARG(0), (ALenum)ARG(1), P(2)); RETV(); }
static void al_GetSourcef(CPU *c) { alGetSourcef(ARG(0), (ALenum)ARG(1), P(2)); RETV(); }
static void al_GetSourcefv(CPU *c) { alGetSourcefv(ARG(0), (ALenum)ARG(1), P(2)); RETV(); }
static void al_SourcePlay(CPU *c)
{
    if (log_audio()) {
        ALint b = 0, q = 0;
        alGetSourcei(ARG(0), AL_BUFFER, &b);
        alGetSourcei(ARG(0), AL_BUFFERS_QUEUED, &q);
        fprintf(stderr, "[openal] alSourcePlay(%u) buffer %d queued %d\n", ARG(0), b, q);
    }
    alSourcePlay(ARG(0));
    RETV();
}
static void al_SourcePause(CPU *c) { alSourcePause(ARG(0)); RETV(); }
static void al_SourceStop(CPU *c) { alSourceStop(ARG(0)); RETV(); }
static void al_SourceRewind(CPU *c) { alSourceRewind(ARG(0)); RETV(); }
static void al_SourcePlayv(CPU *c) { alSourcePlayv((ALsizei)ARG(0), P(1)); RETV(); }
static void al_SourcePausev(CPU *c) { alSourcePausev((ALsizei)ARG(0), P(1)); RETV(); }
static void al_SourceStopv(CPU *c) { alSourceStopv((ALsizei)ARG(0), P(1)); RETV(); }
static void al_SourceRewindv(CPU *c) { alSourceRewindv((ALsizei)ARG(0), P(1)); RETV(); }
static void al_SourceQueueBuffers(CPU *c) { alSourceQueueBuffers(ARG(0), (ALsizei)ARG(1), P(2)); RETV(); }
static void al_SourceUnqueueBuffers(CPU *c) { alSourceUnqueueBuffers(ARG(0), (ALsizei)ARG(1), P(2)); RETV(); }

static void al_Listenerf(CPU *c) { alListenerf((ALenum)ARG(0), argf(c, 1)); RETV(); }
static void al_Listener3f(CPU *c) { alListener3f((ALenum)ARG(0), argf(c, 1), argf(c, 2), argf(c, 3)); RETV(); }
static void al_Listenerfv(CPU *c) { alListenerfv((ALenum)ARG(0), P(1)); RETV(); }
static void al_Listeneri(CPU *c) { alListeneri((ALenum)ARG(0), (ALint)ARG(1)); RETV(); }
static void al_GetListenerf(CPU *c) { alGetListenerf((ALenum)ARG(0), P(1)); RETV(); }
static void al_GetListener3f(CPU *c) { alGetListener3f((ALenum)ARG(0), P(1), P(2), P(3)); RETV(); }
static void al_GetListenerfv(CPU *c) { alGetListenerfv((ALenum)ARG(0), P(1)); RETV(); }
static void al_GetListeneri(CPU *c) { alGetListeneri((ALenum)ARG(0), P(1)); RETV(); }

#define F(n) {"al" #n, al_##n, 0}
#define FC(n) {"alc" #n, alc_##n, 0}
hle_dyn_fn hle_openal_fns[] = {
    FC(OpenDevice), FC(CloseDevice), FC(CreateContext), FC(DestroyContext), FC(MakeContextCurrent),
    FC(GetCurrentContext), FC(GetContextsDevice), FC(GetError), FC(ProcessContext), FC(SuspendContext),
    FC(GetIntegerv), FC(GetString),
    F(GetError), F(Enable), F(Disable), F(IsEnabled), F(DistanceModel), F(DopplerFactor), F(DopplerVelocity),
    F(GetBoolean), F(GetBooleanv), F(GetInteger), F(GetIntegerv), F(GetFloat), F(GetFloatv), F(GetDouble),
    F(GetDoublev), F(GetEnumValue), F(GetString), F(IsExtensionPresent), F(GetProcAddress),
    F(GenBuffers), F(DeleteBuffers), F(IsBuffer), F(GetBufferf), F(GetBufferi), F(BufferData),
    F(GenSources), F(DeleteSources), F(IsSource), F(Sourcei), F(Sourcef), F(Source3f), F(Sourcefv),
    F(GetSourcei), F(GetSourcef), F(GetSourcefv), F(SourcePlay), F(SourcePause), F(SourceStop), F(SourceRewind),
    F(SourcePlayv), F(SourcePausev), F(SourceStopv), F(SourceRewindv), F(SourceQueueBuffers),
    F(SourceUnqueueBuffers), F(Listenerf), F(Listener3f), F(Listenerfv), F(Listeneri), F(GetListenerf),
    F(GetListener3f), F(GetListenerfv), F(GetListeneri),
    {NULL, NULL, 0},
};

// alGetProcAddress: только функции этой же таблицы (EAXGet/EAXSet — нет)
static void al_GetProcAddress(CPU *c) { RET(0, hle_dyn_thunk(hle_openal_fns, "openal32.dll", hle_cstr(ARG(0)))); }
