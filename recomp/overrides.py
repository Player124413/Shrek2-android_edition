"""Нативные замены гостевых функций: "модуль!экспорт" -> (C-символ, глубина x87 на выходе).

Реализации — runtime/overrides_*.c. Вызовы и dispatch по адресу экспорта идут в замену.
Обёртки (WRAPS) — такие же замены, но оригинал остаётся доступен замене как `ovr_orig_<символ>`.
Неэкспортированная функция — "модуль!=имя" с байтами её начала в SIGS (? — любой байт): адрес — единственное
совпадение в коде модуля (английская и русская версии DLL — разные сборки).
"""
import re

OVERRIDES = {}
WRAPS = {}
SIGS = {}

# Core: TFieldIterator<UProperty>::IterateToNext — пропуск не-свойств (функций, состояний, перечислений) в цепочке
# полей класса, кеш (runtime/overrides_core.c)
SIGS["field_next"] = "56 8B F1 83 3E 00 74 ? 57 BF 00 80 00 00 8B FF 8B 46 04 85 C0 74 ? EB"
OVERRIDES["Core.dll!=field_next"] = ("ovr_core_field_next", 0)
WRAPS["Core.dll!?CollectGarbage@UObject@@SAXK@Z"] = ("ovr_core_collect_garbage", 0)
WRAPS["Core.dll!?Link@UStruct@@UAEXAAVFArchive@@H@Z"] = ("ovr_core_struct_link", 0)
# Core: SaveConfig без перечитывания конфига в умолчания классов, чьи секции ini не менялись (подбор монет на
# бонусных уровнях; runtime/overrides_core.c)
WRAPS["Core.dll!?SaveConfig@UObject@@QAEXKPBG@Z"] = ("ovr_core_save_config", 0)
WRAPS["Core.dll!?LoadConfig@UObject@@QAEXHPAVUClass@@PBG@Z"] = ("ovr_core_load_config", 0)
# Core: TObjectIterator<T>::operator++ (общая копия шаблона): обход классов в SaveConfig — по списку подклассов
SIGS["objit_next"] = ("8B 41 04 40 89 41 04 3B 05 ? ? ? ? 7D ? 8B 15 ? ? ? ? 8B 04 82 85 C0 74 ? 8B 40 24 85 C0 8B 11 "
                      "74 ? 3B C2 74 ? 8B 40 2C 85 C0 75 ? 33 C0 85 D2 0F 94 C0 85 C0 74 ? C3")
WRAPS["Core.dll!=objit_next"] = ("ovr_core_objit_next", 0)
# Core: быстрые листовые функции (сравнение памяти, геометрия FBox/FVector; runtime/overrides_core.c)
OVERRIDES["Core.dll!?appMemcmp@@YAHPBX0H@Z"] = ("ovr_core_memcmp", 0)
WRAPS["Core.dll!??YFBox@@QAEAAV0@ABVFVector@@@Z"] = ("ovr_core_box_add_vec", 0)
WRAPS["Core.dll!?SafeNormal@FVector@@QBE?AV1@XZ"] = ("ovr_core_vec_safe_normal", 0)
WRAPS["Core.dll!?Normalize@FVector@@QAEHXZ"] = ("ovr_core_vec_normalize", 0)
# memcpy из статического CRT — своя копия у каждого модуля, хвосты — переходами по вычисленному адресу
SIGS["memcpy"] = "55 8B EC 53 56 57 A1 ? ? ? ? 83 38 00 0F 84 ? ? ? ? 8B 4D 10 8B 7D 08 8B 75 0C 8B D9 FC"
for _m in ["Engine.dll", "D3DDrv.dll", "ALAudio.dll", "Fire.dll", "GUI.dll", "IpDrv.dll", "KWGame.dll", "Window.dll",
           "game.exe"]:
    OVERRIDES[f"{_m}!=memcpy"] = ("ovr_memcpy", 0)
# game.exe: FArchiveFileWriter::Serialize — копия в буфер архива без вызовов гостя
SIGS["file_write"] = "53 8B 5C 24 0C 55 8B 6C 24 0C 56 8B F1 8B 56 48 8B 4E 4C 57 BF 00 10 00 00"
WRAPS["game.exe!=file_write"] = ("ovr_launch_file_write", 0)
# game.exe: FArchiveFileReader::Serialize — копия из буфера архива без вызовов гостя (runtime/overrides_launch.c)
SIGS["file_read"] = ("56 57 8B 7C 24 10 85 FF 8B F1 0F 8E FF 00 00 00 53 55 8B 6C 24 14 "
                     "8B 5E 54 8B 56 4C 8B 4E 50 2B DA 03 D9")
