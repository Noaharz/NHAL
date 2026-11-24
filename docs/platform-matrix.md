# Plattform-Matrix

> Generiert von tools/nhalgen – nicht von Hand ändern. Quelle: `catalog/`.

Legende: **nativ** = eigenes Backend · **emuliert** = Ersatz mit beschriebenen
Unterschieden · **abgelehnt** = Start/Build bricht mit Grund ab · **extern** =
Backend außerhalb dieses Repos · *(kursiv)* = geplant (Entwurf) · – = kein Eintrag.

| Feature | Version | Status | macOS (Apple Silicon) | Windows | Linux | Xernel | Web (WebAssembly) | Konsolen |
|---|---|---|---|---|---|---|---|---|
| `audio.output` – Standard-Audioausgabe | 1 | draft | *(nativ)* | *(nativ)* | *(nativ)* | *(nativ)* | *(nativ)* | *(extern)* |
| `fs.async_read` – Asynchrones Lesen von Dateibereichen | 1 | draft | *(nativ)* | *(nativ)* | *(nativ)* | *(nativ)* | *(nativ)* | *(extern)* |
| `fs.file` – Synchrone Datei wie stdio (öffnen, lesen, schreiben, suchen) | 1 | draft | *(nativ)* | *(nativ)* | *(nativ)* | *(nativ)* | *(nativ)* | *(extern)* |
| `gfx.device` – GPU-Gerät, Ressourcen, Pipelines, Befehlslisten | 1 | draft | *(nativ)* | *(nativ)* | *(nativ)* | *(nativ)* | *(nativ)* | *(extern)* |
| `gfx.mesh_shader` – Mesh- und Task-Shader | 1 | draft | *(nativ)* | *(nativ)* | *(nativ)* | *(nativ)* | *(abgelehnt)* | *(extern)* |
| `input.gamepad` – Gamepads: Achsen, Tasten, Ereignisse | 1 | draft | *(nativ)* | *(nativ)* | *(nativ)* | *(nativ)* | *(nativ)* | *(extern)* |
| `mem.frame` – Linearer Frame-Allocator, pro Frame zurückgesetzt | 1 | draft | *(nativ)* | *(nativ)* | *(nativ)* | *(nativ)* | *(nativ)* | *(extern)* |
| `mem.pool` – Pool-Allocator für Objekte fester Größe | 1 | draft | *(nativ)* | *(nativ)* | *(nativ)* | *(nativ)* | *(nativ)* | *(extern)* |
| `net.socket_udp` – UDP-Sockets | 1 | draft | *(nativ)* | *(nativ)* | *(nativ)* | *(nativ)* | *(abgelehnt)* | *(extern)* |
| `thread.scheduler` – Task-Scheduler mit Abhängigkeiten und Prioritäten | 1 | draft | *(nativ)* | *(nativ)* | *(nativ)* | *(nativ)* | *(emuliert)* | *(extern)* |
| `time.monotonic` – Monotone Uhr mit Nanosekunden-Werten | 1 | stable | nativ | nativ | nativ | abgelehnt | nativ | extern |

## Anmerkungen

- `gfx.mesh_shader` auf Web (WebAssembly): WebGPU kennt keine Mesh-Shader
- `net.socket_udp` auf Web (WebAssembly): Browser erlauben keine UDP-Sockets (WebTransport wäre ein eigenes Feature)
- `thread.scheduler` auf Web (WebAssembly): ohne SharedArrayBuffer laufen Tasks nacheinander auf dem Hauptthread
- `time.monotonic` auf macOS (Apple Silicon): CLOCK_UPTIME_RAW, pausiert im Ruhezustand
- `time.monotonic` auf Windows: QueryPerformanceCounter; Verhalten im Ruhezustand noch nicht verifiziert
- `time.monotonic` auf Linux: CLOCK_MONOTONIC, pausiert im Ruhezustand
- `time.monotonic` auf Xernel: Xernel-Backend folgt
- `time.monotonic` auf Web (WebAssembly): performance.now(); der Browser vergröbert die Auflösung, NHAL misst sie beim Start

## Plattformen

- **Xernel**: Eigenständiges Betriebssystem. Bis Xernel eine libc mitbringt, baut NHAL dort freestanding, und der Kern braucht einen Allocator vom Aufrufer.
- **Web (WebAssembly)**: Gebaut mit Emscripten. Im Browser gibt es keinen direkten Zugriff auf Dateisystem oder Sockets des Betriebssystems; fs.* läuft über das virtuelle Dateisystem bzw. HTTP.
- **Konsolen**: Konsolen-SDKs stehen unter NDA. Backends liegen in privaten Repos lizenzierter Entwickler; hier gibt es nur die Schnittstelle.
