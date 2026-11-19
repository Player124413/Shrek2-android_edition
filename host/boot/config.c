// Настройки порта поверх game.ini игры (см. config.h).
#include "host/boot/config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "runtime/abi.h"
#include "runtime/game/menu.h"
#include "runtime/hle/vfs.h"

static char *read_all(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *p = n >= 0 ? malloc((size_t)n + 1) : NULL;
    if (p && fread(p, 1, (size_t)n, f) != (size_t)n) {
        free(p);
        p = NULL;
    }
    fclose(f);
    if (p) {
        p[n] = 0;
        *len = (size_t)n;
    }
    return p;
}

static int write_all(const char *path, const char *a, size_t na, const char *b, size_t nb, const char *c, size_t nc)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    int ok = fwrite(a, 1, na, f) == na && fwrite(b, 1, nb, f) == nb && fwrite(c, 1, nc, f) == nc;
    return fclose(f) == 0 && ok ? 1 : -1;
}

// длина строки без перевода; *next — начало следующей
static size_t line_at(const char *s, size_t len, size_t at, size_t *next)
{
    size_t e = at;
    while (e < len && s[e] != '\n')
        e++;
    *next = e < len ? e + 1 : len;
    return (e > at && s[e - 1] == '\r') ? e - 1 - at : e - at;
}

int boot_ini_set(const char *path, const char *section, const char *key, const char *value)
{
    size_t len = 0;
    char *s = read_all(path, &len);
    if (!s)
        return -1;
    const char *nl = strstr(s, "\r\n") || !len ? "\r\n" : "\n";
    size_t klen = strlen(key), slen = strlen(section);
    size_t at = 0, next, sec_end = (size_t)-1;  // sec_end: где кончается нужная секция (вставка ключа)
    int in_sec = 0;
    char buf[512];
    for (; at < len; at = next) {
        size_t n = line_at(s, len, at, &next);
        const char *l = s + at;
        if (n && l[0] == '[') {
            if (in_sec)
                break;  // секция кончилась, ключа в ней нет
            in_sec = n >= slen + 2 && l[slen + 1] == ']' && !strncasecmp(l + 1, section, slen);
            if (in_sec)
                sec_end = next;
            continue;
        }
        if (!in_sec)
            continue;
        if (n)
            sec_end = next;  // пустые строки в конце секции оставляем после вставки
        if (n > klen && l[klen] == '=' && !strncasecmp(l, key, klen)) {
            size_t vlen = n - klen - 1;
            if (vlen == strlen(value) && !memcmp(l + klen + 1, value, vlen)) {
                free(s);
                return 0;
            }
            int m = snprintf(buf, sizeof buf, "%.*s=%s", (int)klen, l, value);
            int r = write_all(path, s, at, buf, (size_t)m, s + at + n, len - at - n);
            free(s);
            return r;
        }
    }
    int m, r;
    if (sec_end != (size_t)-1) {
        m = snprintf(buf, sizeof buf, "%s=%s%s", key, value, nl);
        if (sec_end == len && len && s[len - 1] != '\n')
            m = snprintf(buf, sizeof buf, "%s%s=%s%s", nl, key, value, nl);
        r = write_all(path, s, sec_end, buf, (size_t)m, s + sec_end, len - sec_end);
    } else {
        m = snprintf(buf, sizeof buf, "%s[%s]%s%s=%s%s", len && s[len - 1] != '\n' ? nl : "", section, nl, key,
                     value, nl);
        r = write_all(path, s, len, buf, (size_t)m, "", 0);
    }
    free(s);
    return r;
}

int boot_ini_get(const char *path, const char *section, const char *key, char *out, size_t cap)
{
    size_t len = 0, slen = strlen(section), klen = strlen(key);
    char *s = read_all(path, &len);
    if (!s)
        return 0;
    int in_sec = 0, found = 0;
    for (size_t at = 0, next; at < len && !found; at = next) {
        size_t n = line_at(s, len, at, &next);
        const char *l = s + at;
        if (n && l[0] == '[')
            in_sec = n >= slen + 2 && l[slen + 1] == ']' && !strncasecmp(l + 1, section, slen);
        else if (in_sec && n > klen && l[klen] == '=' && !strncasecmp(l, key, klen)) {
            snprintf(out, cap, "%.*s", (int)(n - klen - 1), l + klen + 1);
            found = 1;
        }
    }
    free(s);
    return found;
}

