#!/usr/bin/env python3
"""nhalgen – macht aus dem Feature-Katalog die Bausteine eines NHAL-Builds.

Befehle:
  check     Katalog prüfen (Schema, Abhängigkeiten, Quellen, Header)
  generate  Für eine Plattform und ein Manifest erzeugen:
              include/nhal/nhal_features.h   Feature-IDs, NHAL_HAS_*-Makros
              src/nhal_registry.c            Registry und Build-Manifest
              nhal_sources.cmake             Quellen und Bibliotheken
  matrix    docs/platform-matrix.md erzeugen (mit --check nur vergleichen)

Grundsatz: Ein Pflicht-Feature, das auf der Plattform nicht geht, bricht den
Build mit Begründung ab. Nichts wird still ersetzt.
"""

from __future__ import annotations

import argparse
import re
import sys
import tomllib
from dataclasses import dataclass, field
from pathlib import Path

POLICIES = ("native", "emulated", "planned", "reject", "external")
STATUSES = ("stable", "experimental", "draft")
ID_RE = re.compile(r"^[a-z][a-z0-9_]*(\.[a-z][a-z0-9_]*)+$")
PLATFORM_RE = re.compile(r"^[a-z][a-z0-9_]*$")

POLICY_LABEL = {
    "native": "nativ",
    "emulated": "emuliert",
    "planned": "geplant",
    "reject": "abgelehnt",
    "external": "extern",
}


class CatalogError(Exception):
    """Fehler im Katalog oder in der Auswahl; die Meldungen stehen in .errors."""

    def __init__(self, errors: list[str]):
        super().__init__("\n".join(errors))
        self.errors = errors


def fnv1a32(text: str) -> int:
    """Stabile Feature-ID: FNV-1a (32 Bit) über den Namen."""
    h = 0x811C9DC5
    for b in text.encode("utf-8"):
        h ^= b
        h = (h * 0x01000193) & 0xFFFFFFFF
    return h


@dataclass
class Support:
    policy: str
    sources: list[str] = field(default_factory=list)
    link: list[str] = field(default_factory=list)
    js_libraries: list[str] = field(default_factory=list)  # nur Web: --js-library
    note: str = ""


@dataclass
class Platform:
    id: str
    name: str
    freestanding: bool = False
    external: bool = False   # Backends liegen außerhalb dieses Repos (z. B. Konsolen)
    allocator: list[str] = field(default_factory=list)   # Quellen des Standard-Allocators
    toolchain: str = ""      # CMake-Toolchain-Datei (relativ zur Wurzel)
    wrapper: str = ""        # z. B. "emcmake"
    note: str = ""


@dataclass
class Feature:
    id: str
    version: int
    status: str
    summary: str
    requires: list[str]
    platforms: dict[str, Support]
    file: Path

    @property
    def symbol(self) -> str:
        return self.id.replace(".", "_")

    @property
    def macro(self) -> str:
        return self.symbol.upper()

    @property
    def uid(self) -> int:
        return fnv1a32(self.id)

    @property
    def header(self) -> str:
        return "include/nhal/features/" + "/".join(self.id.split(".")) + ".h"


@dataclass
class Catalog:
    root: Path
    platforms: dict[str, Platform]
    features: dict[str, Feature]


# ── Laden und Prüfen ────────────────────────────────────────────────────────


def _str_list(value, where: str, errors: list[str]) -> list[str]:
    if value is None:
        return []
    if not isinstance(value, list) or not all(isinstance(v, str) for v in value):
        errors.append(f"{where}: erwartet eine Liste von Texten")
        return []
    return list(value)


