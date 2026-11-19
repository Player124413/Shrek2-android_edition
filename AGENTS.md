# AGENTS.md

Инструкции для ИИ-агентов, работающих в этом репозитории.

shrekNX — статический порт **Shrek 2 для PC (KnowWonder, 2004, Unreal Engine 2)** на Nintendo Switch (homebrew
NRO). Нативный x86-32 код игры переводится в C (`recomp/`), Win32/Direct3D 8/DirectInput/OpenAL/Bink реализует
слой HLE (`runtime/`). Основная машина — macOS arm64, цель — Switch (devkitA64, libnx, SDL2, Mesa).

Пользовательские документы — `README.md`, `docs/BUILDING.md`, `docs/CONSOLE.md`, `CHANGELOG.md`. Обновляй их,
когда меняется поведение, сборка или управление.

---

## Правила

1. **Не изменяй `game/`, `Shrek2RUS/`, `original/`** — это локальная копия игры. Всё рабочее пиши в `build/`.
2. **Не добавляй в git ничего из игры и ничего, полученного из неё:** файлы игры, сгенерированный C (`build/gen*`),
   дампы пакетов, дизассемблерные листинги, NRO/ELF, профили PGO, сейвы, логи, снимки экрана. Перед каждым
   коммитом запускай `python3 -I tools/check_public.py` и просматривай дифф: проверка ловит пути и форматы, но не
   происхождение текста.
3. **Не используй утёкшие исходники Unreal Engine.** Можно: дизассемблировать бинарники локальной копии,
   публичные заголовки UT2003/UT2004 для модов, документацию Epic, открытый код с совместимой лицензией — с
   указанием источника. Wine (`dlls/d3d8`, `wined3d`) и DXVK читай как спецификацию, код не копируй.
4. **Данные игры недоверенные.** Python, который их читает, запускай с `-I`, пути передавай аргументами,
   скрипты держи вне каталогов с данными. `game.exe` не запускай нативно.
5. **Не выполняй `sudo`, установку системных пакетов и запуск на Switch.** Подготовь точную команду и попроси
   выполнить её. Не отправляй ничего в удалённые репозитории без явной
   просьбы.
6. **Не распространяй бинарники и сгенерированный код** ни в коммитах, ни в CI, ни в выпусках: в них переведённый
   код игры.
7. **Не выдавай догадку за факт.** Непроверенное помечай «[проверить]». Результаты macOS и Switch указывай
   раздельно; поведение консоли по проверке на маке не утверждай.
8. **Адреса пиши с модулем и экспортным именем** по нашей карте: `Engine.dll!0x12120209 (UGameEngine::LoadMap)`.
9. Код, комментарии, документация и сообщения коммитов — по-русски.
10. Приоритет — «работает как на ПК»; адаптация под геймпад и широкий экран — после. Английская версия — основная,
    русская собирается отдельно.

---

## Проверка изменений

* **Всегда (без игры, как в CI):**
  ```sh
  tools/build.sh mac && ctest --test-dir build/mac --output-on-failure
  python3 -I tests/test_public.py && python3 -I tools/check_public.py
  ```
* **Если есть `game/`** — перегенерируй затронутые модули и запусти сценарий:
  ```sh
  tools/.venv/bin/python -I recomp/main.py gen --game game/System --out build/gen [модули]
  tools/build.sh mac snx_boot
  build/mac/host/boot/snx_boot --user build/userdata --seconds 90 -- 1_Shreks_Swamp.unr
  ```
  Ввод без рук — `SNX_KEYS` (ниже). Нет `game/` — скажи, что игровое поведение не проверено.
* **Рекомпилятор** — дифф-тесты всех модулей (`tests/difftest/run_all.sh`). Критерий: те же числа прошедших и тот
  же список расхождений, что до правки. Для изменений обмена регистрами — ещё `SNX_REGCHECK=1` при генерации.