static void set(const char *path, const char *section, const char *key, const char *value)
{
    int r = boot_ini_set(path, section, key, value);
    const char *name = strrchr(path, '/');
    if (r > 0)
        fprintf(stderr, "[config] %s: [%s] %s=%s\n", name ? name + 1 : path, section, key, value);
    else if (r < 0)
        fprintf(stderr, "[config] cannot update %s\n", path);
}

// нет файла в каталоге пользователя — копия src из каталога игры (как при первом запуске сделала бы игра)
static void copy_if_missing(const char *dst, const char *src_rel)
{
    char src[1100];
    FILE *f = fopen(dst, "rb");
    if (f) {
        fclose(f);
        return;
    }
    if (!vfs_find(src_rel, src, sizeof src))
        return;
    size_t len = 0;
    char *s = read_all(src, &len);
    if (!s || write_all(dst, s, len, "", 0, "", 0) < 0)
        fprintf(stderr, "[config] cannot create %s\n", dst);
    free(s);
}

// Геймпад как джойстик (runtime/hle/dinput8.c, host/boot/input.c): A B X Y L R ZL ZR Minus Plus LStick RStick —
// Joy1..Joy12, крестовина — Joy13..Joy16 (вверх, вправо, вниз, влево), стики — JoyX/JoyY и JoyU/JoyV. Оси
// без SpeedBase: UInput::DirectAxis дописывает «Speed=<значение оси>», и вклад не зависит от частоты кадров;
// множители (бег 1200, как у W) хост закладывает в само значение оси. ZL — кнопка слоя хоста (зелья).
static const char *const g_joy_bindings[][2] = {
    {"Joy1", "Fire"},                                // A — удар
    {"Joy2", "Jump"},                                // B — прыжок
    {"Joy3", "Use"},                                 // X — действие
    {"Joy4", "CenterView"},                          // Y — камера за спину
    {"Joy5", "PrevSkill | CameraSpeedDown"},         // L — как колесо мыши
    {"Joy6", "NextSkill | CameraSpeedUp"},           // R
    {"Joy8", "Use | Fire"},                          // ZR — как Enter
    {"Joy9", "BypassCutscene | StopMoviePlayback"},  // Minus — пропустить ролик
    {"Joy10", "EscHandler"},                         // Plus — меню
    {"Joy12", "CenterView"},                         // RStick
    {"Joy13", "Axis aArrowUp SpeedBase=0.7"},        // крестовина — как стрелки
    {"Joy14", "Axis aArrowRight SpeedBase=0.9 Invert=1"},
    {"Joy15", "Axis aArrowUp SpeedBase=0.7 Invert=-1"},
    {"Joy16", "Axis aArrowRight SpeedBase=0.9 Invert=-1"},
    {"JoyX", "Axis aStrafe"},
    {"JoyY", "Axis aBaseY Invert=-1"},
    {"JoyU", "Axis aMouseX"},
    {"JoyV", "Axis aMouseY Invert=-1"},
};

// Действия вкладки GAME (Tab_OptionsGame.AliasNames1). Переназначение клавиш там в порту выключено
// (runtime/game/menu.c), но прежние сборки его пропускали: ячейка ждала нажатия, и кнопка геймпада забирала
// действие, а у прежних клавиш (W, Ctrl, Joy2...) оно стиралось. Тогда игра пишет «назначьте клавиши во всех
// ячейках», а персонаж не слушается. Признак — клавиша из DefUser.ini потеряла своё действие из этого списка;
// лечение — привязки [Engine.Input] из DefUser.ini (а привязки геймпада — заново, ниже).
static const char *const g_game_actions[] = {"MoveForward", "MoveBackward", "StrafeLeft", "StrafeRight", "Jump", "Fire"};

static int has_ci(const char *s, const char *sub)  // подстрока без учёта регистра (как InStr(Caps(..)) у игры)
{
    for (size_t n = strlen(sub); *s; s++)
        if (!strncasecmp(s, sub, n))
            return 1;
    return 0;
}

