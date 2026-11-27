# Architektur

## Überblick

```
 App (Engine, Server, Tool …)
   │  nhal.toml: required / optional
   ▼
 ┌──────────────────────────── Build ─────────────────────────────┐
 │ tools/nhalgen                                                  │
 │   catalog/*.toml + Manifest + Plattform                        │
 │     → nhal_features.h   (IDs, NHAL_HAS_*)                      │
 │     → nhal_registry.c   (gewählte Features, Reihenfolge)       │
 │     → nhal_sources.cmake (nur die nötigen Backend-Quellen)     │
 └────────────────────────────────────────────────────────────────┘
   │
   ▼
 ┌──────────────────────────── Start ─────────────────────────────┐
 │ nhal_init(): Manifest prüfen, Abhängigkeiten auflösen,         │
 │              Features in Reihenfolge starten (init prüft Gerät)│
 └────────────────────────────────────────────────────────────────┘
   │
   ▼
 ┌──────────────────────────── Laufzeit ──────────────────────────┐
 │ App hält Funktionstabellen (nhal_get einmal) → direkte Aufrufe │
 └────────────────────────────────────────────────────────────────┘
```

## Drei Zeitpunkte der Entscheidung

| Zeitpunkt | Was entschieden wird | Fehlerfall |
|---|---|---|
| **Build** | Welche Features überhaupt existieren (Manifest × Plattform-Regel) | Pflicht-Feature mit `reject`/`external`/Entwurf → Build bricht mit Grund ab |
| **Start** | Ob das Gerät das Feature wirklich kann (`init()` des Backends) | Pflicht-Feature scheitert → `nhal_init` liefert Status mit Feature und Grund |
| **Laufzeit** | nichts | – |

Damit kostet die Flexibilität im Hot Path nichts: Kein Aufruf prüft, ob ein
Modul aktiv ist. Wer eine Tabelle hat, ruft direkt.

## Kern

`include/nhal/nhal_core.h`, `core/src/core.c`

- Reines C11, nur freestanding-Header.
- Kein globaler Zustand außer „ist ein Kontext aktiv“.
- Speicher nur über `nhal_allocator`. Wo es eine libc gibt, gibt es einen
  Standard-Allocator (`core/src/default_allocator.c`).
- Statuscodes: `nhal_status { code, feature, reason }`. `reason` ist immer ein
  statischer Klartext, damit auch ohne libc formatiert werden kann.

## Features

Ein Feature besteht aus:

1. **Katalog-Eintrag** `catalog/<gruppe>/<name>.toml`: ID, Version, Status,
   Abhängigkeiten, Regel pro Plattform (siehe [catalog.md](catalog.md)).
2. **Header** `include/nhal/features/<gruppe>/<name>.h`: die Funktionstabelle
   `nhal_<gruppe>_<name>_v<N>` mit beschriebener Semantik und eine typisierte
   Zugriffsfunktion `nhal_get_<gruppe>_<name>_v<N>()`.
3. **Implementierungen** in `backends/<backend>/…`: je eine Definition von
   `const nhal_feature_impl nhal_impl_<gruppe>_<name>`.

### Versionen

- Die Version eines Features ist die Version seiner Tabelle.
- Neue Felder am Ende einer Tabelle sind kompatibel. `struct_size` zeigt, wie
  groß die Tabelle des Backends ist.
- Inkompatible Änderung = neue Version (`_v2`). Feature-IDs werden nie umbenannt.

### Feature-IDs

`FNV-1a(32 Bit)` über den Namen. Damit sind die IDs ohne zentrale Vergabe
stabil über alle Builds. Kollisionen erkennt `nhalgen check`.

## Plattform-Regeln

| Regel | Bedeutung |
|---|---|
| `native` | Eigenes Backend, volle Semantik |
| `emulated` | Ersatz-Implementierung. Die Unterschiede stehen im Header und in der Notiz des Katalogs, und `nhal_query` meldet `NHAL_SUPPORT_EMULATED` |
| `planned` | Vorgesehen, das Backend fehlt noch; verhält sich wie `reject`, sagt aber, dass es kommt |
| `reject` | Gibt es auf dieser Plattform nicht, mit Begründung |
| `external` | Backend liegt außerhalb dieses Repos (z. B. Konsolen) |

## Semantik statt Gleichmacherei

Verhält sich etwas auf verschiedenen Plattformen wirklich unterschiedlich, wird
das zur Eigenschaft der Schnittstelle, statt dass NHAL so tut, als wären alle
Geräte gleich. Beispiele:

