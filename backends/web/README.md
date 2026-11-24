# Backend: Web (WebAssembly)

Gebaut mit [Emscripten](https://emscripten.org). `emcmake` muss im `PATH` liegen
(emsdk installieren und `source emsdk_env.sh`).

```sh
./nhal build --web
./nhal build --web --test     # Tests laufen mit node, falls vorhanden
```

Besonderheiten des Browsers, die NHAL beschreibt, statt sie zu verstecken:

- `time.monotonic`: Der Browser vergröbert `performance.now()`. NHAL misst die
  tatsächliche Schrittweite beim Start.
- Kein direkter Zugriff auf das Dateisystem oder die Sockets des
  Betriebssystems. `fs.*` läuft über das virtuelle Dateisystem von Emscripten
  bzw. HTTP. Was gar nicht geht (z. B. `net.socket_udp`), steht im Katalog mit
  `reject` und einer Begründung.
