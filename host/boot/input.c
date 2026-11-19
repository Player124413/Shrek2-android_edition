// Ввод хоста (события SDL, главный поток) -> гость: сообщения Win32 окну с фокусом (WM_KEY*, WM_CHAR,
// WM_MOUSE*), состояние клавиш для GetKeyState, относительные смещения и кнопки мыши для DirectInput.
// Геймпад (Switch: Joy-Con/Pro): в игре — джойстик DirectInput, в меню — мышь (см. раздел «геймпад» ниже и
// раскладку <user>/System/Snx.ini, g_snx_default).
#include <SDL.h>
#include <ctype.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "host/boot/input.h"
#include "host/boot/touch.h"
#include "runtime/hle/d3d8.h"
#include "runtime/hle/dinput8.h"
#include "runtime/hle/user32.h"
#include "runtime/game/menu.h"

enum {
    WM_CLOSE = 0x10, WM_KEYDOWN = 0x100, WM_KEYUP = 0x101, WM_CHAR = 0x102, WM_SYSKEYDOWN = 0x104,
    WM_SYSKEYUP = 0x105, WM_MOUSEMOVE = 0x200, WM_LBUTTONDOWN = 0x201, WM_LBUTTONUP = 0x202,
    WM_RBUTTONDOWN = 0x204, WM_RBUTTONUP = 0x205, WM_MBUTTONDOWN = 0x207, WM_MBUTTONUP = 0x208,
    WM_MOUSEWHEEL = 0x20A,
};

static u32 vk_of(SDL_Scancode sc)
{
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z)
        return 'A' + (u32)(sc - SDL_SCANCODE_A);
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9)
        return '1' + (u32)(sc - SDL_SCANCODE_1);
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12)
        return 0x70 + (u32)(sc - SDL_SCANCODE_F1);
    if (sc >= SDL_SCANCODE_KP_1 && sc <= SDL_SCANCODE_KP_9)
        return 0x61 + (u32)(sc - SDL_SCANCODE_KP_1);
    switch (sc) {
    case SDL_SCANCODE_0: return '0';
    case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: return 0x0D;
    case SDL_SCANCODE_ESCAPE: return 0x1B;
    case SDL_SCANCODE_BACKSPACE: return 0x08;
    case SDL_SCANCODE_TAB: return 0x09;
    case SDL_SCANCODE_SPACE: return 0x20;
    case SDL_SCANCODE_MINUS: return 0xBD;
    case SDL_SCANCODE_EQUALS: return 0xBB;
    case SDL_SCANCODE_LEFTBRACKET: return 0xDB;
    case SDL_SCANCODE_RIGHTBRACKET: return 0xDD;
    case SDL_SCANCODE_BACKSLASH: return 0xDC;
    case SDL_SCANCODE_SEMICOLON: return 0xBA;
    case SDL_SCANCODE_APOSTROPHE: return 0xDE;
    case SDL_SCANCODE_GRAVE: return 0xC0;
    case SDL_SCANCODE_COMMA: return 0xBC;
    case SDL_SCANCODE_PERIOD: return 0xBE;
    case SDL_SCANCODE_SLASH: return 0xBF;
    case SDL_SCANCODE_CAPSLOCK: return 0x14;
    case SDL_SCANCODE_PAUSE: return 0x13;
    case SDL_SCANCODE_INSERT: return 0x2D;
    case SDL_SCANCODE_HOME: return 0x24;
    case SDL_SCANCODE_PAGEUP: return 0x21;
    case SDL_SCANCODE_DELETE: return 0x2E;
    case SDL_SCANCODE_END: return 0x23;
    case SDL_SCANCODE_PAGEDOWN: return 0x22;
    case SDL_SCANCODE_RIGHT: return 0x27;
    case SDL_SCANCODE_LEFT: return 0x25;
    case SDL_SCANCODE_DOWN: return 0x28;
    case SDL_SCANCODE_UP: return 0x26;
    case SDL_SCANCODE_KP_0: return 0x60;
    case SDL_SCANCODE_KP_DIVIDE: return 0x6F;
    case SDL_SCANCODE_KP_MULTIPLY: return 0x6A;
    case SDL_SCANCODE_KP_MINUS: return 0x6D;
    case SDL_SCANCODE_KP_PLUS: return 0x6B;
    case SDL_SCANCODE_KP_PERIOD: return 0x6E;
    case SDL_SCANCODE_LCTRL: case SDL_SCANCODE_RCTRL: return 0x11;
    case SDL_SCANCODE_LSHIFT: case SDL_SCANCODE_RSHIFT: return 0x10;
    case SDL_SCANCODE_LALT: case SDL_SCANCODE_RALT: return 0x12;
    default: return 0;
    }
}

static u32 g_buttons;              // MK_LBUTTON 1, MK_RBUTTON 2, MK_MBUTTON 0x10
static float g_cu = 0.5f, g_cv = 0.5f;  // курсор в долях окна (мышь хоста или правый стик)

// курсор (доли окна) -> координаты заднего буфера гостя
static u32 cursor_lparam(void)
{
    int x, y;
    if (!d3d8_window_to_target(g_cu, g_cv, &x, &y)) {
        x = (int)(g_cu * 800.0f);
        y = (int)(g_cv * 600.0f);
    }
    user32_set_cursor_pos(x, y);
    return (u32)(u16)x | (u32)(u16)y << 16;
}

