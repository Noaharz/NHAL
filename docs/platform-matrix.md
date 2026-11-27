# Plattform-Matrix

> Generiert von tools/nhalgen – nicht von Hand ändern. Quelle: `catalog/`.

Legende: **nativ** = eigenes Backend · **emuliert** = Ersatz mit beschriebenen
Unterschieden · **geplant** = vorgesehen, Backend fehlt noch · **abgelehnt** = gibt
es dort nicht (Build/Start bricht mit Grund ab) · **extern** = Backend außerhalb
dieses Repos · *(kursiv)* = Feature ist noch ein Entwurf · – = kein Eintrag.

| Feature | Version | Status | macOS (Apple Silicon) | Windows | Linux | Xernel | Web (WebAssembly) | Konsolen |
|---|---|---|---|---|---|---|---|---|
| `audio.output` – Ausgabe auf das Standard-Audiogerät (Float-Samples per Render-Funktion) | 1 | experimental | nativ | geplant | geplant | geplant | nativ | extern |
| `fs.async_read` – Dateibereiche asynchron lesen, Abholen per poll() | 1 | experimental | nativ | nativ | nativ | abgelehnt | emuliert | extern |
| `fs.file` – Dateien und Ordner, synchron (öffnen, lesen, schreiben, suchen, auflisten) | 1 | experimental | nativ | nativ | nativ | abgelehnt | nativ | extern |
| `gfx.device` – GPU-Gerät, Ressourcen, Pipelines, Befehlslisten, Oberflächen | 2 | experimental | nativ | geplant | geplant | geplant | nativ | extern |
| `gfx.mesh_shader` – Mesh- und Object-/Task-Shader | 1 | experimental | nativ | geplant | geplant | geplant | abgelehnt | extern |
| `gfx.raytracing` – Beschleunigungsstrukturen (BLAS/TLAS) und Ray Queries | 1 | experimental | nativ | geplant | geplant | geplant | abgelehnt | extern |
| `input.actions` – Aktionen mit Bindungen an Tasten, Maus und Gamepad, Standard-Belegung | 1 | experimental | nativ | nativ | nativ | nativ | nativ | nativ |
| `input.gamepad` – Gamepads mit einheitlicher Belegung, Zustand per poll() | 1 | experimental | nativ | geplant | geplant | geplant | nativ | extern |
| `input.state` – Zustand von Tastatur, Maus und Touch mit Flanken (pressed/released je Frame) | 1 | experimental | nativ | nativ | nativ | nativ | nativ | nativ |
| `mem.frame` – Linearer Frame-Allocator mit Marken und Reset | 1 | experimental | nativ | nativ | nativ | nativ | nativ | extern |
| `mem.pool` – Pool-Allocator für Objekte fester Größe | 1 | experimental | nativ | nativ | nativ | nativ | nativ | extern |
| `net.socket_udp` – UDP-Sockets, nicht blockierend | 1 | experimental | nativ | geplant | nativ | geplant | abgelehnt | extern |
| `sys.shell` – Programm-, Ressourcen- und Nutzerdaten-Ordner; URLs und Dateien im System öffnen | 1 | experimental | nativ | geplant | geplant | geplant | nativ | extern |
| `thread.scheduler` – Task-Scheduler mit Abhängigkeiten und Prioritäten | 1 | experimental | nativ | nativ | nativ | abgelehnt | emuliert | extern |
| `time.monotonic` – Monotone Uhr mit Nanosekunden-Werten | 1 | stable | nativ | nativ | nativ | abgelehnt | nativ | extern |
| `window.basic` – Fenster und Ereignisse: Tastatur, Maus, Text/IME, Drops, Touch, Stift, Fokus, Größe | 1 | experimental | nativ | geplant | geplant | geplant | nativ | extern |

## Anmerkungen

