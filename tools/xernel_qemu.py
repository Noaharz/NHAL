#!/usr/bin/env python3
"""xernel_qemu – NHAL-Testprogramme auf Xernel in QEMU ausführen.

Jedes Programm wird mit `cargo xtask run --init <programm>` als init.elf
gebootet. Die serielle Konsole wird gelesen, bis der Test sein Ergebnis
meldet ("…: alles in Ordnung" oder "… fehlgeschlagen"), dann endet QEMU.

  xernel_qemu.py --xernel-root <workspace> <programm> [<programm> …]

Voraussetzungen: cargo (Xernel-Toolchain), qemu-system-x86_64, xorriso.
"""

from __future__ import annotations

import argparse
import os
import selectors
import signal
import subprocess
import sys
import time
from pathlib import Path

PASS_MARK = "alles in Ordnung"
FAIL_MARKS = ("fehlgeschlagen", "panic", "PANIC")


def run_one(xernel_root: Path, program: Path, timeout: float) -> tuple[bool, str]:
    proc = subprocess.Popen(
        ["cargo", "xtask", "run", "--init", str(program)],
        cwd=xernel_root, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        start_new_session=True,   # eigene Prozessgruppe: QEMU endet mit
    )
    sel = selectors.DefaultSelector()
    sel.register(proc.stdout, selectors.EVENT_READ)
    lines: list[str] = []
    booted = False
    verdict: bool | None = None
    deadline = time.monotonic() + timeout
    try:
        while time.monotonic() < deadline and verdict is None:
            if not sel.select(timeout=0.5):
                if proc.poll() is not None:
                    break
                continue
            line = proc.stdout.readline()
            if not line:
                break
            if "[xernel]" in line:
                booted = True
            if booted:
                lines.append(line.rstrip("\n"))
            if any(m in line for m in FAIL_MARKS):
                verdict = False
            elif PASS_MARK in line:
                verdict = True
        if verdict is False:  # restliche Meldungen kurz mitnehmen
            end = time.monotonic() + 1.0
            while time.monotonic() < end and sel.select(timeout=0.2):
                line = proc.stdout.readline()
                if not line:
                    break
                lines.append(line.rstrip("\n"))
    finally:
        try:
            os.killpg(proc.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        proc.wait(timeout=10)
    program_lines = [l for l in lines if not l.startswith("[xernel]")]
    if verdict is None:
        why = "keine Ergebnismeldung" + (" (Xernel hat nicht gebootet)" if not booted else "")
        return False, f"Zeitüberschreitung nach {timeout:.0f} s: {why}\n" + "\n".join(lines[-15:])
    return verdict, "\n".join(program_lines if verdict else lines[-30:])


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="xernel_qemu", description=__doc__.splitlines()[0])
    ap.add_argument("--xernel-root", type=Path, default=os.environ.get("XERNEL_ROOT"))
    ap.add_argument("--timeout", type=float, default=240.0, help="Sekunden je Programm")
    ap.add_argument("programs", nargs="+", type=Path)
    args = ap.parse_args(argv)
    if not args.xernel_root or not (args.xernel_root / "xtask").is_dir():
        print("xernel_qemu: --xernel-root bzw. XERNEL_ROOT zeigt nicht auf den Xernel-Workspace",
              file=sys.stderr)
        return 1
    failed = 0
    for prog in args.programs:
        print(f"xernel_qemu: {prog.name} …", flush=True)
        ok, text = run_one(args.xernel_root.resolve(), prog.resolve(), args.timeout)
        for line in text.splitlines():
            print(f"    {line}")
        print(f"xernel_qemu: {prog.name}: {'bestanden' if ok else 'FEHLGESCHLAGEN'}", flush=True)
        failed += 0 if ok else 1
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
