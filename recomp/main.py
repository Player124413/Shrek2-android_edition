"""CLI рекомпилятора: python3 -I recomp/main.py <команда> --game <game/System> [...]"""
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from discover import Module, Program  # noqa: E402
from gen import Ctx, compute_summaries, write_hle_stubs, write_module  # noqa: E402
from overrides import resolve as resolve_overrides, wrapped as resolve_wraps  # noqa: E402
from regs import compute as compute_regs  # noqa: E402
from image import load_addrmap, load_image  # noqa: E402


def load_program(game_dir, only=None):
    amap = load_addrmap()
    images = {}
    for name, base in amap.items():
        images[name.lower()] = load_image(os.path.join(game_dir, name), base)
    prog = Program(images)
    for name in images:
        if only and name not in only:
            continue
        prog.modules[name] = Module(images[name], prog)
    return prog


def cmd_discover(args):
    prog = load_program(args.game, [m.lower() for m in args.modules] if args.modules else None)
    for name, mod in prog.modules.items():
        t = time.time()
        mod.run()
        fs = mod.functions.values()
        code = sum(s.vsize for s in mod.img.code_sections())
        covered = len(mod.reachable)
        print(f"{name:12s} strong={len(mod.strong):6d} weak={len(mod.weak):5d} funcs={len(mod.functions):6d} "
              f"insns={covered:8d} jt={len(mod.jump_tables):4d} unk_ind={sum(f.unknown_indirect for f in fs):4d} "
              f"invalid={sum(f.invalid for f in fs):4d} dup={sum(len(f.insns) for f in fs) - covered:7d} "
              f"rounds={mod.rounds} code={code} {time.time() - t:.1f}s", flush=True)


def cmd_gen(args):
    prog = load_program(args.game)  # все модули нужны для межмодульных сводок
    t = time.time()
    for name, mod in prog.modules.items():
        mod.run()
    print(f"discovery: {time.time() - t:.0f}s", flush=True)
    ctx = Ctx(prog, resolve_overrides(prog), resolve_wraps(prog))
    t = time.time()
    compute_summaries(ctx)
    print(f"summaries: {time.time() - t:.0f}s", flush=True)
    t = time.time()
    compute_regs(prog, ctx)
    print(f"regs: {time.time() - t:.0f}s", flush=True)
    os.makedirs(args.out, exist_ok=True)
    only = {m.lower() for m in args.modules} if args.modules else None
    for name, mod in prog.modules.items():
        if only and name not in only:
            continue
        write_module(ctx, mod, args.out)
    write_hle_stubs(ctx, args.out, partial=only is not None)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["discover", "gen"])
    ap.add_argument("--game", required=True)
    ap.add_argument("--out", default="build/gen")
    ap.add_argument("modules", nargs="*")
    args = ap.parse_args()
    {"discover": cmd_discover, "gen": cmd_gen}[args.cmd](args)


if __name__ == "__main__":
    main()
