// Нативные обёртки функций Engine.dll (таблица — recomp/overrides.py, WRAPS).
#include <math.h>
#include <stdio.h>

#include "runtime/abi.h"
#include "runtime/rt.h"
#include "runtime/script_fix.h"

// ---------------------------------------------------------------- Hor+ на широком экране
// FCameraSceneNode::UpdateMatrices строит проекцию FPerspectiveMatrix(FOV/2, FOV/2, 1, W/H): горизонтальный
// угол FOV (градусы, [node+0x1E0]) постоянен, и на 16:9 картинка обрезается по вертикали (vert-). Если кадр
// шире 4:3, матрицы строятся заново с горизонтальным углом, при котором вертикальный — как в 4:3 (Hor+).
// Отсечение по пирамиде видимости движок считает из этих же матриц. FOV узла после вызова — прежний.
// Узел: +0x94 ProjectionMatrix (M[0][0] +0x94, M[1][1] +0xA8, M[2][3] +0xC0, M[3][3] +0xD0).
extern void (*const ovr_orig_ovr_engine_camera_matrices)(CPU *c);

#define NODE_FOV 0x1E0u
#define PROJ 0x94u

void ovr_engine_camera_matrices(CPU *c)
{
    u32 node = c->ecx, ret = rd32(g_mem, c->esp);
    RT_ENTER_GEN(c, ovr_orig_ovr_engine_camera_matrices);  // оригинал — код гостя
    float m00 = rdf32(g_mem, node + PROJ), m11 = rdf32(g_mem, node + PROJ + 0x14);
    float m23 = rdf32(g_mem, node + PROJ + 0x2C), m33 = rdf32(g_mem, node + PROJ + 0x3C);
    if (m23 != 1.0f || m33 != 0.0f || !(m00 > 0.0f))
        return;  // не перспектива (виды редактора, ортогональные)
    double aspect = (double)m11 / (double)m00, k = aspect / (4.0 / 3.0);
    float fov = rdf32(g_mem, node + NODE_FOV);
    if (k < 1.01 || !(fov > 1.0f && fov < 170.0f))
        return;
    double wide = 2.0 * atan(tan((double)fov * M_PI / 360.0) * k) * 180.0 / M_PI;
    wrf32(g_mem, node + NODE_FOV, (float)wide);
    c->esp -= 4;  // повторный вызов тем же адресом возврата (thiscall без аргументов)
    wr32(g_mem, c->esp, ret);
    c->ecx = node;
    RT_ENTER_GEN(c, ovr_orig_ovr_engine_camera_matrices);  // оригинал — код гостя
    wrf32(g_mem, node + NODE_FOV, fov);
}

// ---------------------------------------------------------------- без сброса рендера после сохранения
// UGameEngine::Tick после отложенного сохранения (автосохранение, точки сохранения) зовёт SaveGame, затем
// Exec("RestartSounds") и Flush(1): все ресурсы рендера (текстуры, буферы мешей) выбрасываются, и следующий кадр
// прогревает уровень целиком — 36 МБ текстур и 14 тыс. вызовов отрисовки на болоте, на Switch ≈1,3 с из
// двухсекундного фриза сохранения. Уровень после сохранения тот же, и этот сброс пропускается: Flush с той же
// глубины стека гостя, что и SaveGame, и вскоре за местом его вызова (тот же Tick), ничего не делает. Консольный
// SAVEGAME сброса не зовёт. Каждый Flush — строка в лог (откуда и с каким аргументом), SaveGame — с его временем.
extern void (*const ovr_orig_ovr_engine_save_game)(CPU *c);
extern void (*const ovr_orig_ovr_engine_flush)(CPU *c);

static u32 g_save_esp, g_save_ret;  // стек и адрес возврата последнего SaveGame (0 — нет)

void ovr_engine_save_game(CPU *c)
{
    u32 esp = c->esp, ret = rd32(g_mem, esp), slot = rd32(g_mem, esp + 4);
    u64 t0 = rt_ticks_ns();
    RT_ENTER_GEN(c, ovr_orig_ovr_engine_save_game);  // оригинал — код гостя
    fprintf(stderr, "[engine] SaveGame(%d): %.0f ms\n", (int)slot, (double)(rt_ticks_ns() - t0) / 1e6);
    g_save_esp = esp;
    g_save_ret = ret;
}

void ovr_engine_flush(CPU *c)
{
    u32 ret = rd32(g_mem, c->esp), precache = rd32(g_mem, c->esp + 4);
    int skip = g_save_ret && c->esp == g_save_esp && ret - g_save_ret < 0x100u;
    g_save_ret = 0;
    fprintf(stderr, "[engine] Flush(%u) from %08x%s\n", precache, ret, skip ? ", right after SaveGame: skipped" : "");
    if (skip) {
        c->esp += 8;  // thiscall с одним аргументом: ret 4
        return;
    }
    script_fix_scan(0);  // загрузка карты: SHGame.u мог загрузиться заново (runtime/script_fix.c)
    RT_ENTER_GEN(c, ovr_orig_ovr_engine_flush);  // оригинал — код гостя
}