def load_catalog(root: Path) -> Catalog:
    """Lädt catalog/ unter root und prüft alles, was ohne Plattform prüfbar ist."""
    root = root.resolve()
    cat_dir = root / "catalog"
    errors: list[str] = []

    platforms: dict[str, Platform] = {}
    pfile = cat_dir / "platforms.toml"
    if not pfile.is_file():
        raise CatalogError([f"{pfile}: fehlt"])
    with pfile.open("rb") as fh:
        for pid, data in tomllib.load(fh).items():
            if not PLATFORM_RE.match(pid) or not isinstance(data, dict):
                errors.append(f"platforms.toml: ungültige Plattform '{pid}'")
                continue
            freestanding = bool(data.get("freestanding", False))
            allocator = _str_list(data.get("allocator"), f"platforms.toml [{pid}] allocator", errors)
            external = bool(data.get("external", False))
            if not allocator and not freestanding and not external:
                allocator = ["core/src/default_allocator.c"]
            for src in allocator:
                if not (root / src).is_file():
                    errors.append(f"platforms.toml [{pid}]: Allocator-Quelle {src} fehlt")
            toolchain = str(data.get("toolchain", ""))
            if toolchain and not (root / toolchain).is_file():
                errors.append(f"platforms.toml [{pid}]: Toolchain {toolchain} fehlt")
            platforms[pid] = Platform(
                id=pid,
                name=str(data.get("name", pid)),
                freestanding=freestanding,
                external=external,
                allocator=allocator,
                toolchain=toolchain,
                wrapper=str(data.get("wrapper", "")),
                note=str(data.get("note", "")),
            )

    features: dict[str, Feature] = {}
    for path in sorted(cat_dir.rglob("*.toml")):
        if path == pfile:
            continue
        rel = path.relative_to(root)
        try:
            with path.open("rb") as fh:
                data = tomllib.load(fh)
        except tomllib.TOMLDecodeError as exc:
            errors.append(f"{rel}: kein gültiges TOML ({exc})")
            continue

        fid = data.get("id")
        if not isinstance(fid, str) or not ID_RE.match(fid):
            errors.append(f"{rel}: 'id' fehlt oder hat nicht die Form gruppe.name")
            continue
        expected = cat_dir.joinpath(*fid.split(".")).with_suffix(".toml")
        if path != expected:
            errors.append(f"{rel}: Feature '{fid}' gehört nach {expected.relative_to(root)}")
        if fid in features:
            errors.append(f"{rel}: Feature '{fid}' ist doppelt")
            continue

        version = data.get("version")
        if not isinstance(version, int) or isinstance(version, bool) or version < 1:
            errors.append(f"{rel}: 'version' muss eine ganze Zahl >= 1 sein")
            version = 1
        status = data.get("status", "")
        if status not in STATUSES:
            errors.append(f"{rel}: 'status' muss einer von {', '.join(STATUSES)} sein")
        summary = data.get("summary", "")
        if not isinstance(summary, str) or not summary:
            errors.append(f"{rel}: 'summary' fehlt")
            summary = ""

        supports: dict[str, Support] = {}
        for pid, sdata in (data.get("platforms") or {}).items():
            where = f"{rel} [platforms.{pid}]"
            if pid not in platforms:
                errors.append(f"{where}: unbekannte Plattform")
                continue
            if not isinstance(sdata, dict):
                errors.append(f"{where}: erwartet eine Tabelle")
                continue
            policy = sdata.get("policy")
            if policy not in POLICIES:
                errors.append(f"{where}: 'policy' muss einer von {', '.join(POLICIES)} sein")
                continue
            sup = Support(
                policy=policy,
                sources=_str_list(sdata.get("sources"), where + " sources", errors),
                link=_str_list(sdata.get("link"), where + " link", errors),
                js_libraries=_str_list(sdata.get("js_libraries"), where + " js_libraries", errors),
                note=str(sdata.get("note", "")),
            )
            implemented = policy in ("native", "emulated")
            if status != "draft" and implemented and not sup.sources:
                errors.append(f"{where}: '{policy}' braucht 'sources'")
            if not implemented and (sup.sources or sup.link or sup.js_libraries):
                errors.append(f"{where}: '{policy}' darf keine 'sources'/'link'/'js_libraries' haben")
            if sup.js_libraries and pid != "web":
                errors.append(f"{where}: 'js_libraries' gibt es nur für web")
            for src in sup.sources + sup.js_libraries:
                if not (root / src).is_file():
                    errors.append(f"{where}: Quelle {src} fehlt")
            supports[pid] = sup

        features[fid] = Feature(
            id=fid,
            version=version,
            status=status,
            summary=summary,
            requires=_str_list(data.get("requires"), f"{rel} requires", errors),
            platforms=supports,
            file=path,
        )

    # Querprüfungen
    seen_uids: dict[int, str] = {}
    for f in features.values():
        if f.uid == 0:
            errors.append(f"{f.id}: Hash ist 0 (reserviert), bitte umbenennen")
        if f.uid in seen_uids:
            errors.append(f"{f.id}: Hash-Kollision mit {seen_uids[f.uid]}")
        seen_uids[f.uid] = f.id
        for dep in f.requires:
            if dep not in features:
                errors.append(f"{f.id}: Abhängigkeit '{dep}' gibt es nicht")
            elif f.status != "draft" and features[dep].status == "draft":
                errors.append(f"{f.id}: hängt von Entwurf '{dep}' ab")
        if f.status != "draft" and not (root / f.header).is_file():
            errors.append(f"{f.id}: Header {f.header} fehlt")

    cycle = _find_cycle(features)
    if cycle:
        errors.append("Zyklische Abhängigkeit: " + " -> ".join(cycle))

    if errors:
        raise CatalogError(errors)
    return Catalog(root=root, platforms=platforms, features=features)