* **Производительность** — замер на сборке без трассировки (`build/mac-rel`, ниже) по `SNX_PERF=1`, чередуя
  прогоны со сборкой до правки. Предлагай только крупные выигрыши, подтверждённые замером. Замер на Switch выполняется
  вручную: приложи к команде сцену, разрешение и режим сборки (PGO, `SNX_PIN`).
* **Новая логика** — тест в `tests/rt` (синтетика, CTest) или сценарий запуска. Каждое исправление закрепляй тестом.
* После заметных изменений поправь раздел «Состояние» этого файла и, если это видно пользователю, `CHANGELOG.md`.

---

## Карта репозитория

| Путь | Что там |
|---|---|
| `recomp/` | `main.py` (CLI `discover`/`gen`), `image.py`, `x86.py`, `x87.py`, `discover.py`, `flags.py`, `regs.py`, `emit.py`, `gen.py`, `overrides.py`, `addrmap.json` |
| `runtime/` | `abi.h` (CPU, память, `RT_ICALL`), `gen_rt.h` (макросы сгенерированного кода), `rt.c`, `loader.c`, `vmem.c`, `heap.c`, `process.c`, `overrides_{crt,core,engine}.c`, `crt_math.h` |
| `runtime/hle/` | kernel32 (`kernel32.c`, `kernel32_file.c`, `vfs.c`), user32, gdi32, advapi32 (+winmm, ole32, shell32), `misc.c`, `wsock32.c`, `com.c`, ddraw, `d3d8*.c`, `gl_cs.c`, `overlay.c`, dinput8, `openal.c`, `vorbisfile.c`, `bink*.c` |
| `runtime/game/` | знание о структурах UE2: меню с геймпада (`menu.c`), обход UObject (`uobj.c`) |
| `host/boot/` | `main.c` (запуск, ловушки, модальные окна), `input.c` (SDL → WM/DirectInput, `Snx.ini`, `SNX_KEYS`), `config.c` (`game.ini`, `User.ini`), `prof.c` |
| `host/hello/`, `platform/` | проверка GL/DXT1; окно SDL и контекст GL; `gles_shim.{h,c}` — GL 4.1 поверх ES 3.0, `android/` — потоки и ADPF |
| `host/boot/android.c`, `touch.h` | JNI экранного управления (Kotlin → `input.c`) |
| `android/` | проект Gradle: лаунчер, редактор раскладки, `GameActivity` (SDLActivity) на Kotlin — `docs/ANDROID.md` |
| `tests/` | `rt/` (CTest), `bink/` (сверка с ffmpeg), `difftest/` (unicorn, нужна игра), `test_public.py` |
| `tools/` | `build.sh`, `check_public.py`, `switch_upload.py`, `pgo_fetch.py`, `make_rus.sh`, `make_icon.sh`, `cc_partition.sh`, `probe/` (профили, дизассемблер), `recon/` (PE, MSI, пакеты `.u`) |

---

## Игра

* UE2, ветка KnowWonder: пакеты версии 129, licensee 0. Логика — байткод UnrealScript в `.u` (переносится как
  данные). Нативный код — x86-32, MSVC 7.0, CRT `msvcr70.dll`, без RTTI; рендер D3D8 без D3DX.
* Рекомпилируются 12 модулей: `game.exe` (лаунчер, **без `.reloc`**), `Core`, `Engine`, `D3DDrv` (SSE/MMX/3DNow!
  по CPUID), `Window`, `WinDrv`, `GUI`, `IpDrv`, `Fire`, `KWGame`, `ALAudio`, `msvcr70`.
* Заменяются хостом: ogg/vorbis/vorbisfile → libvorbisfile, `DefOpenAL32` → OpenAL, `binkw32` → свой контейнер
  и декодер звука + libavcodec, `IFC23` → заглушки. Не используются: `Editor`, `PixoDrv` (генерирует код на лету),
  `bink.exe`, `binkmake`, `radutil`, .NET-библиотеки.