static u32 mouse_lparam(int wx, int wy)
{
    SDL_Window *w = SDL_GetMouseFocus();
    int ww = 1, wh = 1;
    if (w)
        SDL_GetWindowSize(w, &ww, &wh);
    g_cu = (float)wx / (float)ww;
    g_cv = (float)wy / (float)wh;
    return cursor_lparam();
}

static void key(u32 vk, int down)
{
    u32 hwnd = user32_focus();
    if (!hwnd)
        return;
    user32_set_key(vk, down);
    user32_post(hwnd, down ? WM_KEYDOWN : WM_KEYUP, vk, 1u | (down ? 0 : 3u << 30));
}

// кнопка мыши: 0 левая, 1 правая, 2 средняя
static void mouse_button(int idx, int down, u32 lparam)
{
    static const u32 mk[3] = {1, 2, 0x10}, vk[3] = {1, 2, 4};
    static const u32 msg[3][2] = {{WM_LBUTTONUP, WM_LBUTTONDOWN}, {WM_RBUTTONUP, WM_RBUTTONDOWN},
                                  {WM_MBUTTONUP, WM_MBUTTONDOWN}};
    u32 hwnd = user32_focus();
    g_buttons = down ? g_buttons | mk[idx] : g_buttons & ~mk[idx];
    dinput8_mouse_button(idx, down);
    user32_set_key(vk[idx], down);
    if (hwnd)
        user32_post(hwnd, msg[idx][down], g_buttons, lparam);
}

static void wheel(int clicks)
{
    u32 hwnd = user32_focus();
    dinput8_mouse_move(0, 0, clicks * 120);
    if (hwnd)
        user32_post(hwnd, WM_MOUSEWHEEL, g_buttons | (u32)(u16)(s16)(clicks * 120) << 16, 0);
}

// ---------------------------------------------------------------- геймпад
// Два режима. В игре геймпад виден игре как джойстик DirectInput (runtime/hle/dinput8.c): левый стик — оси
// JoyX/JoyY, правый — JoyU/JoyV, кнопки — Joy1..Joy12, крестовина — Joy13..Joy16; их действия — в User.ini
// (умолчания пишет host/boot/config.c). Пока зажата кнопка слоя (Layer=, по умолчанию ZL), кнопки шлют
// клавиши второго слоя из Snx.ini («ZL+X=5» — зелья 1–9); «Кнопка=Клавиша» без слоя — клавиша вместо джойстика.
// В меню игры (GUIController.ActivePage, runtime/game/menu.c) джойстик молчит, а геймпад ведёт GUI игры:
// крестовина — фокус на соседний элемент (у ползунка и выпадающего списка влево/вправо — стрелки: значение),
// A — щелчок по элементу в фокусе, B — по кнопке «назад» страницы (BACK, CANCEL, NO), если она есть, иначе
// Escape; L/R — вкладки настроек; Plus — Escape, Minus — пробел; стики — курсор. Касание — щелчок в точке.
static float g_axis[SDL_CONTROLLER_AXIS_MAX];
static int g_trig[2];
enum { PB_ZL = SDL_CONTROLLER_BUTTON_MAX, PB_ZR, PB_N };  // курки — тоже кнопки
enum {  // цели, кроме клавиш
    T_LMB = 0x1000, T_RMB, T_MMB, T_WHEEL_UP, T_WHEEL_DOWN, T_FPS, T_JOY, T_NAV, T_SELECT, T_BACK, T_TAB_PREV, T_TAB_NEXT
};
static const char *const g_pb_name[PB_N] = {
    [SDL_CONTROLLER_BUTTON_A] = "A", [SDL_CONTROLLER_BUTTON_B] = "B", [SDL_CONTROLLER_BUTTON_X] = "X",
    [SDL_CONTROLLER_BUTTON_Y] = "Y", [SDL_CONTROLLER_BUTTON_BACK] = "Minus", [SDL_CONTROLLER_BUTTON_START] = "Plus",
    [SDL_CONTROLLER_BUTTON_LEFTSTICK] = "LStick", [SDL_CONTROLLER_BUTTON_RIGHTSTICK] = "RStick",
    [SDL_CONTROLLER_BUTTON_LEFTSHOULDER] = "L", [SDL_CONTROLLER_BUTTON_RIGHTSHOULDER] = "R",
    [SDL_CONTROLLER_BUTTON_DPAD_UP] = "DPadUp", [SDL_CONTROLLER_BUTTON_DPAD_DOWN] = "DPadDown",
    [SDL_CONTROLLER_BUTTON_DPAD_LEFT] = "DPadLeft", [SDL_CONTROLLER_BUTTON_DPAD_RIGHT] = "DPadRight",
    [PB_ZL] = "ZL", [PB_ZR] = "ZR",
};
// кнопка -> номер кнопки джойстика + 1 (Joy1 = A ... Joy12 = RStick); крестовина — POV, 0
static const u8 g_pb_joy[PB_N] = {
    [SDL_CONTROLLER_BUTTON_A] = 1, [SDL_CONTROLLER_BUTTON_B] = 2, [SDL_CONTROLLER_BUTTON_X] = 3,
    [SDL_CONTROLLER_BUTTON_Y] = 4, [SDL_CONTROLLER_BUTTON_LEFTSHOULDER] = 5, [SDL_CONTROLLER_BUTTON_RIGHTSHOULDER] = 6,
    [PB_ZL] = 7, [PB_ZR] = 8, [SDL_CONTROLLER_BUTTON_BACK] = 9, [SDL_CONTROLLER_BUTTON_START] = 10,
    [SDL_CONTROLLER_BUTTON_LEFTSTICK] = 11, [SDL_CONTROLLER_BUTTON_RIGHTSTICK] = 12,
};
static const struct {
    const char *name;
    u32 t;
} g_key_names[] = {
    {"LeftMouse", T_LMB}, {"RightMouse", T_RMB}, {"MiddleMouse", T_MMB}, {"MouseWheelUp", T_WHEEL_UP},
    {"MouseWheelDown", T_WHEEL_DOWN}, {"Enter", 0x0D}, {"Escape", 0x1B}, {"Space", 0x20}, {"Tab", 0x09},
    {"Backspace", 0x08}, {"Shift", 0x10}, {"Ctrl", 0x11}, {"Alt", 0x12}, {"Pause", 0x13}, {"CapsLock", 0x14},
    {"PageUp", 0x21}, {"PageDown", 0x22}, {"End", 0x23}, {"Home", 0x24}, {"Left", 0x25}, {"Up", 0x26},
    {"Right", 0x27}, {"Down", 0x28}, {"Insert", 0x2D}, {"Delete", 0x2E}, {"Semicolon", 0xBA}, {"Equals", 0xBB},
    {"Comma", 0xBC}, {"Minus", 0xBD}, {"Period", 0xBE}, {"Slash", 0xBF}, {"Tilde", 0xC0}, {"LeftBracket", 0xDB},
    {"Backslash", 0xDC}, {"RightBracket", 0xDD}, {"SingleQuote", 0xDE}, {"ToggleFPS", T_FPS},
};