- `audio.output` auf macOS (Apple Silicon): CoreAudio (Default-Output-Unit), render() auf dem Echtzeit-Thread
- `audio.output` auf Windows: WASAPI-Backend folgt
- `audio.output` auf Linux: PipeWire/ALSA-Backend folgt
- `audio.output` auf Xernel: braucht einen Audio-Treiber in Xernel
- `audio.output` auf Web (WebAssembly): Web Audio (ScriptProcessorNode), render() im Hauptthread; startet erst nach einer Nutzeraktion
- `fs.async_read` auf macOS (Apple Silicon): zwei Lese-Threads, pread
- `fs.async_read` auf Windows: zwei Lese-Threads, ReadFile mit Offset
- `fs.async_read` auf Linux: zwei Lese-Threads, pread
- `fs.async_read` auf Xernel: Der VFS-Server von Xernel ist noch ein Platzhalter
- `fs.async_read` auf Web (WebAssembly): ohne pthreads wird erst in poll() gelesen, synchron aus dem virtuellen Dateisystem
- `fs.file` auf Windows: Win32 mit UTF-16-Pfaden
- `fs.file` auf Xernel: Der VFS-Server von Xernel ist noch ein Platzhalter
- `fs.file` auf Web (WebAssembly): virtuelles Dateisystem von Emscripten im Speicher, nicht die Platte des Rechners
- `gfx.device` auf macOS (Apple Silicon): Metal; Shader in MSL; Unified Memory, HOST_VISIBLE-Puffer dauerhaft gemappt; wait() blockiert (MTLSharedEvent)
- `gfx.device` auf Windows: D3D12-Backend folgt
- `gfx.device` auf Linux: Vulkan-Backend folgt
- `gfx.device` auf Xernel: braucht einen GPU-Treiber in Xernel
- `gfx.device` auf Web (WebAssembly): WebGPU; Shader in WGSL; Gerät kommt asynchron (ready()), kein dauerhaftes Mapping, wait() blockiert nicht, Push-Konstanten über Uniform-Ring
- `gfx.mesh_shader` auf macOS (Apple Silicon): Metal 3 (Object- und Mesh-Funktionen); nhal_init meldet UNAVAILABLE ohne passende GPU
- `gfx.mesh_shader` auf Windows: D3D12 Mesh Shader (Amplification/Mesh) folgen mit dem D3D12-Backend
- `gfx.mesh_shader` auf Linux: VK_EXT_mesh_shader folgt mit dem Vulkan-Backend
- `gfx.mesh_shader` auf Xernel: braucht zuerst gfx.device auf Xernel
- `gfx.mesh_shader` auf Web (WebAssembly): WebGPU kennt keine Mesh-Shader
- `gfx.raytracing` auf macOS (Apple Silicon): Metal (MTLAccelerationStructure, Ray Queries in MSL); nhal_init meldet UNAVAILABLE ohne passende GPU
- `gfx.raytracing` auf Windows: DXR (Inline-Raytracing) folgt mit dem D3D12-Backend
- `gfx.raytracing` auf Linux: VK_KHR_acceleration_structure und VK_KHR_ray_query folgen mit dem Vulkan-Backend
- `gfx.raytracing` auf Xernel: braucht zuerst gfx.device auf Xernel
- `gfx.raytracing` auf Web (WebAssembly): WebGPU kennt kein Raytracing
- `input.actions` auf Konsolen: plattformunabhängig, läuft überall mit
- `input.gamepad` auf macOS (Apple Silicon): GameController-Framework; auch ohne Fokus
- `input.gamepad` auf Windows: XInput/GameInput-Backend folgt
- `input.gamepad` auf Linux: evdev-Backend folgt
- `input.gamepad` auf Xernel: braucht einen HID-Treiber in Xernel
- `input.gamepad` auf Web (WebAssembly): Gamepad API; Browser melden ein Pad erst nach dem ersten Tastendruck darauf
- `input.state` auf Konsolen: plattformunabhängig, läuft überall mit
- `net.socket_udp` auf Windows: Winsock-Backend folgt
- `net.socket_udp` auf Xernel: braucht Sockets in xlibc (Netzstack-Server von Xernel)
- `net.socket_udp` auf Web (WebAssembly): Browser erlauben keine UDP-Sockets (WebTransport wäre ein eigenes Feature)
- `sys.shell` auf macOS (Apple Silicon): NSBundle, Application Support, NSWorkspace
- `sys.shell` auf Windows: ShellExecuteW und Known Folders folgen
- `sys.shell` auf Linux: xdg-open und XDG-Verzeichnisse folgen
- `sys.shell` auf Xernel: braucht Dateien und Prozessstart in Xernel
- `sys.shell` auf Web (WebAssembly): Ordner im virtuellen FS (flüchtig); open() nur für URLs, neuer Tab
- `thread.scheduler` auf macOS (Apple Silicon): Worker-Threads: Kerne - 1
- `thread.scheduler` auf Windows: Worker-Threads: Kerne - 1
- `thread.scheduler` auf Linux: Worker-Threads: Kerne - 1
- `thread.scheduler` auf Xernel: Xernel hat noch keine Threads, nur Prozesse
- `thread.scheduler` auf Web (WebAssembly): ohne pthreads laufen Aufgaben sofort im aufrufenden Thread
- `time.monotonic` auf macOS (Apple Silicon): CLOCK_UPTIME_RAW, pausiert im Ruhezustand
- `time.monotonic` auf Windows: QueryPerformanceCounter; Verhalten im Ruhezustand noch nicht verifiziert
- `time.monotonic` auf Linux: CLOCK_MONOTONIC, pausiert im Ruhezustand
- `time.monotonic` auf Xernel: Der Xernel-Timer ist nicht kalibriert: SYS_GET_TICKS zählt Ticks unbekannter Dauer
- `time.monotonic` auf Web (WebAssembly): performance.now(); der Browser vergröbert die Auflösung, NHAL misst sie beim Start
- `window.basic` auf macOS (Apple Silicon): Cocoa (NSWindow/NSView, NSTextInputClient); nur im Hauptthread; kein Touch (Trackpad = Maus)
- `window.basic` auf Windows: Win32-Backend folgt
- `window.basic` auf Linux: Wayland-Backend (mit X11 als Rückfall) folgt
- `window.basic` auf Xernel: braucht einen Fenster-Server in Xernel
- `window.basic` auf Web (WebAssembly): Canvas und DOM-Ereignisse (Pointer Events, IME über ein verstecktes Textfeld); kein QUIT, wait_events blockiert nicht

## Plattformen

- **Xernel**: Eigenständiges Betriebssystem (Mikrokernel, x86_64) mit eigener C-Bibliothek xlibc. Gebaut wird gegen den Xernel-Workspace in XERNEL_ROOT.
- **Web (WebAssembly)**: Gebaut mit Emscripten. Im Browser gibt es keinen direkten Zugriff auf Dateisystem oder Sockets des Betriebssystems; fs.* läuft über das virtuelle Dateisystem bzw. HTTP.
- **Konsolen**: Konsolen-SDKs stehen unter NDA. Backends liegen in privaten Repos lizenzierter Entwickler; hier gibt es nur die Schnittstelle.
