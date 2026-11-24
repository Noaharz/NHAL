# Backend-Vorlage: Konsolen

Die SDKs von Konsolenherstellern stehen unter NDA. Ihr Code darf deshalb nicht
in diesem öffentlichen Repo liegen. Konsolen-Backends entstehen in privaten
Repos lizenzierter Entwickler und implementieren dieselbe Schnittstelle wie
alle anderen Backends:

1. Für jedes Feature ein Symbol `const nhal_feature_impl nhal_impl_<feature>;`
   (siehe `include/nhal/nhal_backend.h`).
2. Im Katalog steht für `console` die Policy `external`.

Die Einbindung externer Backends in den Build (Katalog-Overlay mit eigenen
Quellen) ist für eine spätere Phase geplant, siehe `docs/roadmap.md`.