// пары ключ=значение секции (без Aliases[]); cb возвращает 0 — стоп
static void ini_each(const char *path, const char *section, int (*cb)(const char *k, const char *v, void *ctx),
                     void *ctx)
{
    size_t len = 0, slen = strlen(section);
    char *s = read_all(path, &len), k[128], v[512];
    int in_sec = 0;
    for (size_t at = 0, next; s && at < len; at = next) {
        size_t n = line_at(s, len, at, &next);
        const char *l = s + at, *eq = memchr(l, '=', n);
        if (n && l[0] == '[')
            in_sec = n >= slen + 2 && l[slen + 1] == ']' && !strncasecmp(l + 1, section, slen);
        else if (in_sec && eq && eq > l && (size_t)(eq - l) < sizeof k && strncasecmp(l, "Aliases[", 8)) {
            snprintf(k, sizeof k, "%.*s", (int)(eq - l), l);
            snprintf(v, sizeof v, "%.*s", (int)(n - (size_t)(eq - l) - 1), eq + 1);
            if (!cb(k, v, ctx))
                break;
        }
    }
    free(s);
}

typedef struct {
    const char *user, *def;
    int lost;
} repair_ctx;

static int lost_action(const char *k, const char *v, void *ctx)
{
    repair_ctx *r = ctx;
    char cur[512] = "";
    for (size_t i = 0; i < sizeof g_game_actions / sizeof g_game_actions[0]; i++)
        if (has_ci(v, g_game_actions[i]) &&
            !(boot_ini_get(r->user, "Engine.Input", k, cur, sizeof cur) && has_ci(cur, g_game_actions[i]))) {
            fprintf(stderr, "[config] User.ini: %s lost %s (now \"%s\")\n", k, g_game_actions[i], cur);
            r->lost = 1;
            return 0;
        }
    return 1;
}

static int restore_key(const char *k, const char *v, void *ctx)
{
    repair_ctx *r = ctx;
    char first[512];
    // ключи без учёта регистра, а в DefUser.ini есть повторы (L= и l=): строку пишет только первое вхождение
    if (boot_ini_get(r->def, "Engine.Input", k, first, sizeof first) && !strcmp(first, v))
        set(r->user, "Engine.Input", k, v);
    return 1;
}

static void repair_bindings(const char *user)
{
    char def[1100];
    repair_ctx r = {user, def, 0};
    if (!vfs_find("System/DefUser.ini", def, sizeof def))
        return;
    ini_each(def, "Engine.Input", lost_action, &r);
    if (r.lost) {
        fprintf(stderr, "[config] User.ini: key bindings damaged by the GAME tab, restoring DefUser.ini\n");
        ini_each(def, "Engine.Input", restore_key, &r);
    }
}