* `game.exe` — распакованный лаунчер без SafeDisc (2004-04-14; DLL — 2004-04-21). Защищённый `Game.exe` зашифрован.
* Карты: `SH2_Preamble` (первая), `Book_FrontEnd` (меню), `1_Shreks_Swamp` (первый уровень).
  `Engine.NullRenderDevice` — headless; `-nosound` — без звука.
* Русская версия: DLL 2004-12-13, те же исходники, `.text` совпадает (кроме GUI: +0x220 байт), данные сдвинуты,
  экспорты те же. Генерируется отдельно (`SNX_VARIANT=rus`, `build/gen-rus`), лаунчер — английский. Текст
  рисуется через GDI (TrueType `Tahoma` из `SAPFont.rus`). `ImportText: Bad termination` при разборе её
  `Default.ini` — так же на ПК, не исправляй.

---

## Архитектура и инварианты

**Карта адресов** (`recomp/addrmap.json`, окно гостя 1 ГБ = `RT_WINDOW`): PEB `0x00010000`, TEB
`0x00011000 + 0x1000·i`, трамплины HLE с `0x00100000`; стеки и VirtualAlloc `0x01000000–0x0FFFFFFF`; образы
`0x10000000–0x1FFFFFFF`; куча `0x20000000–0x3FFFFFFF`. Базы: Core 0x10100000, Fire 0x10500000, IpDrv
0x10700000, game.exe 0x10900000, Window 0x11000000, WinDrv 0x11100000, Engine 0x12000000, D3DDrv 0x12800000,
GUI 0x12A00000, KWGame 0x12B00000, ALAudio 0x12C00000, msvcr70 0x13000000. Всё должно оставаться ниже 1 ГБ.

**Память и потоки.** Мак: резерв 4 ГБ `PROT_NONE`. Switch: окно — один блок кучи 1 ГБ (нужен полный режим
памяти), страница NULL закрыта, адреса за окном не охраняются. Поток гостя — pthread со стеком 512 МБ (рекурсия
гостя = рекурсия хоста), главный поток — SDL, отдельный поток рендера. Switch: гость — ядро 1, рендер — 2, фоновые
потоки гостя — 0.

### Рекомпилятор

* x87 декодируй **по опкодам** (`x87.py`); мнемоникам capstone для x87 не доверяй.
* `discover.py`: сильные входы — экспорты, entry, цели `call`, релокации из данных, непосредственные
  операнды-указатели; слабые — пропуски после `int3/nop` с фильтром `plausible` (в `.text` WinDrv есть данные).
  `jmp` на сильный вход — хвостовой вызов, кроме прыжка назад внутрь функции. Jump-таблицы — граница из `cmp/ja`
  или серия релокаций, отрицательные индексы, двухуровневые `switch`. Продолжения `catch` — `_eh_continuations`,
  области `__except` — `_seh_scopes`.
* `emit.py`: регистры гостя — локальные копии, флаги ленивые (`flags.py`), x87 — статическая глубина (`f0..f7`,
  float-результат — `c->st_ret`). `gen.py`: вызовы (прямые, IAT, HLE `hle_<dll>_<имя>`, интринсики
  `_ftol/_ftol2/_CI*`), сводки «возвращает float», встраивание листовых ≤ 32 инструкций, вывод
  `build/gen/<mod>/*.c`, `<mod>_table.c` (таблица функций, IAT, CRC DLL), `hle_stubs.c` (слабые заглушки).
* Вывод детерминирован; каждая инструкция — комментарий `/* <адрес>: <инструкция> */`.

**Вызовы.** Прямой — `f_<адрес>(c)`; импорт — функция HLE; косвенный — `RT_ICALL` (кеш на месте → общий кеш
`rt_ic2` → таблица); адрес не функция — `rt_call` (продолжение EH/setjmp или ловушка). Переходники
инкрементальной компоновки (`jmp тело`) заменяются телом в вызовах и в таблице функций.