def _find_cycle(features: dict[str, Feature]) -> list[str] | None:
    state: dict[str, int] = {}
    stack: list[str] = []

    def visit(fid: str) -> list[str] | None:
        state[fid] = 1
        stack.append(fid)
        for dep in features[fid].requires:
            if dep not in features:
                continue
            if state.get(dep) == 1:
                return stack[stack.index(dep):] + [dep]
            if dep not in state:
                found = visit(dep)
                if found:
                    return found
        stack.pop()
        state[fid] = 2
        return None

    for fid in sorted(features):
        if fid not in state:
            found = visit(fid)
            if found:
                return found
    return None


# ── Auswahl für eine oder mehrere Plattformen ──────────────────────────────


@dataclass
class Manifest:
    required: list[str] = field(default_factory=list)
    optional: list[str] = field(default_factory=list)
    targets: list[str] = field(default_factory=list)   # Plattform-IDs oder ["all"]


@dataclass
class Analysis:
    """Ergebnis für eine Plattform, ohne Ausnahme – für Berichte über viele Plattformen."""
    platform: Platform
    need: dict[str, str]                    # id -> "required" | "optional" (alle angefragten)
    states: dict[str, tuple[str, str]]      # id -> (Zustand, Grund/Notiz)
    order: list[Feature]                    # baubare Features in Abhängigkeits-Reihenfolge
    errors: list[str]                       # fehlende Pflicht-Features
    notes: list[str]                        # weggelassene optionale Features


# Zustände in Analysis.states
STATE_NATIVE, STATE_EMULATED, STATE_EXTERNAL = "native", "emulated", "external"
STATE_DROPPED, STATE_MISSING = "dropped", "missing"


@dataclass
class Selection:
    platform: Platform
    order: list[Feature]          # in Abhängigkeits-Reihenfolge
    need: dict[str, str]          # id -> "required" | "optional"
    notes: list[str]              # weggelassene optionale Features


def split_list(values: list[str] | None) -> list[str]:
    """--features a,b --features c  →  [a, b, c]"""
    out: list[str] = []
    for v in values or []:
        out += [x.strip() for x in v.split(",") if x.strip()]
    return out


def load_manifest(path: Path) -> Manifest:
    with path.open("rb") as fh:
        data = tomllib.load(fh)
    feats = data.get("features") or {}
    plats = data.get("platforms") or {}
    errors: list[str] = []
    m = Manifest(
        required=_str_list(feats.get("required"), f"{path} [features] required", errors),
        optional=_str_list(feats.get("optional"), f"{path} [features] optional", errors),
        targets=_str_list(plats.get("targets"), f"{path} [platforms] targets", errors),
    )
    if errors:
        raise CatalogError(errors)
    return m


