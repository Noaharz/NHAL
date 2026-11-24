# Backend: Xernel

Xernel ist ein eigenständiges Betriebssystem, so wie Linux eines ist, und für
NHAL eine Plattform wie jede andere: `--xernel`.

Solange Xernel keine libc mitbringt, baut NHAL dort freestanding
(`freestanding = true` in `catalog/platforms.toml`):

- nur freestanding-Header (`stddef.h`, `stdint.h`, `stdbool.h` …)
- Speicher ausschließlich über den `nhal_allocator`, den `init()` bekommt
- `nhal_init` verlangt einen Allocator vom Aufrufer

Bauen:

```sh
./nhal build --xernel                                   # Prüfbuild mit Host-Compiler
./nhal build --xernel --toolchain xernel=xernel.cmake   # mit echter Xernel-Toolchain
```

Ohne Toolchain-Datei übersetzt der Host-Compiler NHAL mit `-ffreestanding`.
Das prüft, dass alles ohne libc baut, erzeugt aber noch keine Xernel-Binärdatei.