**Исключения.** setjmp гостя: вызов `_setjmp3` оборачивается хостовым setjmp в кадре функции, longjmp гостя
приходит в `rt_call` и переводится в хостовый longjmp. C++ EH и SEH: `RaiseException` (HLE) строит
EXCEPTION_RECORD/CONTEXT на стеке гостя и зовёт цепочку `fs:[0]`; `__CxxFrameHandler` msvcr70 исполняется как
есть. Функции с `catch`/`__except` получают обёртку с setjmp в `rt_eh_frame` и входом в тело по `switch` на
продолжение. **Не убирай узел диспетчера NT**, который `RaiseException` ставит на вершину цепочки на время
обработчика: без него второе исключение зацикливает обход (зависание на загрузке `3_The_Hunt_Part4`).

**Оверрайды** (`recomp/overrides.py`): замена по экспорту; `WRAPS` — обёртка с оригиналом `ovr_orig_<символ>`;
`SIGS` — неэкспортированная функция `"Модуль!=имя"` по байтам начала (должна находиться ровно одна, в обеих
локализациях). Сейчас заменены: CRT (`mem*/str*`, математика с семантикой MSVC в `crt_math.h`, `rand/srand`,
статические `memcpy` всех модулей), Hor+ в `FCameraSceneNode::UpdateMatrices`, пропуск `Flush(1)` после
`SaveGame`, кеш `TFieldIterator<UProperty>` (эпоха — `UStruct::Link`, `CollectGarbage`), быстрый
`FArchiveFileWriter::Serialize`, `Sleep(500)` в `TryRenderDevice` (по адресу возврата `0x11106a85`).

**Оптимизации.** Соблюдай их условия, иначе ошибки будут тихими:

| Приём | Условие |
|---|---|
| `CPU *restrict c` | память гостя и `CPU` не пересекаются |
| Сводки по регистрам (`regs.py`) | ниже |
| Адреса без заворота: `[рег ± d]` при \|d\| < 0x10000 → указатель `m + рег ± d`; esp — `u64` | корректно, пока адреса не переходят через 0 или 4 ГБ; большие смещения, индексы и `fs:` — с заворотом |
| Регистры значениями | **не передавай `&локальная` регистра гостя в хелперы** — регистр уйдёт в стек на всю функцию. Проверка: `grep -rhoE '&(eax\|ecx\|edx\|ebx\|ebp\|esi\|edi)\b' build/gen` — пусто |
| `SNX_PIN` (только GCC/Switch): eax…edi в x19–x26, `m` в x27 | рантайм входит в гостя только через `rt_pin_enter`; код только без закрепления — в `G_(...)`, только с ним — в `P_(...)`; дифф-тестами не покрыт |
| `FIST/FTRUNC` на месте | вне диапазона и NaN — «неопределённое целое» x87 (`tests/rt/fist_test`) |
| Порядок горячих функций (`build/hot_order.ld`) | первая строка — `KEEP (*(.crt0))`, иначе elf2nro затрёт функцию заголовком NRO |
| PGO GCC | сгенерированный C совпадает с профилированным; файлы со `switch` в разных секциях собирает `tools/cc_partition.sh`; `-fprofile-partial-training` не включай; libgcov devkitA64 пишет TLS в `tpidr_el0`, поэтому профайлер косвенных вызовов определён в `host/boot/main.c` |

**Сводки по регистрам.** `touched(F)` — регистры, которые F с вызываемыми читает или пишет; `out(F)` — те, что к
возврату могут измениться (ebx/esi/edi/ebp — по анализу сохранения push/pop эпилога и `__SEH_prolog/epilog`).
Место вызова U пишет в `CPU` грязные ∩ touched(U) и читает out(U); выход и хвостовой вызов пишут все грязные; HLE
и косвенные цели меняют eax/ecx/edx/esp. Инвариант: на входе в функцию `CPU.R` верен для любого R ∈ touched.
Неподвижная точка ищется сверху вниз, поэтому обрывать итерации безопасно. Функции с EH, `_setjmp*` и
недокодированные — touched = out = все. Опоры: ABI MSVC у незнакомых целей (`SNX_CHECK_ABI=1`) и `pop R` эпилога
снимает свой `push R`. `SNX_REGS=0` даёт прежний вывод для сравнения.