- `time.monotonic`: `suspend` sagt, ob die Uhr im Ruhezustand weiterläuft.
- `gfx.device`: `info()` meldet `unified_memory` und `persistent_mapping`.
  Auf Apple Silicon (Metal) teilen sich CPU und GPU den Speicher, und Puffer
  mit `HOST_VISIBLE` sind dauerhaft über `buffer_mapped()` erreichbar. WebGPU
  kann das nicht; dort lehnt `buffer_create` `HOST_VISIBLE` mit Grund ab, und
  Daten gehen über `buffer_write`/`buffer_read`.
- `gfx.device`: Regeln, die nur eine API verlangt (Ausrichtung auf 256 Bytes
  bei Uniform-Offsets und Zeilen, Usage-Flags für Kopien), gelten überall
  (`backends/common/gfx/validate.h`). Was auf macOS läuft, scheitert so nicht
  erst im Browser.
- `gfx.device`: `info()` meldet außerdem `blocking_wait` (Metal wartet auf
  eine Seriennummer, der Browser nicht), `timestamp_queries` mit
  `timestamp_period` (Ticks in Nanosekunden) und `indirect_first_instance`.
  Push-Konstanten sind auf Metal echte `setBytes`-Daten; WebGPU hat keine,
  dort legt NHAL sie in einen Uniform-Ring mit dynamischem Offset. Die App
  sieht dieselbe Schnittstelle, die Shader-Bindung (Gruppe 3 bzw. Puffer 30)
  steht im Header.

## Backends

| Ordner | Plattformen |
|---|---|
| `backends/common/` | plattformunabhängig: `mem.*`, Scheduler, Warteschlangen von `fs.async_read`, Heap, Thread-Schicht, gemeinsame Prüfungen für `gfx.*` |
| `backends/posix/` | macOS, Linux, Web (gemeinsamer POSIX-Code) |
| `backends/windows/` | Windows |
| `backends/macos/`, `backends/linux/` | plattformeigener Code (macOS: CoreAudio, GameController, Metal in Objective-C mit ARC) |
| `backends/xernel/` | Xernel (eigenes Betriebssystem, mit xlibc) |
| `backends/web/` | Web (Emscripten) |
| `backends/console_template/` | nur Doku: Konsolen-Backends sind extern |

## Plattform- und Feature-Auswahl

Ein Build gilt immer für genau eine Plattform, denn ein Compiler erzeugt Code
für ein Ziel. Das Werkzeug `./nhal` nimmt beliebig viele Plattformen und macht
daraus je einen Build:

```
./nhal build --xernel --macos --features time.monotonic
        │
        ├─ check: Auswahl gegen jede Plattform auflösen (Tabelle + Gründe)
        ├─ build/xernel/  ← cmake -DNHAL_PLATFORM=xernel …
        └─ build/macos/   ← cmake -DNHAL_PLATFORM=macos  …
```

- `--all` steht für alle Plattformen aus `catalog/platforms.toml`.
- Die Host-Plattform, Plattformen mit eigener Toolchain-Datei (Xernel) und
  Plattformen mit Wrapper (Web: `emcmake`) baut `./nhal` direkt. Für andere
  Ziele braucht es `--toolchain <plattform>=<datei>`. Ohne Toolchain gibt es
  einen Fehler mit Grund, keinen stillen Verzicht.
- Externe Plattformen (Konsolen) werden geprüft, aber nicht gebaut.

## Xernel

Xernel ist ein eigenständiges Betriebssystem, so wie Linux, und für NHAL eine
Plattform wie jede andere. NHAL baut gegen die C-Bibliothek von Xernel (xlibc)
mit einer eigenen Toolchain (`cmake/toolchains/xernel-x86_64.cmake`) und
linkt echte Xernel-Programme. `./nhal build --xernel --test` startet die Tests
in QEMU auf Xernel. Was fehlt und warum, steht in `backends/xernel/README.md`.

## Web

Das Web-Backend baut mit Emscripten zu WebAssembly. Grenzen des Browsers, etwa
fehlende UDP-Sockets oder eine vergröberte Uhr, stehen als `reject` oder als
Eigenschaft in der Schnittstelle.

## Konsolen

Konsolen-SDKs stehen unter NDA. Das öffentliche Repo enthält deshalb nur die
Schnittstelle. Die Backends entstehen in privaten Repos, siehe
`backends/console_template/README.md`.

## Gesperrte Begriffe

`tools/guard` prüft Dateinamen, Inhalte und Commit-Nachrichten gegen eine Liste
gesperrter Begriffe. Die Liste steht nur als Hashes in der Datei. Aktivieren:

```sh
tools/guard/install-hooks.sh
```

Der Guard läuft auch als Test (`ctest`) und in der CI.