#define SNX_INI_VERSION 2
static const char g_snx_default[] =
    "; Настройки порта shrekNX. Правятся здесь, действуют со следующего запуска.\n"
    "[Port]\n"
    "Version=2          ; формат файла (файл старого формата переименовывается в Snx.ini.old)\n"
    "\n"
    "[Overlay]\n"
    "ShowFPS=False      ; счётчик кадров в углу (переключается и на ходу: ZL+Minus)\n"
    "\n"
    "; Геймпад. В игре он виден ей как джойстик: кнопки A B X Y L R ZL ZR Minus Plus LStick RStick - Joy1..Joy12,\n"
    "; крестовина - Joy13..Joy16 (вверх, вправо, вниз, влево), левый стик - JoyX/JoyY, правый - JoyU/JoyV.\n"
    "; Что они делают - в User.ini, секция [Engine.Input] (Joy1=Fire и т. д.).\n"
    "; Здесь - второй слой: ZL+Кнопка=Клавиша, пока зажат ZL; Кнопка=Клавиша - клавиша вместо джойстика.\n"
    "; Кнопки: A B X Y L R ZL ZR Plus Minus LStick RStick DPadUp DPadDown DPadLeft DPadRight.\n"
    "; Клавиши - имена из User.ini игры: A..Z 0..9 F1..F12 NumPad0..NumPad9 Enter Escape Space Tab Up Down Left\n"
    ";   Right Home End PageUp PageDown LeftBracket RightBracket ... и LeftMouse RightMouse MiddleMouse\n"
    ";   MouseWheelUp MouseWheelDown, ToggleFPS.\n"
    "; В меню игры: стики - курсор, крестовина - к соседней кнопке меню, A - выбрать, B и Plus - назад.\n"
    "[Pad]\n"
    "Layer=ZL           ; кнопка второго слоя (None - без слоя)\n"
    "Plus=Escape        ; меню - клавишей: открытое с джойстика меню игра рисует без книги\n"
    "MoveSpeed=1200     ; бег при полном наклоне левого стика (как клавиша W)\n"
    "CameraSpeed=72     ; поворот камеры правым стиком (умножается ещё на скорость мыши из меню игры)\n"
    "InvertCameraY=False\n"
    "DeadZone=0.15      ; мёртвая зона стиков (доля хода)\n"
    "; зелья 1-9 (как цифры на клавиатуре): с ZL крестовина по часовой стрелке от верха - 1..4,\n"
    "; кнопки по часовой стрелке от X - 5..8, R - 9\n"
    "ZL+DPadUp=1\n"
    "ZL+DPadRight=2\n"
    "ZL+DPadDown=3\n"
    "ZL+DPadLeft=4\n"
    "ZL+X=5\n"
    "ZL+A=6\n"
    "ZL+B=7\n"
    "ZL+Y=8\n"
    "ZL+R=9\n"
    "ZL+Minus=ToggleFPS\n";

static u32 g_map[2][PB_N];  // [слой][кнопка] -> виртуальная клавиша или T_*; 0 — джойстик (в слое — ничего)
static u32 g_held[PB_N];    // что нажала кнопка: отпускается то же, даже если сменились слой или режим
static int g_layer_btn = -1, g_layer, g_menu;
static float g_move_speed = 1200.0f, g_cam_speed = 72.0f, g_dead = 0.15f;
static int g_invert_y;

static int pad_button_of(const char *n)
{
    for (int b = 0; b < PB_N; b++)
        if (g_pb_name[b] && !strcasecmp(g_pb_name[b], n))
            return b;
    return -1;
}

