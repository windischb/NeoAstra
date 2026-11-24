# Windows R2 – Parentprüfung, 24. November 2026

**Nicht zur Übernahme freigegeben.** Der Patch wurde nur gelesen und mit
`git apply --check` geprüft, nicht angewendet. Kein Runtime-Staging, kein Commit.

## Befunde

1. **P1 – Verweigerte Requests entweichen beim View-Abbau.**
   `refuse_request` hält bei doppeltem Antwortfehler einen Deferral
   ([Patch](artifacts/windows-request-filter-handoff/windows-request-filter-r2.patch#L174)).
   Die finalen Logs zeigen nach Dispose `/b_img.png` und `/b_s.js` am gesperrten
   Ziel. Die instrumentierte Zeitlinie zeigt den GET mit 587 Bytes bereits
   während `Close()`, nicht erst bei Freigabe des gespeicherten Deferrals
   ([Zeitlinie](artifacts/windows-request-filter-handoff/r2-review-logs/logs/after6/failure.log#L141)).
   Der erfolgreich beobachtete Hold ist kein sicherer Abbruch.

2. **P1 – Hauptnavigationen umgehen den neuen Antwort-Fallback.**
   Die neue Navigationsprüfung setzt `remember_navigation(..., true)`.
   Im Ressourcenhandler wird zuerst `take_refused_navigation` behandelt:
   der bestehende 204-Pfad ignoriert Konstruktions-/Zuweisungsfehler und kehrt
   zurück. Die neue `request_refused`-/`refuse_request`-Prüfung folgt erst danach
   ([Originalpfad](native/src/windows/windows_backend.cpp#L1097),
   [Einfügestelle](artifacts/windows-request-filter-handoff/windows-request-filter-r2.patch#L345)).
   Dies ist ein bestätigter Kontrollflussbefund, noch kein separater Laufzeit-
   Regressionsnachweis. Eine spätere Korrektur muss den gemeinsamen Antwortpfad
   und gezielte Dokument-Fehlerinjektion umfassen.

3. **P1 – Die Wiederherstellungsaufforderung ist nicht sicher.**
   `WEB_PROCESS_TERMINATED`/`RECREATE_VIEW` fordert im Fehlerpfad eine
   Wiederherstellung an, deren Dispose-Schritt gerade den Request freigibt
   ([Patch](artifacts/windows-request-filter-handoff/windows-request-filter-r2.patch#L213)).
   `quarantined` unterdrückt lediglich weitere Events; der Code implementiert
   damit keine allgemeine Sperre aller späteren View-Operationen.

4. **P2 – Fehlerpfad und Abnahme-Test bleiben unvollständig.**
   Scheitert die Vorbereitung der gecachten 403, wird nur geloggt und die
   Installation trotzdem als erfolgreich gemeldet (Patch Zeilen 234–241).
   Schlägt auch `GetDeferral` fehl, wird nur für Dokumente `Stop()` versucht;
   diese Injektion fehlt im finalen Lauf (Zeilen 200–211).
   Bei fehlgeschlagener Speicherung wird der COM-Deferral absichtlich per
   `Detach()` ohne verwalteten Besitzer zurückgelassen (Zeile 207).
   Der Test prüft vor Dispose auf Stille, protokolliert danach aber nur und
   liefert Exit 0 (Zeilen 1354–1402). Dieser grüne Exit ist kein Abnahme-Gate.

## Verifizierte Nachweise und Grenzen

- Größen und SHA-256 von GZIP, entpacktem Patch und ZIP stimmen; alle 36
  Manifest-Einträge stimmen. Das Archiv enthält zusätzlich das Manifest,
  insgesamt 37 UTF-8-Textdateien. Keine mitgelieferten Skripte ausgeführt.
- Rohlogs bestätigen Integration 217 Assertions, Absence 46, nicht-GUI-CTest
  8/8 und den erwarteten Fehler der Refusal-Mutation. Die vier normalen
  Antwort-Fallback-Szenarien zeigen keine Zielrequests/-bytes.
- Rot-Regressionslogs belegen den ursprünglichen Konstruktionserror-Leak und
  die durch den Opener-Retain offengehaltene Popup-Entscheidung.
- Backend-Diff und fokussierte Test-/Diagnosestellen geprüft; keine vollständige
  unabhängige Prüfung aller 975 neuen Testzeilen und kein eigener Windows-Lauf.
- TLS, Managed-Integration, echte ältere Runtime, Deferral-Fehler- und
  Popup-Allokationsfehlerinjektion bleiben offen. Der Build ist erfolgreich,
  aber nicht warnungsfrei (`STL4029`).

## Konsequenz

Windows bleibt im Parent beim Unsupported-Adapter (`None`). Der R2-Patch ist
ein gesicherter Untersuchungsstand, keine freigegebene Implementierung.
Eine sichere Fortsetzung benötigt einen belegten Fehler-/Abbruchpfad auch über
Dispose hinweg; eine Wartezeit oder bloße Dokumentation des Durchlasses
erfüllt den bestehenden Vertrag nicht.
