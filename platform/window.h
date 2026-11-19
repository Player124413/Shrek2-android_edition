// Окно игры и GL-контекст поверх SDL2 (mac, Switch, Android).
// SDL требует создавать окна и разбирать события в главном потоке (macOS), а гость (и его D3D-устройство)
// исполняется в своём потоке. Поэтому главный поток крутит plat_run(): события SDL и запросы гостя;
// поток гостя просит окно через plat_window_open() и получает GL-контекст текущим у себя.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

int plat_init(void);       // главный поток: SDL (видео, контроллеры); 0 — успех
void plat_run(void);       // главный поток: события и запросы, пока не plat_quit()
void plat_quit(void);      // любой поток

// Любой поток: создать (или изменить) окно w x h с GL-контекстом. 0 — успех.
int plat_window_open(int w, int h, int fullscreen, const char *title);
// Сделать контекст текущим у вызывающего потока (тот, кто рисует), при первом вызове — загрузить функции GL.
int plat_gl_bind(void);
int plat_window_is_open(void);
void plat_screen_size(int *w, int *h);    // после plat_init: режим экрана (Switch: 1280x720 / 1920x1080)
void plat_drawable_size(int *w, int *h);  // размер кадра окна в пикселях
void plat_swap(void);                     // показать кадр (поток с текущим контекстом)

// Событие SDL (SDL_Event *) — в главном потоке, для перевода в ввод гостя.
typedef void (*plat_event_fn)(const void *sdl_event);
extern plat_event_fn plat_on_event;
// Каждый проход цикла главного потока (не реже ~200 раз в секунду): непрерывный ввод (стики).
extern void (*plat_on_tick)(void);

// Приложение уходит в фон / возвращается (Android: сворачивание, блокировка экрана). Обработчики зовутся в главном
// потоке; до 8 штук (звук, ввод). Поток рендера на это время отпускает контекст GL и ждёт в plat_swap.
typedef void (*plat_pause_fn)(int paused);
void plat_add_pause_hook(plat_pause_fn fn);
int plat_paused(void);

#ifdef __cplusplus
}
#endif