// имя клавиши -> цель; 0 — нет такой
static u32 key_of(const char *n)
{
    size_t len = strlen(n);
    if (len == 1 && isalnum((unsigned char)n[0]))
        return (u32)toupper((unsigned char)n[0]);
    if ((n[0] == 'F' || n[0] == 'f') && len <= 3 && atoi(n + 1) >= 1 && atoi(n + 1) <= 12)
        return 0x6F + (u32)atoi(n + 1);
    if (!strncasecmp(n, "NumPad", 6) && len == 7 && isdigit((unsigned char)n[6]))
        return 0x60 + (u32)(n[6] - '0');
    for (size_t i = 0; i < sizeof g_key_names / sizeof g_key_names[0]; i++)
        if (!strcasecmp(g_key_names[i].name, n))
            return g_key_names[i].t;
    return 0;
}

static char *trim(char *s)
{
    while (isspace((unsigned char)*s))
        s++;
    for (char *t = s + strlen(s); t > s && isspace((unsigned char)t[-1]);)
        *--t = 0;
    return s;
}

static int is_true(const char *v) { return !strcasecmp(v, "True") || !strcmp(v, "1"); }

// 1 — разобран; 0 — файл старого формата (нет Version=SNX_INI_VERSION)
static int load_settings(const char *text, const char *src)
{
    memset(g_map, 0, sizeof g_map);
    g_layer_btn = -1;
    int pad = 0, version = 0;  // pad: секция [Pad]
    for (const char *p = text; *p;) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        char ln[256];
        snprintf(ln, sizeof ln, "%.*s", (int)(n < sizeof ln ? n : sizeof ln - 1), p);
        p += n + (e ? 1 : 0);
        char *c = strchr(ln, ';'), *eq = strchr(ln, '=');
        if (c)
            *c = 0;
        char *sec = trim(ln);
        if (*sec == '[') {
            pad = !strncasecmp(sec, "[Pad]", 5);
            continue;
        }
        if (!eq || (c && c < eq))
            continue;
        *eq = 0;
        char *k = trim(ln), *v = trim(eq + 1);
        if (!pad) {
            if (!strcasecmp(k, "ShowFPS"))
                d3d8_set_fps_overlay(is_true(v));
            else if (!strcasecmp(k, "Version"))
                version = atoi(v);
            continue;
        }
        if (!strcasecmp(k, "Layer")) {
            g_layer_btn = strcasecmp(v, "None") ? pad_button_of(v) : -1;
            continue;
        }
        if (!strcasecmp(k, "MoveSpeed") || !strcasecmp(k, "CameraSpeed") || !strcasecmp(k, "DeadZone")) {
            float f = (float)atof(v);
            *(!strcasecmp(k, "MoveSpeed") ? &g_move_speed : !strcasecmp(k, "CameraSpeed") ? &g_cam_speed : &g_dead) = f;
            continue;
        }
        if (!strcasecmp(k, "InvertCameraY")) {
            g_invert_y = is_true(v);
            continue;
        }
        char *plus = strchr(k, '+');
        int b = pad_button_of(plus ? plus + 1 : k);
        u32 t = *v ? key_of(v) : 0;
        if (b < 0 || (*v && !t))
            fprintf(stderr, "[input] %s: cannot parse '%s=%s'\n", src, k, v);
        else
            g_map[plus != NULL][b] = t;
    }
    if (g_dead < 0.0f || g_dead > 0.9f)
        g_dead = 0.15f;
    return version == SNX_INI_VERSION;
}

static void input_load(const char *user_dir)
{
    char path[1100], old[1100];
    snprintf(path, sizeof path, "%s/System/Snx.ini", user_dir);
    FILE *f = fopen(path, "rb");
    if (f) {
        char *text = calloc(1, 64 << 10);
        int ok = 0;
        if (text) {
            text[fread(text, 1, (64 << 10) - 1, f)] = 0;
            ok = load_settings(text, path);
        }
        free(text);
        fclose(f);
        if (ok)
            return;
        snprintf(old, sizeof old, "%s.old", path);  // старый формат (клавиши вместо джойстика) — в сторону
        remove(old);
        rename(path, old);
        fprintf(stderr, "[input] %s: old format, saved as %s\n", path, old);
    }
    load_settings(g_snx_default, "default");
    if ((f = fopen(path, "wb"))) {
        fputs(g_snx_default, f);
        fclose(f);
    }
}

void input_init(const char *user_dir)
{
    input_load(user_dir);
    const char *e = getenv("SNX_SHOW_FPS");  // поверх Snx.ini: лаунчер Android («Счётчик FPS»)
    if (e)
        d3d8_set_fps_overlay(*e == '1');
}

static u32 g_joy_buttons;  // кнопки джойстика (биты Joy1..Joy12) и крестовины (биты 16..19: вверх, вниз, влево, вправо)

// Щелчок в точке заднего буфера (B — по кнопке «назад», касание экрана): курсор GUI ставит поток гостя
// (game_menu_point), нажатие — когда GUI уже увидел курсор там (элемент под курсором выбирается заранее),
// отпускание — с кнопкой/пальцем или сразу после нажатия, если их уже отпустили.
static int g_click_state;  // 0 — нет, 1 — курсор подведён, ждём нажатия, 2 — нажата
static int g_click_up;     // кнопка/палец уже отпущены
static unsigned g_click_ticket;

static void sync_cursor(float x, float y)  // курсор хоста (для WM_* и GetCursorPos) — туда же, где курсор GUI
{
    float u, v;
    if (d3d8_target_to_window(x, y, &u, &v)) {
        g_cu = u < 0.0f ? 0.0f : u > 1.0f ? 1.0f : u;
        g_cv = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
    }
}

