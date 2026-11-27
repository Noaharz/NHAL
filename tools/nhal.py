#!/usr/bin/env python3
"""nhal – Plattformen und Features wählen, prüfen und bauen.

Plattformen (beliebig kombinierbar):
  --all                       alle Plattformen, die NHAL kennt
  --macos --windows --linux --xernel --web --console
  (ohne Angabe: [platforms] targets aus dem Manifest, sonst die Host-Plattform)

Features:
  --features a,b              Pflicht-Features
  --optional c,d              optionale Features
  --manifest nhal.toml        aus einem App-Manifest
  (ohne Angabe: alle verfügbaren Features als optional)

Befehle:
  list                        Plattformen und Features des Katalogs zeigen
  check                       Auswahl gegen alle gewählten Plattformen prüfen
  build                       für jede gewählte Plattform konfigurieren und bauen

Beispiele:
  ./nhal check --all --features time.monotonic
  ./nhal build --xernel --macos --features time.monotonic
  ./nhal build --web --manifest examples/minimal/nhal.toml
"""

from __future__ import annotations

import argparse
import os
import platform as host_platform
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools" / "nhalgen"))
import nhalgen  # noqa: E402

STATE_LABEL = {
    nhalgen.STATE_NATIVE: "nativ",
    nhalgen.STATE_EMULATED: "emuliert",
    nhalgen.STATE_EXTERNAL: "extern",
    nhalgen.STATE_DROPPED: "weggelassen",
    nhalgen.STATE_MISSING: "FEHLT",
}


def host_platform_id() -> str | None:
    system = host_platform.system()
    machine = host_platform.machine().lower()
    if system == "Darwin" and machine in ("arm64", "aarch64"):
        return "macos"
    if system == "Windows":
        return "windows"
    if system == "Linux":
        return "linux"
    return None


# ── Auswahl aus der Kommandozeile ──────────────────────────────────────────