// ---------------------------------------------------------------- подсказки под геймпад
// Тексты игры (меню, субтитры реплик-подсказок) говорят о мыши и клавиатуре. Порт кладёт в каталог
// пользователя копии .int (и .rus русской версии) из каталога игры с заменой фраз (оверлей VFS читается раньше
// игры); голос остаётся прежним. Замены (UTF-8) — подстроки, по порядку: сначала целые фразы, потом общие обороты.
typedef const char *const prompt_rule[2];
static prompt_rule g_prompt_rules[] = {
    // меню
    {"To jump: Press Control key or Right Mouse Button.", "To jump: Press B."},
    {"Press Control key or Right Mouse Button.", "Press B."},
    {"Press Escape to view Main Menu.", "Press + to view Main Menu."},
    {"CLICK A BOX BELOW TO START OR LOAD A GAME", "SELECT A BOX BELOW TO START OR LOAD A GAME"},
    {"Click to ", "Select to "},
    {"Use the slider to adjust the speed of your mouse", "Use the slider to adjust the camera speed"},
    {"=MOUSE SPEED", "=LOOK SPEED"},
    {"Use the Arrow Keys to move", "Use the Left Stick to move"},
    {"Push the Up Arrow or W Key to move forward", "Push the Left Stick up to move forward"},
    {"Push the Down Arrow or S Key to move backwards", "Push the Left Stick down to move backwards"},
    {"Push the Left and Right Arrows to turn", "Push the Left Stick left and right to turn"},
    {"Push the A Key to move to the Left", "Push the Left Stick left to move to the left"},
    {"Push the D Key to move to the Right", "Push the Left Stick right to move to the right"},
    {"Press Enter to skip cutscene", "Press - to skip cutscene"},
    // реплики: целые фразы
    {"just click your right mouse button and push forward on your arrow key to jump",
     "just press B and push the left stick forward to jump"},
    {"click the right mouse button twice, while pushing forward on your arrow key",
     "press B twice, while pushing the left stick forward"},
    {"Use the arrow keys to move around.", "Use the left stick to move around."},
    {"by clicking your right mouse button - or your control key - twice. Click, click alright?",
     "by pressing B twice. Press, press, alright?"},
    {"Just click your right mouse button twice.", "Just press B twice."},
    {"Click your right mouse button once to jump", "Press B once to jump"},
    {"click the right mouse button again", "press B again"},
    {"hit the control key or your right mouse button while your running forward", "press B while you're running forward"},
    {"use your left and right arrows to shimmy", "push the left stick left and right to shimmy"},
    {"hit the down arrow", "push the left stick down"},
    {"just hit the escape button on your keyboard", "just press the + button"},
    {"Hit your right mouse button to jump", "Press B to jump"},
    {"Click the right mouse button to make me jump", "Press B to make me jump"},
    {"Hit your space bar to bring up your potion menu.", "Press the - button to bring up your potion menu."},
    {"To bring up your potion menu just hit your space bar.", "To bring up your potion menu just press the - button."},
    {"by hitting the key on your number pad that corresponds to that potion",
     "by holding ZL and pressing the button that corresponds to that potion"},
    {"You can also use the 1-9 numbers on your keyboard to use a potion, but you have to be smart enough to know "
     "which number corresponds to which potion.",
     "You can also hold ZL and press a button to use a potion, but you have to be smart enough to know which button "
     "corresponds to which potion."},
    {"Just click on the potion", "Just select the potion"},
    {"Click on the potion", "Select the potion"},
    {"Click 'cancel'", "Select 'cancel'"},
    {"Click 'check out'", "Select 'check out'"},
    {"Click 'use'", "Select 'use'"},
    {"Left mouse.  Two times.", "A button.  Two times."},
    // реплики: обороты
    {"clicking your left mouse button", "pressing A"},
    {"Click on your left mouse button", "Press A"},
    {"Click on the left mouse button", "Press A"},
    {"Click your left mouse button", "Press A"},
    {"click your left mouse button", "press A"},
    {"click the left mouse button", "press A"},
    {"press the left mouse button", "press A"},
    {"hitting your left mouse button", "pressing A"},
    {"hittin' your left mouse button", "hittin' A"},
    {"Hit your left mouse button", "Press A"},
    {"hit your left mouse button", "press A"},
    {"Use your left mouse button", "Press A"},
    {"use your left mouse button", "press A"},
    {"with your left mouse button", "with A"},
    {"your left mouse button", "A"},
    {"the left mouse button", "A"},
    {"your right mouse button", "B"},
    {"the right mouse button", "B"},
};

