# Fahrplan

| Phase | Inhalt | Ergebnis |
|---|---|---|
| **0 – Fundament** ✅ | Lizenz, CMake, Katalog-Schema, `nhalgen`, `./nhal` (Plattform- und Feature-Auswahl), Kern, `time.monotonic`, Tests, CI, Guard | baubares Gerüst, erstes Feature auf macOS/Linux/Windows/Web |
| **1 – System** ✅ | `fs.file`, `fs.async_read`, `mem.pool`, `mem.frame`, `thread.scheduler`; Xernel mit xlibc: echte Toolchain, Allocator, Tests in QEMU | ein Werkzeug ohne Fenster läuft nur mit NHAL (`examples/files_minimal`) |
| **2 – Spiele-Pfad** (macOS ✅) | `gfx.device` v2 (Metal ✅, WebGPU ✅, Vulkan), `gfx.mesh_shader` (Metal ✅), `window.basic` ✅, `input.state` ✅, `input.actions` ✅, `input.gamepad` ✅, `audio.output` ✅, `net.socket_udp` ✅, `sys.shell` ✅ | ein kleines Spiel läuft auf macOS und Linux (`examples/game_input` läuft auf macOS und im Web) |
| **3 – Windows** | D3D12, IOCP, XInput | alle drei Desktop-Plattformen |
| **3b – Web** ✅ | `fs.*` (virtuelles FS), `gfx.device` (WebGPU), `audio.output` (Web Audio), `input.gamepad`; Tests in headless Chrome (`--browser`) | Spiele-Pfad im Browser |
| **4 – Xernel** | `time.monotonic` (sobald der Timer kalibriert ist), `fs.*` (sobald es einen Datei-Server gibt), Threads | alle System-Features auf Xernel |
| **5 – v0.1** | Doku, Beispiele, Plattform-Matrix vollständig | erste öffentliche Version |
| **später** | externe Backends (Katalog-Overlay), Konsolen-Vorlage, `net.*`, `compute.tensor_*`, statische Bindung ohne Tabellen, Bindings (Rust, C#) | |

## Als Nächstes (vereinbarte Reihenfolge)

1. ✅ **`gfx.device` v2:** Gruppen mit Layouts (zusätzlich zu Slots),
   Seriennummern für Frames in Flight, MRT, MSAA mit resolve, Scissor,
   Indirect-Draws und -Dispatches, Occlusion- und Zeitstempel-Abfragen,
   Push-Konstanten, Tiefenvergleich, Blend-Arten, Schreibmasken, Drehsinn.
   Offen: Stencil, Mip-Stufen und Textur-Arrays, Depth-Bias.
   Als Nächstes `gfx.raytracing` (BLAS/TLAS auf Metal; Web: abgelehnt,
   WebGPU kennt kein Raytracing).
2. **Audio:** AudioWorklet statt ScriptProcessorNode im Web; darüber
   `audio.mixer` (Sounds, Busse, Filter) und Decoder (WAV zuerst).
3. **Dateien:** `fs.async_write`, Ergebnis-Handles zum Warten, HTTP-Range im
   Browser, festgelegter I/O-Thread.
4. **2D und UI:** `gfx2d.batch` (Sprites, Tilemaps), Text-Rendering, ein
   Gerät für Immediate-Mode-UIs – alles plattformunabhängig auf `gfx.device`.
5. **System:** `sys.info`, `sys.crash_report`, `thread.basic`,
   `ipc.local_socket`, ein GPU-Allocator über `gfx.device`.

## Offene Designfragen

- **`fs.*` über HTTP im Web:** heute nur das virtuelle Dateisystem von
  Emscripten. Ein Backend mit `fetch` und Range-Anfragen fehlt noch.
- **Fenster:** `gfx.device` nimmt eine fertige Oberfläche (CAMetalLayer,
  NSView, Canvas). Ein eigenes Feature `window.*` (Fenster, Ereignisse,
  Tastatur, Maus) gehört in Phase 2 für Linux und Windows.

- **Statische Bindung:** Bei genau einem Backend pro Build könnten die
  Feature-Header die Funktionen auch direkt deklarieren (ohne Zeiger), etwa per
  `NHAL_STATIC_DISPATCH`.
- **Externe Backends:** ein Katalog-Overlay (`NHAL_EXTERNAL_BACKEND_DIR`), das
  `external`-Einträge mit eigenen Quellen füllt.
- **Mehrere Versionen gleichzeitig:** Ein Backend bietet `_v1` und `_v2` an,
  damit Apps schrittweise umsteigen können.
