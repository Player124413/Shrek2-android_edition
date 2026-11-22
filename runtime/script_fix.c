// Исправления байткода UnrealScript игры в памяти (см. script_fix.h).
//
// Бонусные уровни (Beanstalk_bonus, _dawn, _knight), запущенные напрямую (выбор уровня лаунчера): камера стоит
// у ворот фермы, героя не видно, вступление рассказчика не идёт, таймер стоит. Причина — в скрипте игры
// (SHGame.Shrek.PostLoadGame, зовётся после загрузки карты):
//
//     foreach DynamicActors(class'ShHeroPawn', shhp)
//         if (shhp != None && shhp.Tag == 'MainPlayer') shhp.Tag = SavedTag;   // снять метку MainPlayer
//     if (FuturePlayerLabel == "") return;                                     // <- выход до возврата метки
//     SwitchControlToPawn(FuturePlayerLabel);
//     FuturePlayerLabel = "";
//     SavedTag = Level.PlayerHeroActor.Tag;
//     Level.PlayerHeroActor.Tag = 'MainPlayer';                                // вернуть метку герою
//
// Метку MainPlayer ищут камера и вступительная сценка. Если герой не пришёл с другого уровня (плакат
// «Разыскивается» переносит его через ServerTravel), FuturePlayerLabel пуст, и метка снимается без возврата.
// То же исправление — в Shrek 2 Enhanced Edition (SquareRoundOne/shrek2-enhanced-edition, tools/menupatch.py:
// «bonus level started directly ... lost the player»): при пустом FuturePlayerLabel пропускается только смена
// героя, метка возвращается.
//
// Байткод в памяти (ссылки на объекты и FName — по 4 байта, переходы — смещения в Script):
//     07 <u16 A> 7A 01 <L> 1F 00 16      if (!(FuturePlayerLabel == "")) goto A       ; A = это + 14
//     04 0B                              return
//  A: 1B <FName> 01 <L> 16                SwitchControlToPawn(FuturePlayerLabel)
//     0F 01 <L> 1F 00                    FuturePlayerLabel = ""
//  B: 0F ...                             SavedTag = Level.PlayerHeroActor.Tag
// Правка той же длины: 7A (==) -> 7B (!=), переход -> B, 04 0B (return) -> 0B 0B (nothing, nothing):
// пустая метка — переход к B, непустая — как раньше (смена героя, затем B).
// В моде Shrek 2.2 перед этим ещё «if (IsInBonusLevel()) { FuturePlayerLabel = ""; return; }» — его return тоже
// становится nothing.
// SNX_SCRIPT_FIX=0 — не исправлять. Результат — строка [script] в snx.log.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "runtime/process.h"
#include "runtime/script_fix.h"
#include "runtime/vmem.h"

static u32 le32(const u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }

// if-переход с условием «FuturePlayerLabel <cond> ""» в p; -> смещение B (присваивание SavedTag) или 0
static u32 match_label_check(const u8 *s, u32 n, u32 p, u8 cond, u8 ret, u32 *label)
{
    if (p + 15 > n || s[p] != 0x07 || s[p + 3] != cond || s[p + 4] != 0x01 || s[p + 9] != 0x1F || s[p + 10] ||
        s[p + 11] != 0x16 || s[p + 12] != ret || s[p + 13] != 0x0B || s[p + 14] != 0x1B)
        return 0;
    u32 l = le32(s + p + 5), target = s[p + 1] | (u32)s[p + 2] << 8;
    for (u32 nm = 4; nm <= 8; nm += 4) {  // FName в байткоде — 4 байта (8 — на случай иной сборки)
        u32 a = p + 15 + nm, q = a + 6, b = q + 8;  // a: 01 <L> 16; q: 0F 01 <L> 1F 00; b: 0F
        if (b >= n)
            break;
        if (s[a] != 0x01 || le32(s + a + 1) != l || s[a + 5] != 0x16)
            continue;
        if (s[q] != 0x0F || s[q + 1] != 0x01 || le32(s + q + 2) != l || s[q + 6] != 0x1F || s[q + 7] || s[b] != 0x0F)
            continue;
        if (target != (cond == 0x7A ? p + 14 : b))
            continue;
        *label = l;
        return b;
    }
    return 0;
}

int script_fix_post_load_game(u8 *s, u32 n)
{
    if (!s || n < 40 || n > 0xFFFF)
        return 0;
    for (u32 p = 0; p + 40 <= n; p++) {
        u32 label;
        if (match_label_check(s, n, p, 0x7B, 0x0B, &label))
            return 2;
        u32 b = match_label_check(s, n, p, 0x7A, 0x04, &label);
        if (!b)
            continue;
        // мод Shrek 2.2: «07 <p> 81 1B <FName> 16 16 0F 01 <L> 1F 00 04 0B» прямо перед p
        if (p >= 24 && s[p - 2] == 0x04 && s[p - 1] == 0x0B && s[p - 4] == 0x1F && !s[p - 3] && s[p - 10] == 0x0F &&
            s[p - 9] == 0x01 && le32(s + p - 8) == label)
            for (u32 j = p - 24; j + 10 <= p - 10; j++)
                if (s[j] == 0x07 && (s[j + 1] | (u32)s[j + 2] << 8) == p && s[j + 3] == 0x81 && s[j + 4] == 0x1B) {
                    s[p - 2] = 0x0B;
                    break;
                }
        s[p + 1] = (u8)b;
        s[p + 2] = (u8)(b >> 8);
        s[p + 3] = 0x7B;
        s[p + 12] = 0x0B;
        return 1;
    }
    return 0;
}