// русская версия («Новый Диск», файлы .Rus)
static prompt_rule g_prompt_rules_rus[] = {
    // меню
    {"Чтобы прыгнуть, нажмите [Ctrl] или правую кнопку мыши.", "Чтобы прыгнуть, нажмите B."},
    {"нажмите [Ctrl] или правую кнопку мыши.", "нажмите B."},
    {"Нажмите [Esc], чтобы вызвать игровое меню.", "Нажмите +, чтобы вызвать игровое меню."},
    {"ЧТОБЫ НАЧАТЬ ИГРУ, НАЖМИТЕ НА ЯЧЕЙКУ", "ЧТОБЫ НАЧАТЬ ИГРУ, ВЫБЕРИТЕ ЯЧЕЙКУ"},
    {"Используйте ползунок, чтобы настроить скорость реакции мыши.", "Используйте ползунок, чтобы настроить скорость камеры."},
    {"=СКОРОСТЬ МЫШИ", "=СКОРОСТЬ КАМЕРЫ"},
    {"=Скорость мыши", "=Скорость камеры"},
    {"Чтобы двигаться, используйте клавиши со стрелками", "Чтобы двигаться, используйте левый стик"},
    {"Чтобы пройти вперед, нажмите стрелку вверх или клавишу [W]", "Чтобы пройти вперед, наклоните левый стик вверх"},
    {"Чтобы отойти назад, нажмите стрелку вниз или клавишу [S]", "Чтобы отойти назад, наклоните левый стик вниз"},
    {"Что бы повернуться, нажмите клавиши стрелок влево и вправо", "Чтобы повернуться, наклоните левый стик влево или вправо"},
    {"Чтобы сдвинуться влево, нажмите клавишу [A]", "Чтобы сдвинуться влево, наклоните левый стик влево"},
    {"Чтобы сдвинуться вправо нажмите клавишу [D]", "Чтобы сдвинуться вправо, наклоните левый стик вправо"},
    {"Нажмине [Enter], что бы пропустить ролик", "Нажмите -, чтобы пропустить ролик"},
    // реплики: целые фразы
    {"просто щелкни правой кнопкой мыши и нажми стрелку вперед", "просто нажми B и наклони левый стик вперед"},
    {"зажми стрелку вперед и два раза щелкни правой кнопкой мыши", "наклони левый стик вперед и два раза нажми B"},
    {"Используй клавиши со стрелками.", "Используй левый стик."},
    {"если два раза щелкнуть правой кнопкой мыши или нажать [Ctrl], получится прыжок высокий. Щелк-щелк, ясно?",
     "если два раза нажать B, получится прыжок высокий. Раз-два, ясно?"},
    {"Просто щелкни два раза правой кнопкой мыши.", "Просто нажми B два раза."},
    {"Щелкаешь правой кнопкой мыши, подпрыгиваешь, а когда будешь в верхней точке — щелкаешь еще раз.",
     "Нажимаешь B, подпрыгиваешь, а когда будешь в верхней точке — нажимаешь еще раз."},
    {"Левой кнопкой мыши щелкнешь — пробьешь доски.", "Кнопку A нажмешь — пробьешь доски."},
    {"Одной левой кнопкой мыши.", "Одной кнопкой A."},
    {"Левой кнопкой мыши его! Левой кнопкой мыши!", "Кнопкой A его! Кнопкой A!"},
    {"нажми на бегу клавишу [Ctrl] или щелкни правой кнопкой мыши", "нажми на бегу B"},
    {"Теперь стрелками влево и вправо перемещайся по ней.", "Теперь левым стиком влево и вправо перемещайся по ней."},
    {"Нажми стрелку вниз", "Наклони левый стик вниз"},
    {"нажми стрелку вниз", "наклони левый стик вниз"},
    {"нажми [Esc] — появится меню", "нажми + — появится меню"},
    {"нажми правую кнопку мыши, а потом сразу левую", "нажми B, а потом сразу A"},
    {"Щелкни правой кнопкой мыши, чтоб подскочил я, и сразу кнопку левую нажми.", "Нажми B, чтоб подскочил я, и сразу A нажми."},
    {"Подходи к ним и щелкай левой кнопкой мыши — ты их поднимешь. А потом еще раз щелкай",
     "Подходи к ним и нажимай A — ты их поднимешь. А потом еще раз нажимай"},
    {"нажмите пробел.", "нажмите кнопку -."},
    {"Каждому соответствует клавиша с цифрой. Одно нажатие — и вы его используете!",
     "Каждому соответствует кнопка: зажмите ZL и нажмите ее — и вы его используете!"},
    {"можете нажимать на клавиши с цифрами, только запомните хорошенько, какой цифре",
     "можете зажать ZL и нажимать кнопки, только запомните хорошенько, какой кнопке"},
    {"два раза левой кнопкой мышки щелкнуть", "два раза кнопку A нажать"},
    {"щелкни два раза кнопкой мыши", "нажми два раза A"},
    {"Щелкни два раза левой кнопкой мыши", "Нажми два раза A"},
    {"щелкни три раза подряд левой кнопкой мыши", "нажми три раза подряд A"},
    // реплики: обороты
    {"Щелкни левой кнопкой мыши", "Нажми A"},
    {"щелкни левой кнопкой мыши", "нажми A"},
    {"щелкнуть левой кнопкой мыши", "нажать A"},
    {"щелкая левой кнопкой мыши", "нажимая A"},
    {"щелкай левой кнопкой мыши", "нажимай A"},
    {"Нажми левую кнопку мыши", "Нажми A"},
    {"нажми левую кнопку мыши", "нажми A"},
    {"нажимай левую кнопку мыши", "нажимай A"},
    {"жми на левую кнопку мыши", "жми на A"},
    {"левой кнопкой мыши", "кнопкой A"},
    {"левую кнопку мыши", "A"},
    {"правой кнопкой мыши", "кнопкой B"},
    {"правую кнопку мыши", "B"},
};

