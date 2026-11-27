# Backend: Web (WebAssembly)

Gebaut mit [Emscripten](https://emscripten.org). `emcmake` muss im `PATH` liegen
(emsdk installieren und `source emsdk_env.sh`).

```sh
./nhal build --web
./nhal build --web --test             # Tests laufen mit node, falls vorhanden
./nhal build --web --test --browser   # zusätzlich in headless Chrome
```

In node fehlen WebGPU, Web Audio und die Gamepad API; die Tests melden dort den
Grund und bestehen. `--browser` startet `tools/web_browser_test.mjs`: Es liefert
`build/web/tests` über einen lokalen Server (nur 127.0.0.1) aus, öffnet jeden
Test in headless Chrome (`runner.html?test=<name>`) und wartet auf das Ergebnis.
Den Browser findet es am üblichen Ort oder über die Umgebungsvariable `CHROME`.
Von Hand geht es so:

```sh
python3 -m http.server -d build/web/tests 8000
# dann http://localhost:8000/runner.html?test=gfx öffnen
```

Besonderheiten des Browsers, die NHAL beschreibt, statt sie zu verstecken:

- `time.monotonic`: Der Browser vergröbert `performance.now()`. NHAL misst die
  tatsächliche Schrittweite beim Start.
- Kein direkter Zugriff auf das Dateisystem oder die Sockets des
  Betriebssystems. `fs.*` läuft über das virtuelle Dateisystem von Emscripten
  (ein Backend mit HTTP-Range-Anfragen ist geplant). Was gar nicht geht (z. B. `net.socket_udp`), steht im Katalog mit
  `reject` und einer Begründung.
- `gfx.device`: WebGPU, direkt über `EM_JS` angebunden (ohne fremde
  Bibliothek). Das Gerät kommt asynchron: Die App ruft pro Frame `poll()` und
  `ready()`. Ergebnisse (Lesen, Validierungsfehler) kommen erst an, wenn die
  Seite in die Ereignisschleife zurückkehrt. Es braucht also eine Hauptschleife
  (`emscripten_set_main_loop`). Dauerhaft gemappte Puffer (`HOST_VISIBLE`) gibt
  es nicht; `buffer_create` lehnt sie mit Grund ab. Der größere JS-Teil
  (Layouts, Pipelines, Passes) steht in `gfx/device.js`.
  Version 2: `wait()` blockiert nicht (`info().blocking_wait = false`), der
  Fortschritt kommt über `onSubmittedWorkDone` in `completed()` an.
  Push-Konstanten liegen in 64-KiB-Uniform-Blöcken (je Draw 256 Bytes), die
  beim `commands_submit` mit `writeBuffer` hochgeladen und nach Abschluss
  wiederverwendet werden. Occlusion meldet nur 0 oder 1, keine Anzahl.
  Zeitstempel und `first_instance` in Indirect-Draws gibt es, wenn der Adapter
  `timestamp-query` bzw. `indirect-first-instance` anbietet.
- `gfx.mesh_shader`: abgelehnt, WebGPU kennt keine Mesh-Shader.
- `audio.output`: Web Audio. Browser starten Audio erst nach einer
  Nutzeraktion; bis dahin meldet `state()` `SUSPENDED`.
- `input.gamepad`: Gamepad API. Ein Pad erscheint erst nach dem ersten
  Tastendruck darauf.
- `window.basic`: Ein Fenster ist eine Canvas. Der DOM-Teil steht in
  `window/basic.js` und wird als `--js-library` gelinkt (Katalogfeld
  `js_libraries`). Text und IME laufen über ein verstecktes Textfeld.
  `inject_key` erzeugt ein KeyboardEvent; weil Browser dafür keinen Text
  erzeugen, liefert NHAL den Text selbst – wie unter macOS. Abgelegte Dateien
  landen unter `/drop/` im virtuellen FS.
- `sys.shell`: Ordner liegen im virtuellen FS und sind flüchtig. `open()`
  öffnet nur URLs (neuer Tab, oft nur nach einer Nutzeraktion erlaubt).
