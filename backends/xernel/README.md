# Backend: Xernel

Xernel ist ein eigenständiges Betriebssystem (Mikrokernel in Rust, x86_64),
so wie Linux eines ist, und für NHAL eine Plattform wie jede andere: `--xernel`.
NHAL nutzt die C-Bibliothek von Xernel, **xlibc**.

## Bauen und testen

```sh
export XERNEL_ROOT=<pfad>/Xernel/Xernel      # Xernel-Workspace (enthält libs/xlibc)
./nhal build --xernel                        # Bibliothek und Testprogramme
./nhal build --xernel --test                 # dazu: Tests in QEMU auf Xernel ausführen
```

- `cmake/toolchains/xernel-x86_64.cmake` übersetzt mit clang nach den Regeln
  des Xernel-Userlands: kein SSE/MMX, keine Red Zone, statisch, nicht
  positionsunabhängig. Gelinkt wird mit `rust-lld`, dem Linkerskript des
  Userlands und `libxlibc.a` (inklusive `crt0`).
- Vorher muss `libxlibc.a` gebaut sein (im Xernel-Workspace, siehe dessen
  xlibc-Doku). `XERNEL_PROFILE=release` nimmt die Release-Variante, Standard
  ist `debug`.
- `--test` startet jedes Testprogramm mit `cargo xtask run --init <programm>`
  als `init.elf` in QEMU, liest die serielle Konsole und wertet das Ergebnis
  aus (`tools/xernel_qemu.py`). Am Xernel-Workspace selbst ändert das nichts.

Gebraucht werden `clang`, `cargo` mit der Xernel-Toolchain (bringt `llvm-ar`
und `rust-lld` mit), `qemu-system-x86_64` und `xorriso`.

## Was NHAL auf Xernel nutzt

| Xernel / xlibc | NHAL |
|---|---|
| `malloc`/`free` | Standard-Allocator (`allocator.c`). Ausrichtungen über 16 Byte richtet NHAL selbst aus, weil xlibc kein `aligned_alloc` hat |
| – | `mem.pool`, `mem.frame` (plattformunabhängig, laufen nativ) |

## Was noch fehlt (und warum)

| Feature | Grund | Was Xernel bräuchte |
|---|---|---|
| `time.monotonic` | Der LAPIC-Timer ist nicht kalibriert: `SYS_GET_TICKS` zählt Ticks unbekannter Dauer; xlibc hat noch kein `time.h` | Kalibrierung des Timers und eine Abfrage der Tick-Frequenz, dann `clock_gettime(CLOCK_MONOTONIC)` |
| `fs.file`, `fs.async_read` | Deskriptoren gibt es nur für 0, 1 und 2; der VFS-Server ist noch ein Platzhalter | Datei-Server mit fd-Protokoll, dann `open`/`read`/`close` in xlibc |
| `thread.scheduler` | Ein Thread pro Prozess | Threads im selben Adressraum (dann ein Backend auf Basis der Xernel-Threads) |

Sobald xlibc das anbietet, nutzen die Features einfach das POSIX-Backend.
Im Katalog wird dann aus `reject` ein `native`.
