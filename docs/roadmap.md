# Fahrplan

| Phase | Inhalt | Ergebnis |
|---|---|---|
| **0 – Fundament** ✅ | Lizenz, CMake, Katalog-Schema, `nhalgen`, `./nhal` (Plattform- und Feature-Auswahl), Kern, `time.monotonic`, Tests, CI, Guard | baubares Gerüst, erstes Feature auf macOS/Linux/Windows/Web |
| **1 – System** | `fs.file`, `fs.async_read`, `mem.pool`, `mem.frame`, `thread.scheduler` | ein kleiner Server läuft nur mit NHAL |
| **2 – Spiele-Pfad** | `gfx.device` (Metal, Vulkan), `gfx.compute`, `input.*`, `audio.output` | ein kleines Spiel läuft auf macOS und Linux |
| **3 – Windows** | D3D12, IOCP, XInput | alle drei Desktop-Plattformen |
| **3b – Web** | `fs.*` (fetch/Range), `gfx.device` (WebGPU), `audio.output` (WebAudio), `input.gamepad` | Spiele-Pfad im Browser |
| **4 – Xernel** | Xernel-Backends für `mem.*`, `time.*`, I/O; Xernel-Toolchain-Datei | NHAL-Apps laufen auf Xernel |
| **5 – v0.1** | Doku, Beispiele, Plattform-Matrix vollständig | erste öffentliche Version |
| **später** | externe Backends (Katalog-Overlay), Konsolen-Vorlage, `net.*`, `compute.tensor_*`, statische Bindung ohne Tabellen, Bindings (Rust, C#) | |

## Offene Designfragen

- **Statische Bindung:** Bei genau einem Backend pro Build könnten die
  Feature-Header die Funktionen auch direkt deklarieren (ohne Zeiger), etwa per
  `NHAL_STATIC_DISPATCH`.
- **Externe Backends:** ein Katalog-Overlay (`NHAL_EXTERNAL_BACKEND_DIR`), das
  `external`-Einträge mit eigenen Quellen füllt.
- **Mehrere Versionen gleichzeitig:** Ein Backend bietet `_v1` und `_v2` an,
  damit Apps schrittweise umsteigen können.
