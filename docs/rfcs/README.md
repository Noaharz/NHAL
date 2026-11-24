# RFCs

Neue Features und größere Änderungen am Katalog werden hier beschrieben, bevor
sie umgesetzt werden. Eine Datei pro Vorschlag: `NNNN-kurzer-name.md`.

Inhalt eines RFC:

1. **Feature-ID und Zweck**: wer braucht es (Engine, Server, Tool …)?
2. **Schnittstelle**: Entwurf der Funktionstabelle `_v1`.
3. **Semantik**: was überall gleich ist und was sich je Plattform unterscheidet
   (und wie die Schnittstelle das sichtbar macht).
4. **Plattform-Regeln**: `native`, `emulated`, `reject` oder `external`, jeweils mit Begründung.
5. **Abhängigkeiten** zu anderen Features.
6. **Tests**: was ein Backend erfüllen muss, um `native` eintragen zu dürfen.