def build_parser(cat: nhalgen.Catalog) -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(
        prog="nhal", description=__doc__.splitlines()[0],
        epilog="\n".join(__doc__.splitlines()[2:]), formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    sub = ap.add_subparsers(dest="cmd", required=True)

    def selection_args(p: argparse.ArgumentParser) -> None:
        g = p.add_argument_group("Plattformen")
        g.add_argument("--all", action="store_true", help="alle Plattformen")
        for pid, plat in cat.platforms.items():
            g.add_argument(f"--{pid}", dest="platforms", action="append_const", const=pid, help=plat.name)
        f = p.add_argument_group("Features")
        f.add_argument("--features", action="append", metavar="A,B", help="Pflicht-Features")
        f.add_argument("--optional", action="append", metavar="C,D", help="optionale Features")
        f.add_argument("--manifest", type=Path, help="App-Manifest (nhal.toml)")

    sub.add_parser("list", help="Plattformen und Features zeigen")
    selection_args(sub.add_parser("check", help="Auswahl prüfen"))
    b = sub.add_parser("build", help="konfigurieren und bauen")
    selection_args(b)
    b.add_argument("--build-dir", type=Path, default=ROOT / "build", help="Basisordner (je Plattform ein Unterordner)")
    b.add_argument("--toolchain", action="append", default=[], metavar="PLATTFORM=DATEI",
                   help="CMake-Toolchain-Datei für eine Plattform (Cross-Build)")
    b.add_argument("--test", action="store_true", help="nach dem Bauen ctest ausführen (wo möglich)")
    b.add_argument("--browser", action="store_true",
                   help="mit --test: Web-Tests zusätzlich in headless Chrome (WebGPU, Web Audio, Gamepad)")
    b.add_argument("--skip-unavailable", action="store_true",
                   help="nicht baubare Plattformen nur melden statt als Fehler zu werten")
    b.add_argument("--werror", action="store_true", help="Warnungen als Fehler")
    return ap


def selection(cat: nhalgen.Catalog, args) -> tuple[list[str], nhalgen.Manifest, bool]:
    m = nhalgen.load_manifest(args.manifest) if args.manifest else nhalgen.Manifest()
    required = m.required + nhalgen.split_list(args.features)
    optional = m.optional + nhalgen.split_list(args.optional)
    explicit = bool(required or optional)
    if not explicit:
        optional = nhalgen.default_optional(cat)

    if args.all:
        targets = list(cat.platforms)
    elif args.platforms:
        targets = args.platforms
    elif m.targets:
        targets = m.targets
    else:
        host = host_platform_id()
        if not host:
            raise nhalgen.CatalogError(["Host-Plattform nicht erkannt – Plattform angeben (z. B. --linux)"])
        targets = [host]
    return nhalgen.expand_targets(cat, targets), nhalgen.Manifest(required, optional, []), explicit


# ── Befehle ────────────────────────────────────────────────────────────────


def cmd_list(cat: nhalgen.Catalog) -> int:
    print("Plattformen:")
    for pid, p in cat.platforms.items():
        extra = " (extern)" if p.external else " (freestanding)" if p.freestanding else ""
        print(f"  --{pid:<10} {p.name}{extra}")
    print("\nFeatures:")
    for f in sorted(cat.features.values(), key=lambda x: x.id):
        print(f"  {f.id:<20} v{f.version}  {f.status:<12} {f.summary}")
    return 0


def print_report(cat: nhalgen.Catalog, analyses: list[nhalgen.Analysis]) -> None:
    fids = sorted({fid for a in analyses for fid in a.need})
    width = max([len(f) for f in fids] + [7])
    cols = [a.platform.id for a in analyses]
    cw = max([len(c) for c in cols] + [11])
    print(f"{'Feature':<{width}}  {'Bedarf':<8}  " + "  ".join(f"{c:<{cw}}" for c in cols))
    for fid in fids:
        need = next(a.need[fid] for a in analyses if fid in a.need)
        cells = [STATE_LABEL[a.states[fid][0]] for a in analyses]
        print(f"{fid:<{width}}  {('Pflicht' if need == 'required' else 'optional'):<8}  "
              + "  ".join(f"{c:<{cw}}" for c in cells))
    reasons = [(a.platform.id, fid, a.states[fid][1]) for a in analyses for fid in sorted(a.states)
               if a.states[fid][0] in (nhalgen.STATE_MISSING, nhalgen.STATE_DROPPED)]
    if reasons:
        print("\nGründe:")
        for pid, fid, why in reasons:
            print(f"  {pid}: {fid} {why}")


def cmd_check(cat: nhalgen.Catalog, args) -> int:
    targets, m, _ = selection(cat, args)
    analyses = [nhalgen.analyse(cat, pid, m.required, m.optional) for pid in targets]
    print_report(cat, analyses)
    failed = [a.platform.id for a in analyses if a.errors]
    print(flush=True)
    if failed:
        print(f"nhal: Pflicht-Features fehlen auf: {', '.join(failed)}", file=sys.stderr)
        return 1
    print(f"nhal: Auswahl passt auf allen {len(analyses)} Plattform(en): {', '.join(targets)}")
    return 0


def write_manifest(path: Path, m: nhalgen.Manifest) -> None:
    def arr(items: list[str]) -> str:
        return "[" + ", ".join(f'"{i}"' for i in items) + "]"
    path.parent.mkdir(parents=True, exist_ok=True)
    text = ("# Von tools/nhal.py erzeugt.\n\n[features]\n"
            f"required = {arr(m.required)}\noptional = {arr(m.optional)}\n")
    if not path.exists() or path.read_text(encoding="utf-8") != text:
        path.write_text(text, encoding="utf-8")


def configure_command(pid: str, cat: nhalgen.Catalog, toolchains: dict[str, Path],
                      source: Path, build: Path, defines: list[str]) -> tuple[list[str] | None, str]:
    """Liefert (Befehl, '') oder (None, Grund), warum hier nicht gebaut werden kann.

    Reihenfolge: --toolchain der Kommandozeile, dann toolchain/wrapper aus
    catalog/platforms.toml, dann der Host selbst.
    """
    plat = cat.platforms[pid]
    base = ["cmake", "-S", str(source), "-B", str(build), f"-DNHAL_PLATFORM={pid}"] + defines
    if plat.external:
        return None, "Backends liegen außerhalb dieses Repos"
    if pid in toolchains:
        return base + [f"-DCMAKE_TOOLCHAIN_FILE={toolchains[pid]}"], ""
    if plat.toolchain:
        return base + [f"-DCMAKE_TOOLCHAIN_FILE={ROOT / plat.toolchain}"], ""
    if plat.wrapper:
        if shutil.which(plat.wrapper):
            return [plat.wrapper] + base, ""
        return None, f"{plat.wrapper} fehlt im PATH (für {plat.name} nötig)"
    if pid == host_platform_id():
        return base, ""
    return None, f"Cross-Build braucht eine Toolchain: --toolchain {pid}=DATEI"


def cmd_build(cat: nhalgen.Catalog, args) -> int:
    targets, m, explicit = selection(cat, args)
    toolchains: dict[str, Path] = {}
    for item in args.toolchain:
        pid, _, file = item.partition("=")
        if pid not in cat.platforms or not file:
            raise nhalgen.CatalogError([f"--toolchain erwartet PLATTFORM=DATEI, bekam '{item}'"])
        toolchains[pid] = Path(file).resolve()

    results: list[tuple[str, str, str]] = []   # (Plattform, ok|skipped|failed, Text)
    for pid in targets:
        print(f"\n━━ {cat.platforms[pid].name} ({pid}) ━━", flush=True)
        a = nhalgen.analyse(cat, pid, m.required, m.optional)
        if a.errors:
            for e in a.errors:
                print(f"  Pflicht-Feature fehlt: {e}", file=sys.stderr)
            results.append((pid, "failed", "Pflicht-Features fehlen"))
            continue

        build = args.build_dir.resolve() / pid
        defines = [f"-DNHAL_WERROR={'ON' if args.werror else 'OFF'}"]
        manifest_path = build / "nhal.selection.toml"
        defines.append(f"-DNHAL_MANIFEST={manifest_path if explicit else ''}")
        cmd, why = configure_command(pid, cat, toolchains, ROOT, build, defines)
        if cmd is None:
            state = "skipped" if args.skip_unavailable else "failed"
            print(f"  nicht baubar: {why}", file=sys.stderr, flush=True)
            results.append((pid, state, why))
            continue
        if explicit:
            write_manifest(manifest_path, m)

        steps = [cmd, ["cmake", "--build", str(build), "--config", "Release"]]
        runs_here = pid == host_platform_id() or (pid == "web" and shutil.which("node") is not None)
        on_target = pid == "xernel" and bool(os.environ.get("XERNEL_ROOT")) and shutil.which("qemu-system-x86_64")
        if args.test and runs_here:
            steps.append(["ctest", "--test-dir", str(build), "-C", "Release", "--output-on-failure"])
        elif args.test and on_target:
            # Erst Python-Tests auf dem Host, dann die C-Tests in QEMU auf Xernel.
            steps.append(["ctest", "--test-dir", str(build), "-C", "Release", "--output-on-failure"])
            steps.append(["@xernel-qemu", str(build / "nhal_target_tests.txt")])
            runs_here = True
        if args.test and args.browser and pid == "web":
            steps.append(["@browser", str(build)])
        ok = True
        for step in steps:
            if step[0] == "@browser":
                names = (Path(step[1]) / "nhal_browser_tests.txt").read_text(encoding="utf-8").split()
                step = ["node", str(ROOT / "tools" / "web_browser_test.mjs"), str(Path(step[1]) / "tests"), *names]
            if step[0] == "@xernel-qemu":
                programs = Path(step[1]).read_text(encoding="utf-8").split()
                step = [sys.executable, str(ROOT / "tools" / "xernel_qemu.py"), *programs]
            print("  $ " + " ".join(step), flush=True)
            if subprocess.run(step, cwd=ROOT).returncode != 0:
                ok = False
                break
        text = str(build) if ok else "Build fehlgeschlagen"
        if ok and args.test and not runs_here:
            text += " (Tests laufen nur auf der Zielplattform)"
        results.append((pid, "ok" if ok else "failed", text))

    print("\n━━ Ergebnis ━━")
    mark = {"ok": "✓", "skipped": "–", "failed": "✗"}
    for pid, state, text in results:
        print(f"  {mark[state]} {pid:<10} {text}")
    return 1 if any(state == "failed" for _, state, _ in results) else 0


def main(argv: list[str] | None = None) -> int:
    try:
        cat = nhalgen.load_catalog(ROOT)
        args = build_parser(cat).parse_args(argv)
        if args.cmd == "list":
            return cmd_list(cat)
        if args.cmd == "check":
            return cmd_check(cat, args)
        return cmd_build(cat, args)
    except nhalgen.CatalogError as exc:
        for e in exc.errors:
            print(f"nhal: Fehler: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