static void click_at(float x, float y)
{
    if (g_click_state == 2)
        mouse_button(0, 0, cursor_lparam());
    sync_cursor(x, y);
    g_click_ticket = game_menu_point(x, y);
    g_click_state = 1;
    g_click_up = 0;
}

static void click_release(void)
{
    if (g_click_state == 2) {
        mouse_button(0, 0, cursor_lparam());
        g_click_state = 0;
    } else if (g_click_state == 1)
        g_click_up = 1;
}

static void click_tick(void)
{
    if (g_click_state == 1 && game_menu_point_done(g_click_ticket)) {
        mouse_button(0, 1, cursor_lparam());
        g_click_state = 2;
    } else if (g_click_state == 2 && g_click_up) {
        mouse_button(0, 0, cursor_lparam());
        g_click_state = 0;
    }
}

static void touch(float u, float v)  // касание: доли окна -> точка заднего буфера
{
    int x, y;
    if (d3d8_window_to_target(u, v, &x, &y))
        click_at((float)x, (float)y);
}

static void back(int down)
{
    game_rect r;
    if (!down) {
        click_release();
        return;
    }
    if (game_menu_back(&r)) {
        if (getenv("SNX_LOG_MENU"))
            fprintf(stderr, "[menu] back: click (%.0f,%.0f)\n", (r.x0 + r.x1) / 2, (r.y0 + r.y1) / 2);
        click_at((r.x0 + r.x1) / 2, (r.y0 + r.y1) / 2);
        return;
    }
    key(0x1B, 1);
    key(0x1B, 0);
}

// крестовина в меню: у ползунка и выпадающего списка в фокусе влево/вправо — стрелки (значение меняет сам
// элемент), иначе — фокус на соседний элемент
static u32 g_nav_key;            // нажатая крестовиной стрелка
static int g_nav_dir = -1;       // зажатое направление (повтор, как у клавиатуры)
static u32 g_nav_next;           // время следующего повтора, мс

static void nav(int dir, int down)
{
    static const u32 arrows[4] = {0x26, 0x28, 0x25, 0x27};
    int kind = GM_NONE;
    if (!down) {
        if (g_nav_key)
            key(g_nav_key, 0);
        g_nav_key = 0;
        if (dir == g_nav_dir)
            g_nav_dir = -1;
        return;
    }
    if (dir != g_nav_dir) {
        g_nav_dir = dir;
        g_nav_next = SDL_GetTicks() + 400;
    }
    game_menu_open(&kind);
    if (dir >= 2 && (kind == GM_SLIDER || kind == GM_COMBO)) {
        key(g_nav_key = arrows[dir], 1);
        return;
    }
    game_menu_nav(dir);
}

// A: щелчок по элементу в фокусе (курсор GUI уже на нём); ползунок щелчком не трогаем — щелчок ставит
// значение под курсором (середину)
static void select_(int down)
{
    int kind = GM_NONE;
    static int pressed;
    game_menu_open(&kind);
    if (down && kind == GM_SLIDER)
        return;
    if (down || pressed)
        mouse_button(0, down, cursor_lparam());
    pressed = down;
}

static void send_target(int b, u32 t, int down)
{
    switch (t) {
    case 0: break;
    case T_LMB: case T_RMB: case T_MMB: mouse_button((int)(t - T_LMB), down, cursor_lparam()); break;
    case T_WHEEL_UP: if (down) wheel(1); break;
    case T_WHEEL_DOWN: if (down) wheel(-1); break;
    case T_FPS: if (down) d3d8_set_fps_overlay(!d3d8_fps_overlay()); break;
    case T_NAV: nav(b - SDL_CONTROLLER_BUTTON_DPAD_UP, down); break;
    case T_SELECT: select_(down); break;
    case T_BACK: back(down); break;
    case T_TAB_PREV: case T_TAB_NEXT: if (down) game_menu_tab(t == T_TAB_NEXT ? 1 : -1); break;
    case T_JOY: {
        u32 bit = g_pb_joy[b] ? 1u << (g_pb_joy[b] - 1) : 1u << (16 + b - SDL_CONTROLLER_BUTTON_DPAD_UP);
        g_joy_buttons = down ? g_joy_buttons | bit : g_joy_buttons & ~bit;
        break;
    }
    default: key(t, down);
    }
}

// SDL на Switch раздаёт кнопки по положению, как у геймпада Xbox (нижняя — A): нижняя кнопка Switch (B)
// приходит как SDL_CONTROLLER_BUTTON_A. Здесь — по надписям на кнопках.
static int by_label(int b)
{
#ifdef __SWITCH__
    switch (b) {
    case SDL_CONTROLLER_BUTTON_A: return SDL_CONTROLLER_BUTTON_B;
    case SDL_CONTROLLER_BUTTON_B: return SDL_CONTROLLER_BUTTON_A;
    case SDL_CONTROLLER_BUTTON_X: return SDL_CONTROLLER_BUTTON_Y;
    case SDL_CONTROLLER_BUTTON_Y: return SDL_CONTROLLER_BUTTON_X;
    default: break;
    }
#endif
    return b;
}

static int is_dpad(int b) { return b >= SDL_CONTROLLER_BUTTON_DPAD_UP && b <= SDL_CONTROLLER_BUTTON_DPAD_RIGHT; }