static const struct {
    const char *file;
    const prompt_rule *rules;
    size_t n;
} g_prompt_files[] = {
    {"System/HpMenu.int", g_prompt_rules, sizeof g_prompt_rules / sizeof g_prompt_rules[0]},
    {"System/HpDialog.int", g_prompt_rules, sizeof g_prompt_rules / sizeof g_prompt_rules[0]},
    {"System/BumpDialog.int", g_prompt_rules, sizeof g_prompt_rules / sizeof g_prompt_rules[0]},
    {"System/HpMenu.rus", g_prompt_rules_rus, sizeof g_prompt_rules_rus / sizeof g_prompt_rules_rus[0]},
    {"System/HpDialog.rus", g_prompt_rules_rus, sizeof g_prompt_rules_rus / sizeof g_prompt_rules_rus[0]},
    {"System/BumpDialog.rus", g_prompt_rules_rus, sizeof g_prompt_rules_rus / sizeof g_prompt_rules_rus[0]},
};

// UTF-8 -> UTF-16 (BMP); *n — длина в символах; NULL — нет памяти
static u16 *u16_of(const char *s, size_t *n)
{
    size_t len = strlen(s), k = 0;
    u16 *out = malloc((len + 1) * 2);
    if (!out)
        return NULL;
    for (const u8 *p = (const u8 *)s; *p;) {
        if (*p < 0x80)
            out[k++] = *p++;
        else if ((*p & 0xE0) == 0xC0 && p[1]) {
            out[k++] = (u16)((p[0] & 0x1F) << 6 | (p[1] & 0x3F));
            p += 2;
        } else if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) {
            out[k++] = (u16)((p[0] & 0x0F) << 12 | (p[1] & 0x3F) << 6 | (p[2] & 0x3F));
            p += 3;
        } else
            p++;
    }
    *n = k;
    return out;
}

typedef struct {
    u16 *from, *to;
    size_t nfrom, nto;
} wrule;

// UTF-16 текст (без BOM) с заменами -> новый буфер; *out_n — длина в символах
static u16 *replace_all(const u16 *s, size_t n, const wrule *rules, size_t nr, size_t *out_n)
{
    size_t cap = n + n / 4 + 64, m = 0;
    u16 *out = malloc(cap * 2);
    if (!out)
        return NULL;
    for (size_t i = 0; i < n;) {
        size_t r = 0;
        for (; r < nr; r++)
            if (s[i] == rules[r].from[0] && i + rules[r].nfrom <= n &&
                !memcmp(s + i, rules[r].from, rules[r].nfrom * 2))
                break;
        size_t add = r < nr ? rules[r].nto : 1;
        if (m + add >= cap) {
            u16 *t = realloc(out, (cap = cap * 2 + add) * 2);
            if (!t) {
                free(out);
                return NULL;
            }
            out = t;
        }
        if (r < nr) {
            memcpy(out + m, rules[r].to, add * 2);
            m += add;
            i += rules[r].nfrom;
        } else
            out[m++] = s[i++];
    }
    *out_n = m;
    return out;
}