**Ловушки x86/MSVC** (каждая закреплена дифф-тестом — не ломай):
* тело функции бывает ниже входа (`_woutput`); `0xCC` внутри инструкции (`d8 cc`) и `lea esp,[esp]` — не
  границы функций; цель `jmp` у переходника — отдельная функция;
* `fisttp`/`_ftol` вне диапазона — «неопределённое целое», не насыщение; `fld tbyte` неподдерживаемых
  кодировок — -NaN; `fsin/fcos` при |x| ≥ 2^63 оставляют операнд; `fyl2x/fpatan` от недопустимых — NaN;
* прямой вызов при непустом стеке x87 (`/GL`) — значения остаются у вызывающего; `ffree st(n)…st(0)` — очистка
  стека; входы-интринсики — тело-ловушка, интринсик исполняется нативно;
* HLE с double в ST0 (`ov_time_total/ov_time_tell`, `gen.HLE_FLOAT_RET`) ставит `c->st_ret`;
* `xorps/pxor` регистра с собой — обнуление; CPUID — «Pentium Pro без MMX/SSE»; RDTSC — наносекунды;
* COM-методы снимают со стека и `this`; сдвиг стека на 4 байта ломает восстановление callee-saved.
* На ПК x87 после создания устройства D3D считает с 24-битной мантиссой, в порте — double. Учитывай при
  расхождениях в физике или анимации.

### Рантайм и HLE

* **Загрузчик** (`loader.c`, `process.c`): PE из `game/System`, CRC против сгенерированного кода, релокации, IAT,
  трамплины, DllMain в порядке зависимостей. `GetProcAddress`: внешние DLL — `g_dyn` в `kernel32.c`, свои —
  экспорты `<mod>_table.c` (UE2 ищет `int<Pkg><Class>exec*` по имени).
* **Файлы** (`vfs.c`, `kernel32_file.c`): гость видит `C:\Shrek2`; сначала пользовательский оверлей (`--user`),
  потом игра, имена без учёта регистра. Чтение — кеш блоков 64 КБ и фоновые образы пакетов
  (`SNX_FILE_CACHE_MB`). Запись копится в буфере и сбрасывается **перед любым другим обращением к файлу или
  операцией по имени**; большие файлы пишутся фоном. Меняя это, прогоняй `tests/rt/wfile_test`.
* **Окна** (`user32.c`, `gdi32.c`): классы, очереди по потокам, синтез WM_PAINT, активация, диалоги из ресурсов;
  GDI-шрифты через FreeType. Фатальная ошибка движка идёт в `user32_modal_hook` (`host/boot/main.c: on_modal`).
* **D3D8 → GL** (`d3d8*.c`). Шейдеры держи в рамках GL 4.1 core (мак), хотя Switch даёт 4.3. Соглашение
  `d3d8_int.h`: изображения «сверху вниз», FBO с перевёрнутой осью y, поэтому `D3DCULL_CW` → `GL_FRONT`, +0,5
  пикселя, глубина `2z − w`. Фиксированный конвейер генерируется в GLSL по ключу `ffkey` (кеш 4096, ключи — в
  `<user>/System/SnxPrograms.bin`, собираются фоном при создании устройства); ps.1.x — `d3d8_ps.c`. Не
  реализованы: state blocks, CopyRects, UpdateTexture, GetFrontBuffer, volume-текстуры, ProcessVertices, палитры,
  программируемые vs.1.1 — дают ловушку COM с именем слота или `hle_todo`.
* **Поток рендера** (`gl_cs.c`): все вызовы GL слоя D3D8 — макросы `cs_gl*` в кольцо 64 МБ. Новый вызов GL
  добавляй так же; ожидание (`cs_sync`) — только когда нужен результат. Present — `cs_swap`, отставание ≤ 1 кадра.