def expand_targets(cat: Catalog, targets: list[str]) -> list[str]:
    """["all"] → alle Plattformen; prüft unbekannte IDs."""
    if "all" in targets:
        return list(cat.platforms)
    unknown = [t for t in targets if t not in cat.platforms]
    if unknown:
        raise CatalogError([f"Unbekannte Plattform '{u}' (bekannt: {', '.join(cat.platforms)}, all)" for u in unknown])
    seen: list[str] = []
    for t in targets:
        if t not in seen:
            seen.append(t)
    return seen


def default_optional(cat: Catalog) -> list[str]:
    return [f.id for f in cat.features.values() if f.status != "draft"]


def _collect_need(cat: Catalog, required: list[str], optional: list[str]) -> dict[str, str]:
    errors: list[str] = []
    need: dict[str, str] = {}

    def add(fid: str, level: str, via: str) -> None:
        if fid not in cat.features:
            errors.append(f"Unbekanntes Feature '{fid}'{via}")
            return
        current = need.get(fid)
        if current == "required" or current == level:
            return
        need[fid] = level
        for dep in cat.features[fid].requires:
            add(dep, level, f" (gebraucht von {fid})")

    for fid in required:
        add(fid, "required", " (Pflicht)")
    for fid in optional:
        add(fid, "optional", " (optional)")
    if errors:
        raise CatalogError(errors)
    return need


def analyse(cat: Catalog, platform_id: str, required: list[str], optional: list[str]) -> Analysis:
    if platform_id not in cat.platforms:
        raise CatalogError([f"Unbekannte Plattform '{platform_id}' (bekannt: {', '.join(cat.platforms)})"])
    platform = cat.platforms[platform_id]
    need = _collect_need(cat, required, optional)

    def unavailable(f: Feature) -> str | None:
        if f.status == "draft":
            return "ist noch ein Entwurf (status = draft)"
        sup = f.platforms.get(platform_id)
        if sup is None:
            return f"hat keinen Eintrag für Plattform '{platform_id}'"
        if sup.policy == "reject":
            return "wird auf dieser Plattform abgelehnt" + (f": {sup.note}" if sup.note else "")
        if sup.policy == "planned":
            return "ist auf dieser Plattform geplant, das Backend fehlt noch" + (f": {sup.note}" if sup.note else "")
        if sup.policy == "external" and not platform.external:
            return "braucht ein externes Backend (noch nicht unterstützt)"
        return None

    selected = set(need)
    reasons: dict[str, str] = {}
    changed = True
    while changed:
        changed = False
        for fid in sorted(selected):
            f = cat.features[fid]
            why = unavailable(f)
            if why is None:
                missing = [d for d in f.requires if d not in selected]
                if missing:
                    why = "Abhängigkeit " + ", ".join(missing) + " fehlt"
            if why:
                selected.discard(fid)
                reasons[fid] = why
                changed = True

    states: dict[str, tuple[str, str]] = {}
    errors: list[str] = []
    notes: list[str] = []
    for fid in need:
        if fid in reasons:
            required_here = need[fid] == "required"
            states[fid] = (STATE_MISSING if required_here else STATE_DROPPED, reasons[fid])
            (errors if required_here else notes).append(f"{fid}: {reasons[fid]}")
        else:
            sup = cat.features[fid].platforms[platform_id]
            state = {"native": STATE_NATIVE, "emulated": STATE_EMULATED}.get(sup.policy, STATE_EXTERNAL)
            states[fid] = (state, sup.note)

    buildable = {fid for fid in selected if states[fid][0] in (STATE_NATIVE, STATE_EMULATED)}
    return Analysis(
        platform=platform,
        need=need,
        states=states,
        order=_topo(cat, buildable),
        errors=sorted(errors),
        notes=sorted(notes),
    )