static void prompts_apply(void)
{
    for (size_t f = 0; f < sizeof g_prompt_files / sizeof g_prompt_files[0]; f++) {
        char src[1100], dst[1100];
        const char *file = g_prompt_files[f].file;
        size_t len = 0, olen = 0, n = 0, nr = g_prompt_files[f].n;
        if (!vfs_find_game(file, src, sizeof src))
            continue;
        char *s = read_all(src, &len);
        if (!s || len < 2 || (u8)s[0] != 0xFF || (u8)s[1] != 0xFE) {  // ожидается UTF-16LE с BOM
            free(s);
            continue;
        }
        wrule *rules = calloc(nr, sizeof *rules);
        int ok = rules != NULL;
        for (size_t r = 0; ok && r < nr; r++) {
            rules[r].from = u16_of(g_prompt_files[f].rules[r][0], &rules[r].nfrom);
            rules[r].to = u16_of(g_prompt_files[f].rules[r][1], &rules[r].nto);
            ok = rules[r].from && rules[r].to && rules[r].nfrom;
        }
        u16 *in = ok ? malloc(len) : NULL, *out = NULL;
        if (in) {
            memcpy(in, s, len);
            out = replace_all(in + 1, len / 2 - 1, rules, nr, &n);
        }
        free(in);
        for (size_t r = 0; rules && r < nr; r++) {
            free(rules[r].from);
            free(rules[r].to);
        }
        free(rules);
        if (!out) {
            free(s);
            continue;
        }
        // на диск — только если копия в оверлее отличается (или её нет)
        vfs_for_write(file, dst, sizeof dst, 0);
        char *old = read_all(dst, &olen);
        int same = old && olen == 2 + n * 2 && !memcmp(old, s, 2) && !memcmp(old + 2, out, n * 2);
        if (!same && write_all(dst, s, 2, (const char *)out, n * 2, "", 0) < 0)
            fprintf(stderr, "[config] cannot write %s\n", dst);
        else if (!same)
            fprintf(stderr, "[config] %s: prompts for the gamepad\n", file);
        free(old);
        free(out);
        free(s);
    }
}

void boot_config_apply(int res_w, int res_h)
{
    char ini[1100];
    vfs_for_write("System/game.ini", ini, sizeof ini, 1);  // game.ini из каталога игры, если он там есть
    FILE *f = fopen(ini, "rb");
    if (f)
        fclose(f);
    else  // первый запуск: игра сама скопировала бы Default.ini (мастер первого запуска — по ключу FirstRun)
        copy_if_missing(ini, "System/Default.ini");
    // ReduceMouseLag: D3DDrv каждый кадр блокирует задний буфер ради ожидания GPU — в порте это чтение из GL
    // и простой GPU/CPU (на маке ≈27 % времени потока гостя)
    set(ini, "D3DDrv.D3DRenderDevice", "ReduceMouseLag", "False");
    if (res_w > 0 && res_h > 0) {
        char w[16], h[16];
        snprintf(w, sizeof w, "%d", res_w);
        snprintf(h, sizeof h, "%d", res_h);
        set(ini, "WinDrv.WindowsClient", "FullscreenViewportX", w);
        set(ini, "WinDrv.WindowsClient", "FullscreenViewportY", h);
        set(ini, "WinDrv.WindowsClient", "StartupFullscreen", "True");
    }
    set(ini, "WinDrv.WindowsClient", "UseJoystick", "True");
    char lang[16] = "int";
    boot_ini_get(ini, "Engine.Engine", "Language", lang, sizeof lang);
    game_menu_set_language(lang);  // подписи меню, которые меняет порт (runtime/game/menu.c)
    // User.ini: привязки геймпада — там, где пусто (свои привязки игрока не трогаем)
    char user[1100], cur[256];
    vfs_for_write("System/User.ini", user, sizeof user, 1);
    copy_if_missing(user, "System/DefUser.ini");
    {  // диагностика «второго запуска»: файлы, записанные игрой в прошлый раз (обрезанный ini — без привязок)
        size_t ul = 0, gl = 0;
        char *u = read_all(user, &ul), *g = read_all(ini, &gl);
        fprintf(stderr, "[config] User.ini %zu bytes (Aliases %d, Joy1 %d), game.ini %zu bytes\n", ul,
                u ? has_ci(u, "Aliases[") : -1, u ? has_ci(u, "Joy1=") : -1, gl);
        free(u);
        free(g);
    }
    repair_bindings(user);
    for (size_t i = 0; i < sizeof g_joy_bindings / sizeof g_joy_bindings[0]; i++) {
        const char *k = g_joy_bindings[i][0];
        if (!boot_ini_get(user, "Engine.Input", k, cur, sizeof cur) || !cur[strspn(cur, " \t")])
            set(user, "Engine.Input", k, g_joy_bindings[i][1]);
    }
    prompts_apply();
    vfs_changed();  // game.ini, User.ini и .int могли появиться в оверлее
}