* **Звук и видео**: `bink.c` (контейнер), `bink_audio.c` (свой декодер; сверяй с ffmpeg `tests/bink/compare.py`),
  `bink_video.c` (libavcodec), `openal.c` (без EAX), `vorbisfile.c` (FILE* гостя читается его msvcr70).
* **Ввод**: в игре геймпад — джойстик DirectInput (`dinput8.c`, привязки `Joy*` пишет `config.c` в `User.ini`),
  в меню — мышь и фокус (`runtime/game/menu.c`). Скрипты UI разбирай `tools/recon/upkg.py` (`exports`/`at`/`dis`/
  `grep`). Настройки порта — `<user>/System/Snx.ini`, описаны в `docs/CONSOLE.md`.
* **Switch**: выход и ловушка завершают процесс через `svcExitProcess` (после `exit()` потоки жили в выгруженном
  коде); падения ловит `__libnx_exception_handler`; окно SDL после создания не меняется; вывод без nxlink —
  `<user>/snx.log`.

---

## Команды

```sh
tools/build.sh mac|mac-rus|switch|switch-rus|switch-pgo [цели]   # каталог build/<вариант>
tools/android_deps.sh [--ffmpeg] && tools/build.sh android|android-rus   # APK, docs/ANDROID.md
tools/.venv/bin/python -I recomp/main.py discover --game game/System [модули]
tools/.venv/bin/python -I recomp/main.py gen --game game/System --out build/gen [модули]
build/mac/host/boot/snx_boot [--game game] [--user DIR] [--dlls-only] [--headless] [--seconds N] [-- карта]
tools/make_rus.sh Shrek2RUS game/System/game.exe   # затем gen --game build/game-rus/System --out build/gen-rus
cmake -S . -B build/mac-rel -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DSNX_TRACE=OFF   # сборка для замеров
```

* `--seconds N` сбрасывает лог движка и выходит с кодом 0 — используй для коротких прогонов. Headless — копия
  пользовательского каталога с `RenderDevice=Engine.NullRenderDevice` и `--headless`.
* Генерация: `SNX_INLINE_MAX`, `SNX_REGS=0`, `SNX_REGCHECK=1`. CMake: `SNX_TRACE` (в `build/mac` включён,
  ≈−35 % скорости), `SNX_PIN` (Switch), `SNX_PGO=gen|use`, `SNX_PGO_DIR`, `SNX_SECTION_ORDER`, `SNX_VARIANT=rus`.
* Ограничение времени на маке: `perl -e "alarm 120; exec @ARGV" ...` (`timeout` нет).

### Дифф-тесты

```sh
tests/difftest/run_all.sh [funcs] [seeds]          # логи build/difftest_<stem>.log
UC_IGNORE_REG_BREAK=1 tools/.venv/bin/python -I tests/difftest/difftest.py \
    --harness build/mac/tests/difftest/difftest_core --game game/System --module Core.dll --funcs 1500 --seeds 4
tools/build.sh mac difftest_msvcr70 && UC_IGNORE_REG_BREAK=1 tools/.venv/bin/python -I tests/difftest/crtmath.py \
    --harness build/mac/tests/difftest/difftest_msvcr70 --game game/System --cases 2000 [--only pow]
```

Прогон долгий (Engine ≈150 с): запускай с выводом в файл. Опции: `--only`, `--debug <addr>`, `--check-image`,
`--full-every 1`, `--watch`, `--blocklog`; в сборке с `SNX_TRACE` — `DIFFTEST_VERBOSE=1`, `DIFFTEST_WATCH=<addr>`,
`RT_BLOCKLOG=rc.log` + `tests/difftest/bdiff.py rc.log uc.log`. Не считай ошибкой NaN другого знака и ecx/edx
после замен CRT. Эталон unicorn неточен на краях x87 (`fprem` с большим частным, `fsin/fcos/fptan` при
|x| ≥ 2^63, `f2xm1/fscale`, `fyl2x` от отрицательного).

### Переменные запуска