static u32 target_of(int b)
{
    if (g_menu) {
        switch (b) {
        case SDL_CONTROLLER_BUTTON_A: return T_SELECT;
        case SDL_CONTROLLER_BUTTON_B: return T_BACK;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return T_TAB_PREV;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return T_TAB_NEXT;
        case SDL_CONTROLLER_BUTTON_START: return 0x1B;
        case SDL_CONTROLLER_BUTTON_BACK: return 0x20;
        default: return is_dpad(b) ? T_NAV : 0;
        }
    }
    if (b == g_layer_btn)
        return 0;
    if (g_layer)
        return g_map[1][b];
    return g_map[0][b] ? g_map[0][b] : g_pb_joy[b] || is_dpad(b) ? T_JOY : 0;
}

static void pad_button(int b, int down)
{
    if (b < 0 || b >= PB_N)
        return;
    if (b == g_layer_btn)
        g_layer = down;
    if (down) {
        if (g_held[b])
            return;
        g_held[b] = target_of(b);
        send_target(b, g_held[b], 1);
    } else {
        send_target(b, g_held[b], 0);
        g_held[b] = 0;
    }
}

// смена режима (меню <-> игра): отпустить всё, что держат кнопки; нажатое заново сработает уже по-новому.
// Кнопки джойстика при входе в меню не отпускаются: событие Joy* в открытом меню переводит GUI игры (в нём
// остался код для Xbox) в режим фокуса с затемнёнными кнопками. Джойстик в меню замирает в прежнем состоянии
// (g_joy_frozen) и отпускается уже по возвращении в игру.
static u32 g_joy_frozen;

static void release_all(void)
{
    u32 joy = g_joy_buttons;
    for (int b = 0; b < PB_N; b++)
        if (g_held[b]) {
            send_target(b, g_held[b], 0);
            g_held[b] = 0;
        }
    g_joy_buttons = 0;
    g_joy_frozen = joy;
}

// стик с круглой мёртвой зоной: (x, y) -> длина 0..1 с тем же направлением; curve — квадратичный отклик
static void stick(float x, float y, int curve, float *ox, float *oy)
{
    float m = sqrtf(x * x + y * y);
    if (m <= g_dead) {
        *ox = *oy = 0.0f;
        return;
    }
    float k = (m - g_dead) / (1.0f - g_dead);
    k = k > 1.0f ? 1.0f : k;
    if (curve)
        k *= k;
    *ox = x / m * k;
    *oy = y / m * k;
}

// SNX_KEYS=<секунда>:<VK hex>,... — нажать и отпустить клавишу в заданное время от старта (проверки без рук:
// например, F12 с привязкой «setres 1024x768» в User.ini); <секунда>:@<u>/<v> — касание экрана в точке окна
// (доли ширины и высоты; в меню); <секунда>:%<кнопка>+ / %<кнопка>- — нажать / отпустить кнопку геймпада
// (имена как в Snx.ini); <секунда>:~<ось>=<-1..1> — стик (LX LY RX RY).
static void scripted_keys(void)
{
    static const char *p;
    static int init;
    static u32 t0, held, clicked;
    if (!init) {
        init = 1;
        t0 = SDL_GetTicks();
        p = getenv("SNX_KEYS");
    }
    if (held) {
        key(held, 0);
        held = 0;
    }
    if (clicked && SDL_GetTicks() - clicked >= 300) {  // палец убран
        click_release();
        clicked = 0;
    }
    double at;
    unsigned vk;
    float u, v;
    int n;
    if (!p || sscanf(p, "%lf:%n", &at, &n) != 1 || (double)(SDL_GetTicks() - t0) < at * 1000)
        return;
    p += n;
    if (sscanf(p, "@%f/%f%n", &u, &v, &n) == 2) {
        fprintf(stderr, "[input] scripted touch %.3f %.3f\n", u, v);
        touch(u, v);
        clicked = SDL_GetTicks();
    } else if (*p == '%' || *p == '~') {  // геймпад
        char name[16], op = 0;
        float val = 0;
        if (*p == '%' && sscanf(p + 1, "%15[A-Za-z]%c%n", name, &op, &n) == 2) {
            fprintf(stderr, "[input] scripted pad %s%c\n", name, op);
            pad_button(pad_button_of(name), op == '+');
        } else if (*p == '~' && sscanf(p + 1, "%2s=%f%n", name, &val, &n) == 2) {
            static const char *const ax[4] = {"LX", "LY", "RX", "RY"};
            for (int i = 0; i < 4; i++)
                if (!strcasecmp(name, ax[i]))
                    g_axis[i] = val;
            fprintf(stderr, "[input] scripted stick %s=%.2f\n", name, val);
        } else {
            p = NULL;
            return;
        }
        n++;
    } else if (sscanf(p, "%x%n", &vk, &n) == 1) {
        fprintf(stderr, "[input] scripted key %02x\n", vk);
        key(vk, 1);
        held = vk;
    } else {
        p = NULL;
        return;
    }
    p += n;
    p = *p == ',' ? p + 1 : NULL;
}

// ---------------------------------------------------------------- сенсорный экран (host/boot/touch.h)
// Пишут потоки лаунчера (JNI), читает input_tick: состояние под замком, применение — только в главном потоке.
static pthread_mutex_t g_touch_lock = PTHREAD_MUTEX_INITIALIZER;
static u32 g_touch_buttons, g_touch_applied;
static float g_touch_axis[4];
static float g_look_x, g_look_y;
static struct {
    short vk;
    char down;
} g_touch_keys[32];
static int g_ntouch_keys;
static volatile int g_touch_menu, g_touch_ready;