def resolve(cat: Catalog, platform_id: str, required: list[str], optional: list[str]) -> Selection:
    """Auswahl für einen Build. Fehlt ein Pflicht-Feature, gibt es CatalogError."""
    a = analyse(cat, platform_id, required, optional)
    if a.platform.external:
        raise CatalogError([f"Plattform '{platform_id}' wird außerhalb dieses Repos gebaut (externe Backends)"])
    if a.errors:
        raise CatalogError([f"Pflicht-Feature nicht verfügbar auf '{platform_id}' – {e}" for e in a.errors])
    return Selection(
        platform=a.platform,
        order=a.order,
        need={k: v for k, v in a.need.items() if any(f.id == k for f in a.order)},
        notes=a.notes,
    )


def _topo(cat: Catalog, selected: set[str]) -> list[Feature]:
    done: list[Feature] = []
    seen: set[str] = set()

    def visit(fid: str) -> None:
        if fid in seen:
            return
        seen.add(fid)
        for dep in cat.features[fid].requires:
            visit(dep)
        done.append(cat.features[fid])

    for fid in sorted(selected):
        visit(fid)
    return done


# ── Ausgabe ────────────────────────────────────────────────────────────────

HEADER_NOTE = "Generiert von tools/nhalgen – nicht von Hand ändern."


def _c_str(text: str) -> str:
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'


def render_features_h(cat: Catalog, sel: Selection) -> str:
    out = [
        f"/* {HEADER_NOTE} */",
        "#ifndef NHAL_FEATURES_H",
        "#define NHAL_FEATURES_H",
        "",
        "#include <stdint.h>",
        "",
        "/* Feature-ID: FNV-1a (32 Bit) über den Namen, stabil über alle Builds. */",
        "typedef uint32_t nhal_feature_id;",
        "#define NHAL_FEATURE_NONE ((nhal_feature_id)0u)",
        "",
        "/* Alle Features des Katalogs. */",
    ]
    for f in sorted(cat.features.values(), key=lambda x: x.id):
        out.append(f"#define NHAL_FEATURE_{f.macro} ((nhal_feature_id)0x{f.uid:08x}u) /* {f.id} v{f.version} */")
    out += [
        "",
        "/* Dieser Build. */",
        f"#define NHAL_PLATFORM_NAME {_c_str(sel.platform.id)}",
        f"#define NHAL_PLATFORM_{sel.platform.id.upper()} 1",
    ]
    if sel.platform.freestanding:
        out.append("#define NHAL_PLATFORM_FREESTANDING 1")
    if sel.platform.allocator:
        out.append("#define NHAL_HAS_DEFAULT_ALLOCATOR 1")
    out.append("")
    out.append("/* Im Build enthaltene Features (zur Compile-Zeit abfragbar). */")
    for f in sel.order:
        out.append(f"#define NHAL_HAS_{f.macro} 1")
    out += ["", "#endif /* NHAL_FEATURES_H */", ""]
    return "\n".join(out)