| Переменная | Назначение |
|---|---|
| `SNX_LOG_LOADER`, `SNX_LOG_FILES`, `SNX_LOG_READS`, `SNX_LOG_WINDOWS`, `SNX_LOG_AUDIO`, `SNX_LOG_EH`, `SNX_LOG_MENU` | логи загрузчика, файлов, чтений (`tools/probe/reads.py`), окон, звука, исключений, фокуса меню |
| `SNX_HLE_TRACE=1` | каждый вызов HLE (~100 МБ; фоновый поток Core `10174140` шумит критическими секциями) |
| `SNX_CHECK_ABI=1` | после косвенного вызова проверить esi/edi/ebx/ebp |
| `SNX_KEYS=<с>:<VK hex>,<с>:@<u>/<v>,<с>:%<кнопка>+,<с>:~LX=<v>` | ввод без рук: клавиши, щелчок, кнопки и стики геймпада |
| `SNX_SHOT=300,600`, `SNX_SHOT_DIR`, `SNX_DUMP_DRAWS=<кадр>` | PNG заднего буфера; состояние каждой отрисовки (`tools/probe/sheet.py`) |
| `SNX_EH_TEST=<с>`, `SNX_ERR_TEST=<с>` | исключение C++ в `StaticLoadObject`; фатальная ошибка движка |
| `SNX_FIELD_CHECK=1` | сверить кеш обхода свойств с полным обходом |
| `SNX_PERF=1`, `SNX_NOVSYNC=1`, `SNX_PROF=<файл>`, `SNX_CALLSTATS=<файл>` | инструкции и такты на кадр; без vsync; профиль; счётчики входов |
| `SNX_RENDER_THREAD=0`, `SNX_GLTHREAD=1`, `SNX_FILE_CACHE_MB` | без потока рендера; glthread Mesa; бюджет образов пакетов |
| `SNX_RENDER_SCALE`, `SNX_FPS_CAP`, `SNX_TEX16`, `SNX_GLES_S3TC=0`, `SNX_SHADER_CACHE`, `SNX_AFFINITY=0`, `SNX_ADPF=0`, `SNX_SHOW_FPS` | Android (`docs/ANDROID.md`): доля разрешения, ограничение FPS, DXT→16 бит, кэш программ, ядра, ADPF |
| `SNX_UO_DUMP=<с>`, `SNX_UO_CLASS`, `SNX_UO_ALL`, `SNX_FONT`, `SNX_GDI_FONT`, `SNX_QUIET_LOG`, `SNX_FULLSCREEN` | дамп объектов UE2; шрифты оверлея и GDI; без `[log]`; полный экран на маке |

### Отладка

* После ловушки или доступа к неотображённой памяти смотри вывод (цепочка ebp, скан стека с экспортами,
  `GErrorHist`) и **полный лог движка `<user>/System/game.log`**. `throw 1` — это `appErrorf`.
* Код по адресу: `tools/probe/gdis.sh <mod> <адрес> [n]` или `grep -n "/\* <адрес>: " build/gen/<mod>/*.c`;
  ещё `tools/probe/fdis.py`, `iat.py`, `vastr.py`. lldb: `process handle SIGALRM -s false`, точка останова по
  строке `<mod>_NNNN.c`, память гостя — `p/x *(unsigned*)(g_mem+esi+0x74)`.
* Имена в дифф-тестах — после `tools/recon/exports.py --game game/System --undname <llvm-undname> --out build/kb/exports`.
* Отчёты по бинарникам — `tools/recon/pesummary.py`, `pe_imports.py --all`, `isastats.py` → `build/recon/`.

### Профили и замеры

* Мак сбрасывает частоту — сравнивай по `SNX_PERF=1` (`[perf]`), не по кадрам в секунду.
* Профайлер `host/boot/prof.c` (`SNX_PROF`/`--prof`) ставит на паузу поток гостя (`g`) и рендера (`r`). Разбор:
  `tools/probe/profsplit.py --bin <elf> --objs <build>/runtime prof.txt`, `prof.py --thread g|r`, `hitchprof.py`
  (долгие кадры), `callcount.py`, `hotorder.py` (порядок функций для ld).