// ---------------------------------------------------------------- объекты гостя
// Раскладка этой сборки UE2 (как runtime/game/uobj.c): UObject — Outer +0x18, Name +0x20, Class +0x24;
// UField — Next +0x30; UStruct — Children +0x40, Script (TArray<BYTE>: данные, число, ёмкость) +0x4C.
#define OBJ_OUTER 0x18u
#define OBJ_NAME 0x20u
#define OBJ_CLASS 0x24u
#define FIELD_NEXT 0x30u
#define STRUCT_CHILDREN 0x40u
#define STRUCT_SCRIPT 0x4Cu

static u32 g_names;  // FName::Names (TArray<FNameEntry*>)
static int g_state;  // 0 — ищем, 1 — исправлено или не нужно, -1 — выключено
static u32 g_fn;     // Shrek.PostLoadGame, встреченная до загрузки её байткода: проверяется на каждом Link

static int committed(u32 a, u32 size)
{
    vm_info mi;
    return a >= 0x10000 && vm_query(a, &mi) && mi.state == VM_MEM_COMMIT && a + size <= mi.base + mi.size;
}

static int name_is(u32 obj, const char *name)
{
    if (!obj || !committed(obj, 0x28))
        return 0;
    u32 idx = rd32(g_mem, obj + OBJ_NAME);
    if (idx >= rd32(g_mem, g_names + 4))
        return 0;
    u32 e = rd32(g_mem, rd32(g_mem, g_names) + 4 * idx);
    if (!e)
        return 0;
    for (u32 i = 0;; i++) {
        u16 ch = rd16(g_mem, e + 0xC + 2 * i);
        unsigned a = (unsigned char)name[i];
        if ((ch | 0x20u) != (a | 0x20u) || (!ch) != (!a))
            return 0;
        if (!ch)
            return 1;
    }
}

// Script функции: TArray на STRUCT_SCRIPT (*exact = 1), иначе первый правдоподобный TArray (байткод кончается на
// 0x53, EX_EndOfScript) в полях UStruct
static int script_of(u32 fn, u32 *data, u32 *num, int *exact)
{
    for (u32 k = 0; k <= 16; k++) {
        u32 off = k ? 0x40 + 4 * k : STRUCT_SCRIPT;
        u32 d = rd32(g_mem, fn + off), cnt = rd32(g_mem, fn + off + 4), cap = rd32(g_mem, fn + off + 8);
        if (cnt < 16 || cnt > 0xFFFF || cap < cnt || !committed(d, cnt) || g_mem[d + cnt - 1] != 0x53)
            continue;
        *data = d;
        *num = cnt;
        *exact = !k;
        return 1;
    }
    return 0;
}

static void fix_post_load_game(u32 fn)
{
    u32 data, num;
    int exact;
    if (!script_of(fn, &data, &num, &exact))
        return;  // байткод ещё не загружен — следующий Link
    int r = script_fix_post_load_game(g_mem + data, num);
    if (r == 1)
        fprintf(stderr, "[script] Shrek.PostLoadGame: hero keeps the MainPlayer tag without a travelled hero "
                        "(bonus levels started directly)\n");
    else if (r == 2)
        fprintf(stderr, "[script] Shrek.PostLoadGame: already fixed\n");
    else if (exact) {
        fprintf(stderr, "[script] Shrek.PostLoadGame: bytecode not recognised (%u bytes), left as is:", num);
        for (u32 i = 0; i < num && i < 256; i++)
            fprintf(stderr, " %02x", g_mem[data + i]);
        fputc('\n', stderr);
    } else
        return;  // не тот TArray — ждать следующего Link
    g_state = 1;
}

void script_fix_linked(u32 st)
{
    if (g_state)
        return;
    if (!g_names) {
        const char *env = getenv("SNX_SCRIPT_FIX");
        if (env && !strcmp(env, "0")) {
            g_state = -1;
            return;
        }
        rt_module *core = rt_module_by_name("Core");
        g_names = core ? rt_module_export(core, "?Names@FName@@0V?$TArray@PAUFNameEntry@@@@A", 0) : 0;
        if (!g_names)
            return;
    }
    if (!st || !committed(st, 0x58))
        return;
    if (g_fn && committed(g_fn, 0x58)) {
        fix_post_load_game(g_fn);
        if (g_state)
            return;
    }
    u32 cls = rd32(g_mem, st + OBJ_CLASS);
    if (name_is(st, "PostLoadGame") && name_is(cls, "Function") && name_is(rd32(g_mem, st + OBJ_OUTER), "Shrek")) {
        g_fn = st;
        fix_post_load_game(st);
        return;
    }
    if (name_is(st, "Shrek") && name_is(cls, "Class"))  // функции класса могли загрузиться раньше него
        for (u32 f = rd32(g_mem, st + STRUCT_CHILDREN), j = 0; f && j < 4096 && !g_state; j++) {
            if (!committed(f, 0x58))
                break;
            if (name_is(f, "PostLoadGame") && name_is(rd32(g_mem, f + OBJ_CLASS), "Function")) {
                g_fn = f;
                fix_post_load_game(f);
            }
            f = rd32(g_mem, f + FIELD_NEXT);
        }
}