WRAPS["game.exe!=file_read"] = ("ovr_launch_file_read", 0)
# game.exe: FConfigCacheIni::GetString — по памяти кеша ini без вызовов гостя (runtime/overrides_core.c)
SIGS["cfg_get_string"] = ("55 8B EC 6A FF 68 ? ? ? ? 64 A1 00 00 00 00 50 64 89 25 00 00 00 00 83 EC 18 8B 45 18 53 56 "
                          "57 8B 7D 10 33 DB 89 65 F0 53 50 89 5D FC 66 89 1F E8")
WRAPS["game.exe!=cfg_get_string"] = ("ovr_launch_cfg_get_string", 0)
# game.exe: FFileManagerWindows::FileSize — размер без открытия, если он известен (runtime/overrides_launch.c)
SIGS["file_size"] = ("55 8B EC 6A FF 68 ? ? ? ? 64 A1 00 00 00 00 50 64 89 25 00 00 00 00 83 EC 0C A1 ? ? ? ? 8B 08 53 "
                     "56 57 33 FF 3B CF 89 65 F0 89 7D FC 74 ? 8B 4D 08 57 68 80 00 00 00 6A 03 57 6A 01 68 00 00 00 80 51 FF 15")
WRAPS["game.exe!=file_size"] = ("ovr_launch_file_size", 0)

# Engine: Hor+ для широкого экрана (runtime/overrides_engine.c)
WRAPS["Engine.dll!?UpdateMatrices@FCameraSceneNode@@UAEXXZ"] = ("ovr_engine_camera_matrices", 0)
# Engine: без сброса ресурсов рендера сразу после сохранения (runtime/overrides_engine.c)
WRAPS["Engine.dll!?SaveGame@UGameEngine@@UAEXH@Z"] = ("ovr_engine_save_game", 0)
WRAPS["Engine.dll!?Flush@UEngine@@UAEXH@Z"] = ("ovr_engine_flush", 0)
# Engine: NEON/нативное отсечение по пирамиде видимости (в оригинале SSE отключён по CPUID; runtime/overrides_engine.c)
WRAPS["Engine.dll!?BoxCheck@FConvexVolume@@QAEEABVFVector@@0@Z"] = ("ovr_engine_box_check", 0)
WRAPS["Engine.dll!?SphereCheck@FConvexVolume@@QAEEABVFSphere@@@Z"] = ("ovr_engine_sphere_check", 0)
# Engine: CPU-скиннинг скелетных мешей (MeshSkinVerts, Engine.dll!0x121ed630, не экспортирована; runtime/overrides_engine.c)
SIGS["skin_verts"] = ("6A FF 68 ? ? ? ? 64 A1 00 00 00 00 50 64 89 25 00 00 00 00 81 EC FC 00 00 00 "
                      "53 55 56 57 0F 31 F7 D8 8D 8C 24 84 00 00 00")
WRAPS["Engine.dll!=skin_verts"] = ("ovr_engine_skin_verts", 0)
# Engine: генерация вершинного буфера спрайтовых частиц (Engine.dll!0x122039d0; runtime/overrides_engine.c)
WRAPS["Engine.dll!?FillVertexBuffer@USpriteEmitter@@UAEHPAVFSpriteParticleVertex@@PAVFLevelSceneNode@@@Z"] = (
    "ovr_engine_sprite_fill_vb", 0)
# Engine: 1-проходное GPU-размытие теней-проекторов вместо 9x FCanvasUtil::DrawTile (runtime/overrides_engine.c)
WRAPS["Engine.dll!?Get@UShadowBitmapMaterial@@UAEPAVUBitmapMaterial@@NPAVUViewport@@@Z"] = (
    "ovr_engine_shadow_get", 0)
WRAPS["Engine.dll!?DrawTile@FCanvasUtil@@QAEXMMMMMMMMMPAVUMaterial@@VFColor@@@Z"] = (
    "ovr_engine_shadow_draw_tile", 0)

# D3DDrv: горячий слой состояния D3D8 напрямую поверх d3d8_direct.h без COM-трамплинов (runtime/overrides_engine.c)
SIGS["d3d_commit_state"] = ("55 8B EC 6A FF 68 ? ? ? ? 64 A1 00 00 00 00 50 64 89 25 00 00 00 00 "
                            "83 EC 28 8B C1 89 4D EC 8B 08 8B 91 94 09 03 00")
WRAPS["D3DDrv.dll!=d3d_commit_state"] = ("ovr_d3ddrv_commit_state", 0)
SIGS["d3d_set_mat_pass"] = ("55 8B EC 6A FF 68 ? ? ? ? 64 A1 00 00 00 00 50 64 89 25 00 00 00 00 "
                            "83 EC 10 89 4D EC 8B C1 8B 88 0C AA 02 00 8B 91 50 05 00 00")