* Строки лога: `[hitch]` (долгий кадр: программы GL, чтения, заливки, ожидание рендера), `[cs]`,
  `[engine] SaveGame(N): M ms`, `[engine] Flush(N) from`, `[file] ... written in background`.

### Switch (запуск вручную)

Готовь команды; данные — `sdmc:/switch/shrek2/{game,user}` (русская — `shrek2rus/`), полный режим памяти:

```sh
tools/build.sh switch snx_boot_nro
tools/.venv/bin/python -I tools/switch_upload.py --host <IP> game /switch/shrek2/game
/opt/devkitpro/tools/bin/nxlink -s build/switch/host/boot/snx_boot.nro -- [--res WxH] [--env SNX_X=1] \
    [--prof sdmc:/switch/shrek2/prof.txt --prof-after 60 --prof-for 60] [--pgo-dump 300]
```

PGO: `tools/build.sh switch-pgo` → прогон с `--pgo-dump` → `python3 -I tools/pgo_fetch.py <IP>` (в `build/pgo`) →
`tools/build.sh switch` берёт профиль сам (`SNX_PGO=` — без него). Профиль снимай заново после каждой генерации.

---

## Состояние (0.1.0)

Игра играется на Switch: меню, первый уровень, катсцены, магазин, звук, ролики, геймпад, сохранения; обе
локализации. Все импорты реализованы; дифф-тесты без необъяснённых расхождений.

Точка отсчёта для замеров (Switch, болото у точки сохранения после катсцены, 1280×720, CPU 1020 МГц):
**38,0–38,8 кадр/с с PGO, 34,2–35,3 без PGO**. Меню — 60, ролики — 45–52. Кадр сохранения — 0,77–0,79 с,
фризы 50–200 мс от сборки программ GL — только в первом запуске. На Switch упор в поток гостя (в основном
сгенерированный код), поток рендера простаивает.

**Известные расхождения дифф-тестов — не ищи в них ошибку перевода:** msvcr70 — `_inp/_outp`, SSE2-ветки, обходы
FDIV, `fnstenv/fldenv` (не реализованы), `_j0/_j1/_jn` на краях, 117 функций математики с ошибками x87-анализа
(заменены нативными); D3DDrv — SSE/3DNow!/MMX-ветки, определение CPU (`1282b8e5`, `1282b9db`, `12831be5`),
`fsincos` при |x| ≥ 2^63, eax после `_CIacos` (`12829305`), мусорный указатель функции (`128298c1`); Core —
`appLoge` от отрицательного.

**Открытые задачи:**
* Фризы: сборка программ GL на отдельном контексте (риск: контексты Mesa nouveau делят pushbuf), остаток
  сохранения (`SavePackage`).
* Производительность: тени-проекторы (`UShadowBitmapMaterial::Get` — 8 % потока гостя), D3D8 в потоке рендера,
  нативное чтение `FArchive`, старт игры. Выбирай по свежему профилю.
* D3D8: недостающие методы (выше); проверить тени в катсцене полёта Осла после исправления полупикселя.
* 2D-интерфейс растянут в 16:9.
* **Android (не проверен ни разу):** C проверен только синтаксически (zig cc, musl, заголовки Khronos);
  CMake-ветка `cmake/android.cmake`, Gradle и Kotlin не собирались. Сначала — сборка и запуск на телефоне, затем
  прослойка ES (`gles_shim.c`: смещение LOD, чтение DXT и глубины, `glCompressedTexSubImage2D`), кольцевой
  буфер для `STREAM`-вершин, замер на слабом телефоне.
* Не проверены: загрузка сейвов, переходы между уровнями (`3_The_Hunt_Part4`), окно фатальной ошибки на Switch,
  HOME и сон (`docs/CONSOLE.md`), визуальная сверка с ПК.