def render_registry_c(cat: Catalog, sel: Selection) -> str:
    index = {f.id: i for i, f in enumerate(sel.order)}
    out = [
        f"/* {HEADER_NOTE} */",
        f"/* Plattform: {sel.platform.id} */",
        "",
        '#include "nhal/nhal_backend.h"',
        "",
    ]
    for f in sel.order:
        out.append(f"extern const nhal_feature_impl nhal_impl_{f.symbol};")
    out.append("")
    for f in sel.order:
        if f.requires:
            deps = ", ".join(f"{index[d]}u" for d in f.requires)
            out.append(f"static const uint16_t deps_{f.symbol}[] = {{ {deps} }};")
    out += ["", "const nhal_registry_entry nhal_registry[] = {"]
    for f in sel.order:
        sup = f.platforms[sel.platform.id]
        level = "NHAL_SUPPORT_NATIVE" if sup.policy == "native" else "NHAL_SUPPORT_EMULATED"
        deps = f"deps_{f.symbol}, {len(f.requires)}u" if f.requires else "NULL, 0u"
        out.append(
            f"    {{ NHAL_FEATURE_{f.macro}, {_c_str(f.id)}, {f.version}u, {level}, "
            f"&nhal_impl_{f.symbol}, {deps} }},"
        )
    if not sel.order:
        out.append("    { NHAL_FEATURE_NONE, \"\", 0u, NHAL_SUPPORT_NONE, NULL, NULL, 0u } /* Platzhalter */")
    out += ["};", f"const size_t nhal_registry_count = {len(sel.order)}u;", ""]

    out.append("const nhal_catalog_entry nhal_catalog[] = {")
    for f in sorted(cat.features.values(), key=lambda x: x.id):
        out.append(f"    {{ NHAL_FEATURE_{f.macro}, {_c_str(f.id)}, {f.version}u }},")
    out += ["};", f"const size_t nhal_catalog_count = {len(cat.features)}u;", ""]

    wanted = [f for f in sel.order if f.id in sel.need]
    out.append("static const nhal_request build_requests[] = {")
    for f in wanted:
        level = "NHAL_REQUIRED" if sel.need[f.id] == "required" else "NHAL_OPTIONAL"
        out.append(f"    {{ NHAL_FEATURE_{f.macro}, {f.version}u, {level} }},")
    if not wanted:
        out.append("    { NHAL_FEATURE_NONE, 0u, NHAL_OPTIONAL } /* Platzhalter */")
    out += [
        "};",
        f"const nhal_manifest nhal_build_manifest_data = {{ build_requests, {len(wanted)}u }};",
        "",
    ]
    return "\n".join(out)


def _cmake_list(items: list[str]) -> str:
    return '"' + ";".join(items) + '"'


def render_sources_cmake(cat: Catalog, sel: Selection) -> str:
    sources: list[str] = []
    links: list[str] = []
    js_libs: list[str] = []
    for f in sel.order:
        sup = f.platforms[sel.platform.id]
        for src in sup.sources:
            entry = "${NHAL_ROOT}/" + src
            if entry not in sources:
                sources.append(entry)
        for lib in sup.link:
            if lib not in links:
                links.append(lib)
        for js in sup.js_libraries:
            entry = "${NHAL_ROOT}/" + js
            if entry not in js_libs:
                js_libs.append(entry)
    inputs = ["${NHAL_ROOT}/" + str(p.relative_to(cat.root)) for p in sorted((cat.root / "catalog").rglob("*.toml"))]
    return "\n".join([
        f"# {HEADER_NOTE}",
        f'set(NHAL_PLATFORM_ID "{sel.platform.id}")',
        f"set(NHAL_PLATFORM_FREESTANDING {'ON' if sel.platform.freestanding else 'OFF'})",
        f"set(NHAL_FEATURE_SOURCES {_cmake_list(sources)})",
        f"set(NHAL_FEATURE_LINK_LIBS {_cmake_list(links)})",
        f"set(NHAL_FEATURE_JS_LIBRARIES {_cmake_list(js_libs)})",
        f"set(NHAL_ALLOCATOR_SOURCES {_cmake_list(['${NHAL_ROOT}/' + a for a in sel.platform.allocator])})",
        f"set(NHAL_SELECTED_FEATURES {_cmake_list([f.id for f in sel.order])})",
        f"set(NHAL_CATALOG_FILES {_cmake_list(inputs)})",
        "",
    ])


def render_matrix(cat: Catalog) -> str:
    pids = list(cat.platforms)
    out = [
        "# Plattform-Matrix",
        "",
        f"> {HEADER_NOTE} Quelle: `catalog/`.",
        "",
        "Legende: **nativ** = eigenes Backend · **emuliert** = Ersatz mit beschriebenen",
        "Unterschieden · **geplant** = vorgesehen, Backend fehlt noch · **abgelehnt** = gibt",
        "es dort nicht (Build/Start bricht mit Grund ab) · **extern** = Backend außerhalb",
        "dieses Repos · *(kursiv)* = Feature ist noch ein Entwurf · – = kein Eintrag.",
        "",
        "| Feature | Version | Status | " + " | ".join(cat.platforms[p].name for p in pids) + " |",
        "|---|---|---|" + "---|" * len(pids),
    ]
    notes: list[str] = []
    for f in sorted(cat.features.values(), key=lambda x: x.id):
        cells = []
        for p in pids:
            sup = f.platforms.get(p)
            if sup is None:
                cells.append("–")
                continue
            label = POLICY_LABEL[sup.policy]
            cells.append(f"*({label})*" if f.status == "draft" else label)
            if sup.note:
                notes.append(f"- `{f.id}` auf {cat.platforms[p].name}: {sup.note}")
        out.append(f"| `{f.id}` – {f.summary} | {f.version} | {f.status} | " + " | ".join(cells) + " |")
    if notes:
        out += ["", "## Anmerkungen", ""] + notes
    pnotes = [f"- **{p.name}**: {p.note}" for p in cat.platforms.values() if p.note]
    if pnotes:
        out += ["", "## Plattformen", ""] + pnotes
    out.append("")
    return "\n".join(out)


