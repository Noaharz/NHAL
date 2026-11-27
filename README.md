# NHAL

**Ein modulares Hardware Abstraction Layer mit Feature-Katalog.**

NHAL bietet einen großen Katalog klar abgegrenzter Fähigkeiten, etwa
`gfx.mesh_shader`, `audio.output`, `fs.async_read` oder `net.socket_udp`. Jede
Anwendung nimmt daraus nur das mit, was sie wirklich nutzt. NHAL übersetzt diese
Auswahl auf die jeweilige Plattform.

- Eine **Game Engine** nimmt Grafik, Input, Audio und asynchrones I/O und lässt
  Server-Module weg.
- Ein **Cloud-Dienst** nimmt Netzwerk, Storage und Compute und lässt
  Gamepad-Haptik und Fenster weg.

> Status: **Phase 2 für macOS und Web** – Kern, Katalog, Werkzeug und 15 Features:
> System (`time.monotonic`, `mem.pool`, `mem.frame`, `fs.file`, `fs.async_read`,
> `thread.scheduler`, `net.socket_udp`, `sys.shell`), Fenster und Eingabe
> (`window.basic`, `input.state`, `input.actions`, `input.gamepad`), Grafik
> (`gfx.device` v2 mit Metal und WebGPU – Gruppen, Push-Konstanten, Frames in
> Flight, MRT, MSAA, Indirect, Abfragen –, `gfx.mesh_shader` mit Metal) und
> `audio.output`. Auf macOS und im Web ist jedes Feature gebaut und getestet
> oder mit Grund abgelehnt. [examples/game_input](examples/game_input/main.c)
> zeigt Fenster, Grafik und Eingabe zusammen. Alles Weitere steht in
> [docs/roadmap.md](docs/roadmap.md).

## Grundsätze

1. **Winziger Pflichtkern.** Er enthält nur Status, Manifest, Fähigkeiten-Abfrage
   und Funktionstabellen. Er ist C11 und freestanding-fähig, läuft also auch ohne
   libc.
2. **Nur mitnehmen, was gebraucht wird.** Die App beschreibt ihre Features in
   einem Manifest (`nhal.toml`). Alles andere wird gar nicht erst kompiliert.
3. **Binden beim Bauen und beim Start, nie pro Aufruf.** Die App holt jede
   Funktionstabelle einmal. Im Hot Path gibt es keine Prüfungen.
4. **Kein stiller Ersatz.** Jedes Feature hat pro Plattform eine klare Regel:
   `native`, `emulated`, `reject` oder `external`. Ein fehlendes Pflicht-Feature
   bricht den Build oder Start ab und nennt den Grund.
5. **Unterschiede beschreiben statt verstecken.** Wo sich Hardware wirklich
   unterscheidet (z. B. Unified Memory gegenüber getrenntem VRAM), steht das
   als abfragbare Eigenschaft in der Schnittstelle.

## Plattformen

| Flag | Plattform |
|---|---|
| `--macos` | macOS (Apple Silicon) |
| `--windows` | Windows |
| `--linux` | Linux |
| `--xernel` | [Xernel](backends/xernel/README.md), ein eigenständiges Betriebssystem |
| `--web` | Web (WebAssembly, Emscripten) |
| `--console` | Konsolen (Backends wegen NDA extern) |
| `--all` | alle oben |

Den aktuellen Stand je Feature zeigt die [Plattform-Matrix](docs/platform-matrix.md).

## Schnellstart

Voraussetzungen: CMake ≥ 3.21, ein C11-Compiler, Python ≥ 3.11. Für `--web`
zusätzlich Emscripten.

Plattformen und Features lassen sich frei kombinieren:

```sh
./nhal list                                          # was kennt NHAL?
./nhal check --all --features time.monotonic         # geht das überall?
./nhal build --xernel --macos --features time.monotonic
./nhal build --web --manifest examples/minimal/nhal.toml
./nhal build --test                                  # Host-Plattform, alle Features, mit Tests
./nhal build --web --test --browser                  # Web-Tests zusätzlich in headless Chrome
```

`check` zeigt eine Tabelle Feature × Plattform (nativ, emuliert, extern,
weggelassen, FEHLT) mit den Gründen. `build` legt je Plattform einen Ordner
`build/<plattform>/` an. Ohne Plattform-Angabe gilt die Host-Plattform, ohne
Feature-Angabe gilt alles Verfügbare.

Plattformen ohne Toolchain baut `nhal` nicht still mit. Es meldet, was fehlt:
`--toolchain windows=win.cmake` gibt die Toolchain an, `--skip-unavailable`
macht aus solchen Plattformen bloße Hinweise.

Direkt mit CMake geht es auch:

```sh
cmake -S . -B build -DNHAL_PLATFORM=linux -DNHAL_MANIFEST=nhal.toml
```

## In eine App einbinden

`nhal.toml` neben der App:

```toml
[platforms]
targets = ["macos", "xernel", "web"]   # oder ["all"]; Standard für ./nhal ohne Plattform-Flags

[features]
required = ["time.monotonic"]
optional = ["gfx.mesh_shader"]
```

`CMakeLists.txt`:

```cmake
set(NHAL_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/nhal.toml" CACHE FILEPATH "" FORCE)
add_subdirectory(extern/nhal)
target_link_libraries(meine_app PRIVATE nhal::nhal)
```

Code:

```c
#include "nhal/nhal_core.h"
#include "nhal/features/time/monotonic.h"

nhal_context* ctx;
nhal_status s = nhal_init(NULL, &ctx);          /* Manifest aus nhal.toml */
if (!nhal_ok(s)) { /* s.feature und s.reason sagen, was fehlt */ }

const nhal_time_monotonic_v1* clock = nhal_get_time_monotonic_v1(ctx); /* einmal */
uint64_t t = clock->now_ns();                                          /* Hot Path */

nhal_shutdown(ctx);
```

Beispiele: [examples/minimal](examples/minimal) (Start, Fähigkeiten, Uhr) und
[examples/files_minimal](examples/files_minimal) (Werkzeug ohne Fenster: Ordner
lesen, asynchron laden, Prüfsummen parallel).

## Aufbau des Repos

| Ordner | Inhalt |
|---|---|
| `catalog/` | Der Feature-Katalog: eine TOML-Datei pro Feature, dazu `platforms.toml` |
| `include/nhal/` | Öffentliche C-Header: Kern, Backend-Schnittstelle, ein Header pro Feature |
| `core/` | Der Pflichtkern |
| `backends/` | Implementierungen je Plattform (`posix`, `windows`, `xernel`, `web`, …) |
| `nhal`, `tools/nhal.py` | Werkzeug: Plattformen und Features wählen, prüfen, bauen |
| `tools/nhalgen/` | Generator: Katalog + Auswahl → Header, Registry, Build-Listen |
| `tools/guard/` | Prüft Dateien und Commit-Nachrichten auf gesperrte Begriffe |
| `tests/` | Tests für Kern und Generator |
| `examples/` | Beispiel-Apps |
| `docs/` | Architektur, Katalog-Format, Plattform-Matrix, Fahrplan |

## Dokumentation

- [Architektur](docs/architecture.md)
- [Katalog-Format](docs/catalog.md)
- [Plattform-Matrix](docs/platform-matrix.md)
- [Fahrplan](docs/roadmap.md)

## Beiträge

Derzeit werden keine externen Beiträge angenommen, siehe [CONTRIBUTING.md](CONTRIBUTING.md).

## Lizenz

[MIT](LICENSE)