void touch_pad(uint32_t buttons, float lx, float ly, float rx, float ry)
{
    pthread_mutex_lock(&g_touch_lock);
    g_touch_buttons = buttons & ((1u << PB_N) - 1);
    g_touch_axis[0] = lx;
    g_touch_axis[1] = ly;
    g_touch_axis[2] = rx;
    g_touch_axis[3] = ry;
    pthread_mutex_unlock(&g_touch_lock);
}

void touch_look(float dx, float dy)
{
    pthread_mutex_lock(&g_touch_lock);
    g_look_x += dx;
    g_look_y += dy;
    pthread_mutex_unlock(&g_touch_lock);
}

void touch_key(int vk, int down)
{
    pthread_mutex_lock(&g_touch_lock);
    if (g_ntouch_keys < 32 && vk > 0 && vk < 256) {
        g_touch_keys[g_ntouch_keys].vk = (short)vk;
        g_touch_keys[g_ntouch_keys].down = (char)(down != 0);
        g_ntouch_keys++;
    }
    pthread_mutex_unlock(&g_touch_lock);
}

int touch_state(void) { return (g_touch_menu ? 1 : 0) | (g_touch_ready ? 2 : 0); }

// оси экранных стиков: поверх физического геймпада — тот, что отклонён сильнее
static float g_touch_ax[4];

static void touch_tick(void)
{
    u32 buttons;
    float look_x, look_y;
    int nkeys;
    short vks[32];
    char downs[32];
    pthread_mutex_lock(&g_touch_lock);
    buttons = g_touch_buttons;
    memcpy(g_touch_ax, g_touch_axis, sizeof g_touch_ax);
    look_x = g_look_x;
    look_y = g_look_y;
    g_look_x = g_look_y = 0;
    nkeys = g_ntouch_keys;
    for (int i = 0; i < nkeys; i++) {
        vks[i] = g_touch_keys[i].vk;
        downs[i] = g_touch_keys[i].down;
    }
    g_ntouch_keys = 0;
    pthread_mutex_unlock(&g_touch_lock);
    int x, y;
    g_touch_ready = d3d8_window_to_target(0.5f, 0.5f, &x, &y);
    u32 diff = buttons ^ g_touch_applied;
    for (int b = 0; diff && b < PB_N; b++)
        if (diff & (1u << b))
            pad_button(b, (buttons >> b) & 1);
    g_touch_applied = buttons;
    for (int i = 0; i < nkeys; i++)
        key((u32)vks[i], downs[i]);
    // камера пальцем: смещение мыши DirectInput (в меню — не нужно, там касание — щелчок)
    static float rx_, ry_;
    if (!g_menu && (look_x != 0.0f || look_y != 0.0f)) {
        rx_ += look_x;
        ry_ += (g_invert_y ? -look_y : look_y);
        int dx = (int)rx_, dy = (int)ry_;
        rx_ -= (float)dx;
        ry_ -= (float)dy;
        if (dx || dy)
            dinput8_mouse_move(dx, dy, 0);
    }
}

static float axis_of(int a)
{
    float p = g_axis[a], t = a < 4 ? g_touch_ax[a] : 0.0f;
    return fabsf(t) > fabsf(p) ? t : p;
}

void input_tick(void)
{
    scripted_keys();
    touch_tick();
    static u64 last;
    static float fx, fy;  // дробные остатки смещения мыши
    u64 now = SDL_GetPerformanceCounter();
    float dt = last ? (float)(now - last) / (float)SDL_GetPerformanceFrequency() : 0.0f;
    last = now;
    if (dt > 0.1f)
        dt = 0.1f;
    click_tick();
    int menu = game_menu_open(NULL);
    g_touch_menu = menu;
    if (menu != g_menu) {
        release_all();
        g_menu = menu;
        g_nav_dir = -1;
        if (!menu)
            g_joy_frozen = 0;
    }
    // курки -> кнопки ZL/ZR
    for (int i = 0; i < 2; i++) {
        int on = g_axis[SDL_CONTROLLER_AXIS_TRIGGERLEFT + i] > (g_trig[i] ? 0.3f : 0.5f);
        if (on != g_trig[i])
            pad_button(PB_ZL + i, g_trig[i] = on);
    }
    float lx, ly, rx, ry;
    stick(axis_of(SDL_CONTROLLER_AXIS_LEFTX), axis_of(SDL_CONTROLLER_AXIS_LEFTY), 0, &lx, &ly);
    stick(axis_of(SDL_CONTROLLER_AXIS_RIGHTX), axis_of(SDL_CONTROLLER_AXIS_RIGHTY), 1, &rx, &ry);
    if (!g_menu) {
        // джойстик: lX lY — левый стик, lRy lRz (JoyU, JoyV) — правый; крестовина — POV
        static const int pov_of[16] = {-1, 0, 18000, -1, 27000, 31500, 22500, 27000,
                                       9000, 4500, 13500, 9000, -1, 0, 18000, -1};
        float ax[6] = {lx * g_move_speed, ly * g_move_speed, 0, 0, rx * g_cam_speed,
                       (g_invert_y ? -ry : ry) * g_cam_speed};
        dinput8_joy(ax, g_joy_buttons & 0xFFF, pov_of[g_joy_buttons >> 16 & 15]);
        return;
    }
    static const float zero[6];
    dinput8_joy(zero, g_joy_frozen & 0xFFF, -1);
    float gx, gy;
    if (game_menu_cursor_moved(&gx, &gy))  // курсор GUI переставлен на элемент в фокусе
        sync_cursor(gx, gy);
    if (g_nav_dir >= 0 && (s32)(SDL_GetTicks() - g_nav_next) >= 0) {
        g_nav_next = SDL_GetTicks() + 120;
        nav(g_nav_dir, 1);
    }
    // меню: оба стика -> курсор (смещения мыши DirectInput); курсор виден, когда им водят
    float cx = fabsf(lx) > fabsf(rx) ? lx : rx, cy = fabsf(ly) > fabsf(ry) ? ly : ry;
    if (cx == 0.0f && cy == 0.0f)
        return;
    game_menu_cursor(1);
    fx += cx * 1200.0f * dt;
    fy += cy * 1200.0f * dt;
    int dx = (int)fx, dy = (int)fy;
    fx -= (float)dx;
    fy -= (float)dy;
    g_cu += cx * 0.7f * dt;
    g_cv += cy * 0.9f * dt;
    g_cu = g_cu < 0.0f ? 0.0f : g_cu > 1.0f ? 1.0f : g_cu;
    g_cv = g_cv < 0.0f ? 0.0f : g_cv > 1.0f ? 1.0f : g_cv;
    if (dx || dy)
        dinput8_mouse_move(dx, dy, 0);
    u32 hwnd = user32_focus();
    if (hwnd)
        user32_post(hwnd, WM_MOUSEMOVE, g_buttons, cursor_lparam());
}