WRAPS["D3DDrv.dll!=d3d_set_mat_pass"] = ("ovr_d3ddrv_set_mat_pass", 0)
SIGS["d3d_ehvec_ctor"] = ("8B 44 24 0C 48 78 26 53 8B 5C 24 14 55 8B 6C 24 10 56 8B 74 24 10 57 "
                          "8D 78 01 8D 9B 00 00 00 00 8B CE FF D3 03 F5 4F 75 F7 5F 5E 5D 5B C2 10 00")
WRAPS["D3DDrv.dll!=d3d_ehvec_ctor"] = ("ovr_d3ddrv_ehvec_ctor", 0)

# msvcr70: математика (внутри — x87-трюки с нестандартной передачей стека, статически не переводится)
for _n in ["acos", "asin", "atan", "atan2", "ceil", "cos", "cosh", "exp", "fabs", "floor", "fmod", "frexp",
           "ldexp", "log", "log10", "modf", "pow", "sin", "sinh", "sqrt", "tan", "tanh", "_logb", "_nextafter",
           "_y0", "_y1", "_yn", "_wtof", "wcstod", "atof", "strtod", "_atodbl"]:
    OVERRIDES[f"msvcr70.dll!{_n}"] = (f"ovr_crt_{_n.lstrip('_')}", 0 if _n == "_atodbl" else 1)
# msvcr70: rand/srand без _getptd (три вызова HLE на каждое число; runtime/overrides_crt.c)
OVERRIDES["msvcr70.dll!rand"] = ("ovr_crt_rand", 0)
OVERRIDES["msvcr70.dll!srand"] = ("ovr_crt_srand", 0)
# msvcr70: нативный qsort с быстрым путём для 16-байтной сортировки примитивов рендера (runtime/overrides_crt.c)
WRAPS["msvcr70.dll!qsort"] = ("ovr_crt_qsort", 0)


def _match_sig(img, sig):
    rx = re.compile(b"".join(b"." if p == "?" else re.escape(bytes([int(p, 16)])) for p in sig.split()), re.S)
    hits = []
    for s in img.code_sections():
        o = s.va - img.base
        hits += [s.va + m.start() for m in rx.finditer(bytes(img.data[o:o + s.vsize]))]
    return hits


def _find_sig(img, sig):
    hits = _match_sig(img, sig)
    if len(hits) != 1:
        raise SystemExit(f"overrides: signature {sig!r} in {img.name}: {len(hits)} matches")
    return hits[0]


def verify(prog):
    """Проверить все сигнатуры и экспорты из OVERRIDES и WRAPS без аварийного выхода.

    -> список (key, sym, status, detail), где status: 'ok' | 'missing_module' | 'missing_sig' | 'dup_sig' |
       'missing_export' | 'unknown_sig'.
    """
    results = []
    entries = list(OVERRIDES.items()) + list(WRAPS.items())
    for key, (sym, _) in entries:
        mod, name = key.split("!", 1)
        img = prog.images.get(mod.lower())
        if img is None:
            results.append((key, sym, "missing_module", mod))
            continue
        if name.startswith("="):
            sig_name = name[1:]
            sig = SIGS.get(sig_name)
            if sig is None:
                results.append((key, sym, "unknown_sig", sig_name))
                continue
            hits = _match_sig(img, sig)
            if len(hits) == 1:
                results.append((key, sym, "ok", hits[0]))
            elif len(hits) == 0:
                results.append((key, sym, "missing_sig", 0))
            else:
                results.append((key, sym, "dup_sig", hits))
        else:
            va = img.exports.get(name)
            if va is not None:
                results.append((key, sym, "ok", va))
            else:
                results.append((key, sym, "missing_export", name))
    return results


def _addr(prog, key):
    mod, name = key.split("!")
    img = prog.images.get(mod.lower())
    if img is None:
        return None
    if name.startswith("="):
        return _find_sig(img, SIGS[name[1:]])
    return img.exports.get(name)


def resolve(prog):
    """-> {guest addr: (sym, ret_depth)} для загруженных модулей (замены и обёртки)."""
    out = {}
    for key, val in list(OVERRIDES.items()) + list(WRAPS.items()):
        a = _addr(prog, key)
        if a is not None:
            out[a] = val
    return out


def wrapped(prog):
    """-> {guest addr: sym} обёрток: таблица модуля определяет для них ovr_orig_<sym> = f_<addr>."""
    out = {}
    for key, (sym, _) in WRAPS.items():
        a = _addr(prog, key)
        if a is not None:
            out[a] = sym
    return out