def _write_if_changed(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists() and path.read_text(encoding="utf-8") == text:
        return
    path.write_text(text, encoding="utf-8")


def generate(cat: Catalog, sel: Selection, out_dir: Path) -> None:
    _write_if_changed(out_dir / "include/nhal/nhal_features.h", render_features_h(cat, sel))
    _write_if_changed(out_dir / "src/nhal_registry.c", render_registry_c(cat, sel))
    _write_if_changed(out_dir / "nhal_sources.cmake", render_sources_cmake(cat, sel))


# ── Kommandozeile ──────────────────────────────────────────────────────────


def main(argv: list[str] | None = None) -> int:
    default_root = Path(__file__).resolve().parents[2]
    ap = argparse.ArgumentParser(prog="nhalgen", description=__doc__.splitlines()[0])
    ap.add_argument("--root", type=Path, default=default_root, help="Wurzel des NHAL-Repos")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("check", help="Katalog prüfen")
    g = sub.add_parser("generate", help="Build-Dateien erzeugen")
    g.add_argument("--platform", required=True)
    g.add_argument("--manifest", type=Path, help="App-Manifest (nhal.toml)")
    g.add_argument("--features", action="append", help="Pflicht-Features, kommagetrennt")
    g.add_argument("--optional", action="append", help="optionale Features, kommagetrennt")
    g.add_argument("--out", type=Path, required=True)
    m = sub.add_parser("matrix", help="docs/platform-matrix.md erzeugen")
    m.add_argument("--check", action="store_true", help="nur prüfen, ob die Datei aktuell ist")
    args = ap.parse_args(argv)

    try:
        cat = load_catalog(args.root)
        if args.cmd == "check":
            print(f"nhalgen: Katalog in Ordnung ({len(cat.features)} Features, {len(cat.platforms)} Plattformen)")
        elif args.cmd == "generate":
            m = load_manifest(args.manifest) if args.manifest else Manifest()
            required = m.required + split_list(args.features)
            optional = m.optional + split_list(args.optional)
            explicit = bool(required or optional)
            if not explicit:
                optional = default_optional(cat)   # nichts angegeben: alles Verfügbare
            sel = resolve(cat, args.platform, required, optional)
            if explicit:
                for note in sel.notes:
                    print(f"nhalgen: optional weggelassen – {note}")
            generate(cat, sel, args.out)
            names = ", ".join(f.id for f in sel.order) or "keine"
            print(f"nhalgen: {sel.platform.id}: {len(sel.order)} Features ({names})")
        elif args.cmd == "matrix":
            path = cat.root / "docs/platform-matrix.md"
            text = render_matrix(cat)
            if args.check:
                if not path.exists() or path.read_text(encoding="utf-8") != text:
                    print("nhalgen: docs/platform-matrix.md ist veraltet – 'nhalgen.py matrix' ausführen", file=sys.stderr)
                    return 1
                print("nhalgen: Plattform-Matrix ist aktuell")
            else:
                _write_if_changed(path, text)
                print(f"nhalgen: {path.relative_to(cat.root)} geschrieben")
    except CatalogError as exc:
        for e in exc.errors:
            print(f"nhalgen: Fehler: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
