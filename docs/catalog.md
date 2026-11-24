# Katalog-Format

Jedes Feature ist eine Datei `catalog/<gruppe>/<name>.toml`. Der Pfad muss zur
ID passen: `gfx.mesh_shader` liegt in `catalog/gfx/mesh_shader.toml`.

```toml
id = "time.monotonic"          # gruppe.name, klein, Ziffern und _ erlaubt
version = 1                    # Version der Funktionstabelle
status = "stable"              # stable | experimental | draft
summary = "Monotone Uhr mit Nanosekunden-Werten"
requires = []                  # andere Feature-IDs

[platforms.macos]
policy = "native"              # native | emulated | reject | external
sources = ["backends/posix/time/monotonic.c"]
link = []                      # Bibliotheken/Frameworks, z. B. ["-framework Metal"]
note = "CLOCK_UPTIME_RAW, pausiert im Ruhezustand"

[platforms.xernel]
policy = "reject"
note = "Xernel-Backend folgt"
```

## Regeln, die `nhalgen check` prüft

- `id`, `version`, `status` und `summary` sind gesetzt und gültig.
- Der Pfad passt zur ID, und keine ID ist doppelt.
- Alle `requires` existieren, und es gibt keine Zyklen.
- Ein nicht-Entwurf hängt nicht von einem Entwurf ab.
- `native`/`emulated` haben `sources` (außer bei Entwürfen), und die Dateien existieren.
- `reject`/`external` haben keine `sources`/`link`.
- Ein nicht-Entwurf hat seinen Header `include/nhal/features/<gruppe>/<name>.h`.
- Plattformen sind in `catalog/platforms.toml` bekannt.
- Die Hashes der IDs kollidieren nicht und sind nicht 0.

## Status

| Status | Bedeutung |
|---|---|
| `draft` | Geplant. Kann nicht gewählt werden und erscheint *kursiv* in der Matrix |
| `experimental` | Benutzbar, die Schnittstelle kann sich noch ändern |
| `stable` | Änderungen nur noch kompatibel oder über eine neue Version |

## Manifest der App (`nhal.toml`)

```toml
[platforms]
targets = ["macos", "xernel"]   # oder ["all"]

[features]
required = ["time.monotonic", "fs.async_read"]
optional = ["gfx.mesh_shader"]
```

- Abhängigkeiten werden automatisch mitgenommen. Die Abhängigkeiten eines
  Pflicht-Features sind ebenfalls Pflicht.
- Ein optionales Feature, das auf der Plattform nicht geht, wird mit Hinweis
  weggelassen. Features, die davon abhängen, fallen ebenfalls weg.
- Ohne Manifest nimmt der Build alle verfügbaren Features als optional.
- Alles lässt sich auch ohne Manifest angeben: `./nhal build --xernel --macos
  --features a,b --optional c`. Flags auf der Kommandozeile ergänzen das Manifest,
  Plattform-Flags ersetzen `targets`.

## Plattformen (`catalog/platforms.toml`)

```toml
[xernel]
name = "Xernel"
freestanding = true   # ohne libc bauen, Allocator vom Aufrufer
note = "…"

[console]
name = "Konsolen"
external = true       # Backends außerhalb dieses Repos
```

Die ID ist zugleich das Flag von `./nhal` (`--xernel`).

## Neues Feature anlegen

1. Entwurf unter `docs/rfcs/` beschreiben (Semantik, Plattform-Unterschiede).
2. `catalog/<gruppe>/<name>.toml` mit `status = "draft"` anlegen.
3. Header und Implementierungen schreiben, dann den Status auf `experimental` setzen.
4. `python3 tools/nhalgen/nhalgen.py matrix` ausführen, damit die Matrix aktuell ist.
