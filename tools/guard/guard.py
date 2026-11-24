#!/usr/bin/env python3
"""guard – prüft Dateien, Pfade und Commit-Nachrichten auf gesperrte Begriffe.

Die Begriffe stehen nur als SHA-256-Hash in dieser Datei, damit der Guard sie
nicht selbst enthält. Zwei Arten:
  - Teilwort: trifft auch innerhalb längerer Wörter (Groß-/Kleinschreibung egal)
  - Ganzes Wort: trifft nur ein eigenständiges Wort (kurze Begriffe)

Aufruf:
  guard.py [--root DIR]        ganzes Repo prüfen (Dateinamen und Inhalte)
  guard.py --text-file DATEI   eine einzelne Datei prüfen (z. B. Commit-Nachricht)

Neuen Begriff sperren: Hash mit
  python3 -c "import hashlib,sys;print(hashlib.sha256(sys.argv[1].lower().encode()).hexdigest())" BEGRIFF
berechnen und unten eintragen (Teilwort zusätzlich mit Länge).
"""

from __future__ import annotations

import argparse
import hashlib
import re
import sys
from pathlib import Path

# (Länge, Hash) – trifft auch als Teil eines Wortes.
SUBSTRING_HASHES: set[tuple[int, str]] = {
    (15, "bc3d88e4425fd60a5498e7edc0b4ed062cbe7a5169f7e2bab5016932b06139b8"),
    (5, "f5cfcb570b7edac2ed16e1a025d50155d6148de7397f4068790cdfc142300070"),
    (6, "c857d09db23e6822e3600bc06ad8d58f92ed62bc8efd81c753f77048662cb97d"),
    (9, "c70eca6b0f88f44d81a41311647e50fda1ac454ec04ffd442b0eb4743a993131"),
    (6, "7d3194f79e645c42e4396dda38be04766810ec6a00d00aced3ffc2a0a1f1a9ef"),
    (7, "60965168ce762e949600281ba6d01fee136e5b6e8257b1f216f9025ed324474c"),
    (7, "3ea125d0bff386e6754b3782b300016fc79a9cf8f8669c0a5c3db64467ddb681"),
}

# Hash – trifft nur ein ganzes Wort.
WORD_HASHES: set[str] = {
    "2b84b2a9020f8b086b7709581ca4586a459938f80e0393abce3840dba921694a",
    "32e83e92d45d71f69dcf9d214688f0375542108631b45d344e5df2eb91c11566",
    "487b91042c7cf27a19e23ea8699f5f354b1a0c3af9e418138dc6150d830f970d",
    "053ea4804ef1bb33d4a3d6fb024a614b6d257cebc2bc7cd915da9c9522f37ffc",
}

SKIP_DIRS = {".git", "__pycache__", ".cache", ".vscode", ".idea"}
TOKEN_RE = re.compile(r"[A-Za-z0-9]+")
_LENGTHS = sorted({n for n, _ in SUBSTRING_HASHES})


def _sha(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def blocked(token: str) -> bool:
    low = token.lower()
    if _sha(low) in WORD_HASHES:
        return True
    for n in _LENGTHS:
        for i in range(len(low) - n + 1):
            if (n, _sha(low[i:i + n])) in SUBSTRING_HASHES:
                return True
    return False


def scan_text(text: str) -> list[tuple[int, str]]:
    hits = []
    for lineno, line in enumerate(text.splitlines(), start=1):
        for tok in TOKEN_RE.findall(line):
            if blocked(tok):
                hits.append((lineno, tok))
    return hits


def _is_skipped(rel: Path) -> bool:
    return any(part in SKIP_DIRS or part.startswith("build") for part in rel.parts[:-1])


def scan_repo(root: Path) -> list[str]:
    problems: list[str] = []
    for path in sorted(root.rglob("*")):
        rel = path.relative_to(root)
        if _is_skipped(rel) or (path.is_dir() and (rel.name in SKIP_DIRS or rel.name.startswith("build"))):
            continue
        for part in rel.parts:
            for tok in TOKEN_RE.findall(part):
                if blocked(tok):
                    problems.append(f"{rel}: gesperrter Begriff im Pfad ('{tok}')")
        if not path.is_file():
            continue
        data = path.read_bytes()
        if b"\0" in data:
            continue  # Binärdatei
        for lineno, tok in scan_text(data.decode("utf-8", errors="replace")):
            problems.append(f"{rel}:{lineno}: gesperrter Begriff ('{tok}')")
    return problems


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="guard", description=__doc__.splitlines()[0])
    ap.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
    ap.add_argument("--text-file", type=Path, help="nur diese Datei prüfen")
    args = ap.parse_args(argv)

    if args.text_file:
        text = args.text_file.read_text(encoding="utf-8", errors="replace")
        # Kommentarzeilen von git (#) nicht prüfen.
        text = "\n".join(l if not l.startswith("#") else "" for l in text.splitlines())
        problems = [f"{args.text_file}:{n}: gesperrter Begriff ('{t}')" for n, t in scan_text(text)]
    else:
        problems = scan_repo(args.root.resolve())

    for p in problems:
        print(f"guard: {p}", file=sys.stderr)
    if problems:
        print(f"guard: {len(problems)} Treffer – bitte entfernen", file=sys.stderr)
        return 1
    print("guard: keine gesperrten Begriffe gefunden")
    return 0


if __name__ == "__main__":
    sys.exit(main())