void input_event(const void *p)
{
    const SDL_Event *e = p;
    u32 hwnd = user32_focus();
    switch (e->type) {
    case SDL_QUIT:
        if (hwnd)
            user32_post(hwnd, WM_CLOSE, 0, 0);
        else
            exit(0);
        break;
    case SDL_KEYDOWN:
    case SDL_KEYUP: {
        u32 vk = vk_of(e->key.keysym.scancode);
        if (!vk || !hwnd)
            break;
        int down = e->type == SDL_KEYDOWN, alt = (e->key.keysym.mod & KMOD_ALT) != 0;
        user32_set_key(vk, down);
        u32 lp = 1u | (e->key.repeat ? 1u << 30 : 0) | (down ? 0 : 3u << 30);
        user32_post(hwnd, alt ? (down ? WM_SYSKEYDOWN : WM_SYSKEYUP) : (down ? WM_KEYDOWN : WM_KEYUP), vk, lp);
        break;
    }
    case SDL_TEXTINPUT:
        if (!hwnd)
            break;
        for (const unsigned char *s = (const unsigned char *)e->text.text; *s; s++)
            if (*s < 0x80)  // TODO: UTF-8 -> UTF-16 для не-ASCII
                user32_post(hwnd, WM_CHAR, *s, 1);
        break;
    case SDL_FINGERDOWN:  // сенсорный экран (Switch в руках): в меню — курсор и щелчок, в игре — ничего
    case SDL_FINGERUP:
    case SDL_FINGERMOTION:
        if (!g_menu)
            break;
        if (e->type == SDL_FINGERDOWN)
            touch(e->tfinger.x, e->tfinger.y);
        else if (e->type == SDL_FINGERUP)
            click_release();
        else {  // палец ведут (ползунок): курсор GUI за ним
            int x, y;
            if (d3d8_window_to_target(e->tfinger.x, e->tfinger.y, &x, &y)) {
                sync_cursor((float)x, (float)y);
                game_menu_point((float)x, (float)y);
            }
        }
        break;
    case SDL_MOUSEMOTION:
        if (e->motion.which == SDL_TOUCH_MOUSEID)  // мышь, которую SDL изображает из касаний
            break;
        dinput8_mouse_move(e->motion.xrel, e->motion.yrel, 0);
        if (hwnd)
            user32_post(hwnd, WM_MOUSEMOVE, g_buttons, mouse_lparam(e->motion.x, e->motion.y));
        break;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP: {
        if (e->button.which == SDL_TOUCH_MOUSEID)
            break;
        int b = e->button.button;
        int idx = b == SDL_BUTTON_LEFT ? 0 : b == SDL_BUTTON_RIGHT ? 1 : b == SDL_BUTTON_MIDDLE ? 2 : -1;
        if (idx >= 0)
            mouse_button(idx, e->type == SDL_MOUSEBUTTONDOWN, mouse_lparam(e->button.x, e->button.y));
        break;
    }
    case SDL_MOUSEWHEEL:
        wheel(e->wheel.y);
        break;
    case SDL_CONTROLLERDEVICEADDED:
        SDL_GameControllerOpen(e->cdevice.which);
        break;
    case SDL_CONTROLLERBUTTONDOWN:
    case SDL_CONTROLLERBUTTONUP:
        pad_button(by_label(e->cbutton.button), e->type == SDL_CONTROLLERBUTTONDOWN);
        break;
    case SDL_CONTROLLERAXISMOTION:
        if (e->caxis.axis < SDL_CONTROLLER_AXIS_MAX)
            g_axis[e->caxis.axis] = (float)e->caxis.value / 32767.0f;
        break;
    default:
        break;
    }
}
