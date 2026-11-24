---
title: "Request-Filter für isolierte Views – Umsetzungsplan"
status: blocked
created: 2026-11-24
summary: "Teil 1: deklarative Request-Sperren als zusätzliche Schutzschicht für CodeAlta; dokumentierte Engine-Abdeckung statt Zero-TCP- und atomarer Update-Garantie."
---

# Request-Filter für isolierte Views

## Auftrag und Ausgangspunkt

Nur **Teil 1, §§1–9** des Entwurfs
`/Users/bwi/git/NeoAstra/.alta/plans/2026-11-24-request-filter-and-trusted-input-for-views.md`
umsetzen, mit dem hier präzisierten Sicherheits- und API-Vertrag. Grundlage
sind außerdem der untersuchte CodeAlta-Anwendungsfall und Issue #490 mit
seinen Verweisen auf #487 und #355.[^issue490][^issue487][^issue355]
Trusted Input, Data Roots, allgemeine Netzwerk-Interception und eine
Netzwerk-Sandbox sind nicht Teil dieses Plans.

Geprüfter Ausgangsstand: NeoAstra `1.14.0`, Commit `c6639dc`, nach Fast-forward
auf `origin/main`; CodeAlta-Quellcodeprüfung auf `64f5408b0`.
NeoAstra-Implementierung ausschließlich im aktuellen Worktree. Hauptcheckouts
und Ausgangsentwurf bleiben unverändert. Die CodeAlta-Abnahme in Abschnitt 4
ist eine getrennte Host-Integrationsaufgabe: dieser Plan autorisiert keine
Änderungen im CodeAlta-Hauptcheckout und keine Tests gegen laufende Nutzerdienste.

**In Review-Runde 2 freigegeben; macOS, natives iOS und Managed API implementiert und geprüft.**
Die übrigen Plattformschritte und die getrennte CodeAlta-Abnahme sind offen.
Die frühere Fassung wurde bis zu den Engine-Probes ausgeführt; ihre Messwerte
und die damalige strengere Bewertung bleiben in `doc/request-filter.md`
ausdrücklich als historische Ergebnisse erhalten.

### Entscheidung aus dem konkreten Anwendungsfall

Eine beliebige Webseite im integrierten Browser darf **keine CodeAlta-MCP-
Befehle ausführen**. Entwicklungsserver und die bewusst bereitgestellten
Projektseiten müssen dagegen weiter erreichbar sein. Reserviert werden die
MCP-Ports 2582/2583, der konfigurierte und tatsächliche Port sowie bekannte
Ports anderer CodeAlta-Instanzen – nicht jeder lokale oder von CodeAlta
geöffnete Port.[^codealta-browser]

Die durchgehende Sicherheitsgrenze ist der **Desktop-MCP-Server-Guard vor dem
MCP-Dispatcher**. Er prüft Fetch-Metadata sowie Host/Origin und gegebenenfalls
Bearer-Token. Es ist kein pauschales Verbot jedes `Origin`-Headers: ohne
Fetch-Metadata lässt der Kompatibilitätspfad auch bestimmte lokale Origins zu.
Deshalb gehört die Abdeckung tatsächlicher Browserrequests zur Host-Abnahme;
die bisher gelesenen HttpClient-Tests allein beweisen sie nicht.[^codealta-guard]

Der NeoAstra-Filter ist eine zusätzliche Schutzschicht. Diese Trennung ist für
Portänderungen wesentlich: andere Instanzen publizieren ihren Port erst nach
dem Listenerstart, und `McpPortRecord` cached die Liste bis zu eine Sekunde.
Selbst eine atomare Engine-Aktualisierung könnte diese vorgelagerte
Discovery-Verzögerung nicht beseitigen.[^codealta-ports]

Unverändert gelten die expliziten Isolationsanforderungen aus #490: eigene
Environment und getrennte Daten, keine normale Seiten-RPC-Bridge, keine
Anwendungs-Custom-Schemes in der fremden Umgebung, isolierte Host-Scripts und
hostseitige Entscheidungen über Berechtigungen, Downloads, Dateiauswahl und
Popups. Der Filter ersetzt nichts davon. Die Toolbar darf weiterhin keine von
der Seite synthetisch erzeugten Nutzerannotationen akzeptieren.[^issue490][^issue355]
Externe Browser bleiben gemäß #490 die Standardoption. Platzierung, Fokus,
Shortcuts und die bekannten nativen Überlagerungsgrenzen sind eigenständige
Aspekte des integrierten Browsers, keine zusätzlichen Filteraufgaben.

### Ausführungsstand, 24. November 2026

- Native macOS-Probe mit echten NeoAstra-Views, zwei Loopback-Listenern,
  TCP-/Byte-/Request-/Upgrade-Zählern und positivem Kontrollpfad erstellt.
  Alle 16 nativen Debug-CTest-Tests sind grün, die Baseline zusätzlich fünfmal
  hintereinander.
- Erweiterte Engine-Probes liefern erste Ergebnisse zu Subresources, Frames,
  Redirect, WebSocket, Dedicated Worker, Isolation und Add/Remove.
  Der Frame-Dokument-Fall ist wegen ausbleibendem load/error-Signal
  bisher inconclusive; mit geeigneter Request-Beobachtung nachprüfen.
- Der bisherige G4-Test liefert einen Gegenbeleg zu Zero-TCP: Beim ersten Hauptdokument akzeptiert der Ziel-
  Listener trotz vorher installierter Content Rule eine TCP-Verbindung,
  allerdings ohne HTTP-Request oder empfangene Bytes. Auch zusätzliches
  `Cancel` im nachweislich aufgerufenen nativen Navigation-Callback verhindert
  diese Verbindung nicht. Eine getestete Seitennavigation wird dagegen ohne
  Verbindung blockiert. Unter dem neuen Vertrag ist die leere Verbindung
  allein kein Fehlschlag; die Beobachtung wird nicht gelöscht.
- Die frühere G3-Barrierenforderung ist unbelegt: WebKit-Quellcodeprüfung findet getrennte, unbestätigte
  Updates an WebContent und Network. Kein öffentlicher Mechanismus für die
  globale Update-Barriere identifiziert; kein Timing-Leck reproduziert.
  Der neue Vertrag verspricht diese Barriere nicht.
- Die ersten `dotnet build -c Release` und `dotnet test -c Release` endeten
  vor Ausführung mit Exit 155 durch fehlendes SDK `10.0.114`. Dieser Blocker
  ist nach ausdrücklich freigegebener Installation behoben (siehe unten);
  fehlende Windows-/GTK-Laufzeitnachweise bleiben davon unabhängig.
- Schritt 3 abgeschlossen: Regelkern, Matcher und WebKit-Compiler mit
  browserfreien Tests und echten Host-Alias-Probes; `job-317c238c` bestand
  alle 17 damaligen nativen Tests und den erweiterten Gate-Lauf.
- Native ABI V2, Update-Queue und Cancellation/Disposal implementiert;
  ABI-1.7-Kompatibilität und deterministische Operationstests bestanden.
  macOS unterstützte in diesem ersten Stand die Basis als `Limited`; andere
  Backends blieben `None`. Die gezielte API-Prüfung `job-b86eb1ce` bestand.
  `job-84b3dd01` bestand anschließend alle 19 nativen Tests samt Popup-
  Isolation und den Gate-Lauf. `job-d5c87e8d` bestand vier ASan/UBSan-Prüfungen
  einschließlich der echten macOS-API; LeakSanitizer war gemäß Projektpraxis aus.
  Eine beim Gesamt-Test gefundene Regression in der Reihenfolge der
  Argumentvalidierung wurde im gemeinsamen Create-Entry-Point korrigiert und
  zusätzlich durch einen ungültigen V2-Filter abgesichert.
- SDK `10.0.114` nach ausdrücklicher Nutzerfreigabe unter
  `~/.dotnet/neoastra-sdk-10.0.114` installiert; offizielles Archiv vor
  Extraktion per SHA-512 geprüft. `global.json` und Systeminstallation
  unverändert. Generatorlauf `job-70a6b0ab` erfolgreich; Interop und
  `NeoAstra.Core.NativeSymbols.props` sind generiert, nicht handgeschrieben.
- Managed API vollständig angebunden: unveränderliche normalisierte Snapshots,
  Optionen/Getter/Setter, UI-Dispatch, FIFO, native-bestätigte Cancellation,
  Popup-Vererbung und Versionsschutz. Die frühere isolierte SDK-11-Prüfung des
  Schreib-Kinds ist durch Repository-Build und Tests mit dem gepinnten SDK ergänzt.
- macOS-Release-Runtime und iOS-Geräte-/Simulator-Archive bewusst neu gestagt.
  iOS lieferte bei diesem ersten Staging nur die ABI-Symbole und `None`;
  seine spätere Implementierung ist unten gesondert erfasst.
  `job-aad3febc` bestand alle 19 nativen Tests; macOS-Mindestziel bleibt 26.0
  bei SDK 27.2. Alle drei Manifest-Hashes stimmen mit den Artefakten überein.
- Endkontrolle `job-932ef304` mit dieser Runtime: Release-Build ohne Warnungen
  oder Fehler; 748 .NET-Tests, davon 666 bestanden und 82 übersprungen.
  Fokussierte Managed-Conformance: zwei Fälle mit neuer Native-Library
  (Lifecycle und Popup) sowie ein Fall mit alter Library (Fail-closed/Empty)
  bestanden. Logs: `artifacts/dotnet-tests-final.log` und Job-Ausgabe.
- Der breite Conformance-Lauf `job-78f31b43` endete mit Exit 137 nach einem
  bestehenden Mikrofontest; genaue Ursache ungeklärt, kein voller Conformance-
  Erfolg. Der separate Vorschlag `rx38` behandelt explizites Medien-Opt-in.
- Verbleibende Fortsetzungsgrenzen: VINBBWI verweigerte Agent-Sessions zunächst
  mit `machine.policyDenied` / `agent_sessions_off`.
  Besitzerfreigabe ist inzwischen bestätigt; isolierte Windows-Toolchain mit
  ausdrücklicher Installationsfreigabe eingerichtet. Standalone-Engine-Preflight
  bestanden; echte Windows-C-ABI-Integration wird im isolierten Kind umgesetzt.
  Kein verfügbarer GTK-Testhost nachgewiesen. Android ist inzwischen integriert
  und auf eigenen ARM64-Emulatoren geprüft (Nachweise unten).
  Keine Umgehung, keine Fremdcheckout-Änderungen.
  G5-Testpaket steht in `doc/request-filter.md`; CodeAlta-Abnahme ausstehend.
  Änderungen bleiben ohne Nutzerfreigabe uncommitted.
- Schritt 2 neu geprüft: Baseline und revidiertes `--gates` bestanden
  (`job-91173359`, Exit 0). Der Frame-Versuch wird jetzt nativ beobachtet;
  zusammen mit positiver Kontrolle und fünf Sekunden ohne Requestbytes
  trägt dies den Messbefund auch ohne `load/error`. TCP bleibt diagnostisch.
  Die ursprünglichen Ergebnisse stehen getrennt in `doc/request-filter.md`.
- UIKit-Fortsetzung: gemeinsamer Apple-Store-/Compile-/Installationscode statt
  Kopie der Cocoa-Implementierung; private Stores und native Queue unverändert.
  Eigene iOS-Engine-Gates `job-bc830aef` bestanden. Eine neue Regression
  (`job-193bb7fc`) zeigte Ersatznavigation bei einer zurückgestellten Popup-
  Entscheidung nach Filteränderung; UIKit prüft jetzt vor dem Fallback den
  aktuellen lokalen Snapshot.
- Vollständiger iOS-Lauf `job-f895d81b` nach Korrektur: Baseline, Gates und
  öffentliche API bestanden auf iOS 27.2 (24B8117), WebKit 8625.2.8.10.7,
  ARM64-Simulator. Initialfilter, Ersetzung, Clear, Cancellation, Isolation,
  Disposal/Store-Cleanup und Popup-Fallback geprüft. `Limited` mit Basis67,
  keine Zusatzflags; tracked Popups bleiben unsupported. Kein Geräte- oder
  iOS-Managed-Host-Nachweis und keine Prüfung älterer iOS-Versionen.
- Apple-Restaging `job-65d11979` bestanden: macOS Release19/19, iOS-Geräte- und
  Simulatorarchive neu gebaut/exportgeprüft; alle drei Manifesthashes korrekt,
  macOS-Mindestziel26.0 unverändert. Gezielte ASan/UBSan-Prüfung
  `job-181247de` 4/4 bestanden; drei Simulatorrunner-Unit-Tests bestanden.
- Abschließende Managed-Regression nach Apple-Restaging (`job-825c906c`):
  Release-Build ohne Warnungen/Fehler; erneut 666 Tests bestanden, 82
  übersprungen sowie fokussierte Conformance mit neuer/alter Native grün.
  Log `artifacts/dotnet-tests-apple-final.log`.
- Android-C-ABI/JNI/WebHost-Integration auf API36/WebView133.0.6943.137 und
  API37.0/WebView145.0.7632.218 bestanden. Testtransporte über eigene
  adb-reverse-Tunnel; frühere fehlerhafte Transportversuche gelten nicht als
  Blockiernachweis. Gleicher Environment-/Profilkontext, positive Kontrollen,
  Hostrequestzähler, Initial/Update/Clear/Cancel/Unsupported, Frames,
  Custom-Scheme und echte JNI-Disposal-Race geprüft. Nur Limited-Basis67;
  insbesondere keine Redirect-, Worker- oder WebSocket-Zusage.
- Android ARM64+x64 mit NDK29.0.14206865 gebaut/exportgeprüft und gestagt
  (`job-70000d3c`). x64 nur Crossbuild, keine Geräte-/Android-Managed-Host-
  Prüfung. Die erweiterte Matrix besteht inzwischen auf beiden APIs über
  HTTP und HTTPS mit testlokaler CA; öffentliche Origins bleiben ungeprüft.
- Android-Endkontrolle `job-80fb7cce`: Release-Build ohne Warnungen/Fehler;
  667 .NET-Tests bestanden, 82 übersprungen. Zehn Python-Runner-Tests bestanden,
  alle fünf gestagten Runtime-Manifeste mit korrektem SHA-256.
  Log `artifacts/dotnet-tests-android-final.log`.
- Erweiterte Android-Matrix `job-bc407caf`: API36/37 jeHTTP+HTTPS, insgesamt
  vierLäufe mitje55Hostzählererwartungen bestanden. EigeneCA nurProbeAPK,
  TLS-Handshake getrennt vonHTTPbytes. Ressourcen/Forms/Reload/Popup-
  DeferredUpdate zusätzlich; keineProduktänderung oder neueCapabilityflags.
- Windowszugriff bestätigt; fehlendeBuildvoraussetzungen mitNutzerfreigabe
  isoliert installiert undHashesgeprüft. clang-cl23.1.6/CMake4.5.1/Ninja1.13.2/
  Python3.15.2/.NET10.0.114/WebView2SDK1.0.4078.44. WebView2Runtime158.0.4370.33.
  Eigenständiger Engine-Preflight im Kind abgeschlossen: zweiter Lauf9PASS/
  0FAIL+4Diagnosen, gleicheEnvironment/Profil-Isolation, Dokument/Frames/
  Subresources/POST/Updates. NurHTTP/Fixturematcher, keineNeoAstra-Integration.
  Shared-/ServiceWorkerFetches bypass DOCUMENTscope; Zusatzflags bleibenunset.
  Abschlussmeldung war durchProviderquota verzögert, keinTesttimeout.
- macOS-ManagedHTTPS-Prüfung `job-7b7808c1` bestanden: Source-/Targetkontrollen,
  Filterisolation/Clear mitdekodiertenBytes; zweiTLS-Server-Tests bestanden.
  Ein Nullable-Warnhinweis danach korrigiert. Abschluss `job-56d2a284`:
  Release ohne Warnungen/Fehler, 669 Tests bestanden/82 übersprungen,
  fokussierte Conformance neue Native3/3 und alte Native1/1 bestanden.
  13 Python-Runner-Tests bestanden; Log `artifacts/request-filter-expanded-regression.log`.
- Erweiterte macOS-Managed-HTTPS-Frameprüfung `job-637cbcf4` bestanden;
  anschließende Regression `job-c1d23a85`: Release ohne Warnungen/Fehler,
  670 Tests bestanden und 82 übersprungen.
- Abschließender Windows-Kindbericht: C-ABI-Integration mit 217 Hostzähler-
  Assertions, Antwort-Fallbacks mit 18 Assertions, simulierte `_22`-Abwesenheit
  mit 46 Assertions und nicht-GUI-Suite 8/8 bestanden. Die Mutationskontrolle
  mit abgeschalteter Refusal-Entscheidung schlägt erwartungsgemäß fehl.
  Original- und R2-Patch bleiben unangewendet. R2-Patch und Logarchiv wurden
  im Parent empfangen; Datei-Hashes, alle 36 Manifest-Einträge und
  `git apply --check` sind geprüft. Der temporäre TLS-Empfänger ist geschlossen.
  Reviewkorrekturen an Popup-Lifetime, Runtime-Gating und Testfenstern sowie
  Cross-Origin-Frame-/Preflight-Tests sind laut Kind inzwischen umgesetzt.
  Die vorbereitete 403 fängt die geprüften Fehler der frischen Antwort ab.
  **Windows-Abnahme blockiert:** Scheitern beide Antwortzuweisungen dauerhaft,
  erreicht der gehaltene Request beim View-Abbau dennoch das Ziel.
  Die im Parent gelesenen Rohlogs bestätigen GETs auf `/b_s.js` und `/b_img.png` nach
  Disposal sowie Zeitlinien der Teardown-Diagnosen. Halten allein verhindert
  den Versand nur während der View-Lebensdauer. 600-ms-Abbruchversuche sind
  keine Lösung und wurden nicht in den Produktcode übernommen.
  Das aktuelle Backend friert die View laut Kind ein und meldet dem Host
  `WEB_PROCESS_TERMINATED`/`RECREATE_VIEW`; deren späterer Abbau bleibt unsicher.
  Die gezielte Parent-Codeprüfung findet außerdem den alten 204-Antwortpfad
  für bereits verweigerte Hauptnavigationen vor dem neuen Fallback: Fehler
  bleiben dort ungeprüft. Der grüne Fehler-Test prüft nur vor Dispose auf
  Stille und protokolliert den späteren Durchlass. Details sind in
  `artifacts/windows-request-filter-handoff/r2-review.md` festgehalten.
  TLS, Managed-Integration,
  echte alte Runtime, Deferral-Fehler- und Popup-Allokationsfehler-Injektion
  sind nicht belegt.
  Eine bestandene Diagnose des Fehlverhaltens gilt nicht als bestandene
  Sicherheits-Conformance. Keine weiteren Abbruch-Workarounds beauftragt.
  Die vom Nutzer angebotene NAS-Freigabe ist vom Mac im eigenen Übergabeordner
  schreib-/lesbar; Windows meldet den UNC-Pfad trotz erreichbarem SMB-Port
  als nicht gefunden. Die Ursache bleibt offen. Der kleine Common-Quellfix wurde
  unabhängig davon direkt übermittelt und laut Kind hashgeprüft übernommen.
  Kein Runtime-Staging
  und keine Windows-Capability-Freigabe durch diese Zwischenresultate.

## Arbeitsaufteilung für die Fortsetzung

Der Nutzer hat am 24. November 2026 eine vollständige, eigenständige
Windows-Arbeitskopie einschließlich Toolchain und Kontextübergabe beauftragt.
Für die einmalige Übergabe sind Commit und Branch-Push in den eigenen Fork
ausdrücklich erlaubt; dies ist keine Windows- oder Release-Freigabe.
Der Parent bleibt für macOS/Linux und die Integration verantwortlich.
Windows-spezifische Dateien werden dort nicht parallel bearbeitet;
notwendige Common-/Managed-Änderungen aus der separaten Windows-Kopie werden
bei der abschließenden Integration geprüft.

Das vollständige Dossier liegt unter
`.alta/handoffs/2026-11-24-windows-request-filter/README.md`, zusammen mit dem
unverändert archivierten R2-Kandidaten, Rohlogs und Parentreview.
Der Kandidat ist nicht auf den Parent-Quellstand angewendet.
Ein echtes Session-Fork/Move ist zielseitig nicht freigegeben; die Übergabe
erfolgt als neue Windows-Session mit Aufgabenkontext, nicht als Migration.
Die Einrichtung ist noch nicht abgeschlossen. Der Windows-Provider meldete
zwischenzeitlich sein Fünf-Stunden-Limit; kein Provider-/Account-Bypass.

Die frische lokale Übergabeprüfung `job-0545550b` bestand: Release ohne
Warnungen/Fehler, 670 Tests bestanden und 82 übersprungen. Der oben
dokumentierte Windows-Abnahmeblocker bleibt davon unabhängig bestehen.

## Ziel und Sicherheitsgrenze

Der Host kann vor der ersten Navigation und später im laufenden Betrieb
unveränderliche Sperrregeln für eine View setzen. Matching geschieht nativ,
ohne verwalteten Callback oder DNS-Abfrage im Request-Pfad. Innerhalb der
ausgewiesenen Engine-Abdeckung verweigert eine wirksame Regel den passenden
HTTP-Request bzw. WebSocket-Upgrade, bevor dessen Anwendungsrequest zum Ziel
gesendet wird. Eine zweite View darf nicht mitgefiltert werden.

**Nicht zugesagt:** das Ausbleiben jeder TCP-Verbindung, DNS-Abfrage oder
TLS-Aushandlung; Abwehr von Verbindungs-DoS; rückwirkendes Beenden laufender
Requests/WebSockets; eine atomare Umschaltung aller Engine-Prozesse oder eine
feste maximale Propagationsdauer nach einer Regeländerung.

Ein `Limited`-Backend mit belegter Grundfunktion ist ausdrücklich nutzbar.
Fehlende Request-Klassen werden über Flags und Dokumentation sichtbar.
CodeAlta entscheidet über zusätzliche Anforderungen und zeigt Einschränkungen
an; NeoAstra erzwingt nicht pauschal die maximal mögliche Abdeckung aller
Engines. Eine reine Navigationssperre wird aber nicht als Subresource-Filter
ausgegeben.

> [!WARNING]
> Eine erfolgreiche Filterinstallation ersetzt weder Authentisierung noch
> serverseitige Request-Prüfung. Insbesondere macht `await SetRequestFilterAsync`
> einen neu bekannt gewordenen Port nicht global und augenblicklich unerreichbar.
> Ein JavaScript-Fehler oder ein HTTP 403 beweist für sich allein auch nicht,
> dass der Filter den Request vor dem Ziel abgefangen hat.

## 1. Bestätigte Anknüpfungspunkte

| Bereich | Bestehender Code | Konsequenz |
| --- | --- | --- |
| Managed View-Erstellung | `src/NeoAstra.Core/NeoEnvironment.cs`, `CreateWebViewCoreAsync`, `NeoTransportViewInitializer.CreateAsync` | Filter nicht erst nach Rückgabe der nativen View installieren; insbesondere Popups können früher laden. |
| ABI | `native/include/neoastra.h`, `neoastra_view_options_t`; `native/src/common/neoastra.cpp`, `neoastra_environment_create_view_async` | Optionen sind derzeit Version 1; Größenprüfung muss für alte und neue Structs getrennt erfolgen. |
| Versionsschutz | `NeoEnvironment.CapabilityUnknownToLibrary`, `NeoRuntimeInfo` in `NeoTypes.cs` | Neue Exporte nur nach Build-Feature-Prüfung aufrufen. |
| Completion/Lifetime | `native/src/common/operation_completion.hpp`; `Interop/NativeOperation.cs` | Vorhandene Ownership und Exactly-once-Completion nutzen; lokale Installation und Cancellation eindeutig ordnen, ohne daraus eine Netzwerk-Barriere abzuleiten. |
| Windows | `windows_backend.cpp`, `register_view_events`, `WebResourceRequested`, `FrameNavigationStarting` | Bisher allgemeiner Filter nur für Dokumente, außerdem Custom Schemes; Frames werden nicht vollständig an den Host gemeldet. |
| Apple | `cocoa_backend.mm`, `uikit_backend.mm`, View-Erstellung und Popup-Delegates | Eigene Filter pro Content Controller; Popup-Konfiguration und asynchrone Vorbereitung ausdrücklich berücksichtigen. |
| GTK | `gtk_backend.cpp`, eigener `WebKitUserContentManager` pro View | Content-Filter statt neuer Web-Process-Erweiterung. |
| Android | `NeoAstraWebHost.java`, `Client.shouldInterceptRequest` und `shouldOverrideUrlLoading` | Request-Prüfung vor dem vorhandenen Early Return für nicht projizierte URLs; keine UI-Objekte im Request-Thread verwenden. |
| Tests/Generator | `native/tests`, `src/NeoAstra.Tests`, `src/NeoAstra.CodeGen/Program.cs`, `CMakeLists.txt` | Bestehende ABI-, Completion-, Loopback- und Engine-Testmuster erweitern; generierte Dateien nicht von Hand ändern. |

Wichtige Präzisierungen gegenüber dem Ausgangsentwurf:

- Das vorhandene WebView2-SDK ist bereits auf `1.0.4078.44` festgelegt
  (`CMakeLists.txt:15`). Die `_22`-Schnittstelle wurde laut Microsoft mit
  `1.0.2365.46` eingeführt. Kein pauschales SDK-Update einplanen; entscheidend
  bleibt die tatsächlich verfügbare Runtime-Schnittstelle.[^webview2]
- Service-/Shared-Worker-Events sind bei WebView2 **profilbezogen**, nicht
  eindeutig einer View zugeordnet. Mehrere passende Views desselben Profils
  können dasselbe Event erhalten. Eine gemeinsame Umgebung kann wiederum
  mehrere getrennte Profile enthalten. `SOURCE_KINDS_ALL` allein beweist
  deshalb keine per-View-Isolation.[^webview2]
- Apples Kompilierung hat einen Completion-Handler; `addContentRuleList:`
  und `removeContentRuleList:` dagegen nicht. Der Rücksprung belegt die
  Installation am Controller, nicht die Übernahme in allen Prozessen.[^apple]
- Android dokumentiert nur den ersten Request einer Redirect-Kette und
  Ausführung des Interception-Hooks außerhalb des UI-Threads.[^android]
- GTK stellt asynchrones Speichern/Kompilieren und separat das Hinzufügen
  zum Content Manager bereit. Diese Schritte bestimmen den lokalen
  Abschluss; die Engine-Propagation wird separat untersucht.[^gtk]

## 2. Festgelegter Vertrag für die Umsetzung

### 2.1 Regeln und Scope

- Refuse-only; Regeln werden ODER-verknüpft, Kriterien innerhalb einer Regel
  UND-verknüpft. Keine Prioritäten, Allow-Overrides, Skript-Hooks oder DNS-Auflösung.
- `HostMatch.Any`, `Exact`, `Suffix`, `Loopback`.
  `Suffix("example.com")` umfasst den Host selbst und Subdomains mit Punktgrenze,
  nicht `notexample.com`. Suffix-Matching nur für DNS-Namen, nicht IP-Literale.
- `Host` ist bei `Exact`/`Suffix` erforderlich, sonst nicht zulässig.
  Managed normalisiert DNS-Namen nach IDNA/ASCII, Kleinschreibung und
  einheitlicher Behandlung des abschließenden DNS-Punkts.
  Der C-Vertrag verlangt kanonische ASCII-Hosts; ungültige bzw. nicht
  kanonisierte Eingaben werden abgelehnt, nicht verschieden interpretiert.
- Port: `null`/nativ `-1` bedeutet beliebig; sonst `1..65535`.
  Effektive Defaults: HTTP/WS `80`, HTTPS/WSS `443`. Explizite und implizite
  Default-Ports müssen gleich behandelt werden.
- Schemes: HTTP, HTTPS, WS, WSS als Flags. Kein generischer Filter für `file:`,
  `data:`, `blob:`, Custom Schemes oder WebRTC/WebTransport.
  Standard einer Regel ist **HTTP | HTTPS**. WS/WSS können bei ausgewiesener
  Unterstützung ergänzt werden; explizit verlangte, nicht unterstützte Schemes
  werden abgelehnt. Netzwerkzugriffe aus Dokumenten anderer Schemes bleiben
  im angegebenen HTTP-/WebSocket-Scope.
- `Loopback` umfasst die im Ausgangsentwurf genannten IPv4-/IPv6- und
  Localhost-Formen nach URL-Kanonisierung, einschließlich IPv4-mapped IPv6.
  Keine Behauptung über DNS-Aliase oder Rebinding. Für reservierte Ports:
  `HostMatch.Any` verwenden.
- Höchstens 256 Regeln; höchstens 253 ASCII-Zeichen je Host; null-Einträge,
  unbekannte Enum-/Flag-Bits, leere Scheme-/Kind-Masken und NUL/Steuerzeichen
  ablehnen. Regeln und Listen bei Eintritt kopieren, niemals mutable
  Caller-Collections als installierten Zustand halten.
- `Kinds.All` umfasst auch engine-spezifisch unbekannte Request-Typen, soweit
  der jeweilige Interception-Pfad sie erfasst. Präzise Kind-Auswahl nur bei
  nachgewiesener, verlustfreier Abbildung; sonst entsprechende Regel ablehnen,
  nicht still verbreitern oder auf `All` reduzieren.

### 2.2 Managed API und bewusste Ergänzungen

Neue öffentliche Typen in `src/NeoAstra.Core/NeoRequestFilter.cs`:
`NeoRequestFilter`, `NeoRequestRule`, `NeoHostMatch`, `NeoRequestSchemes`,
`NeoResourceKinds`, `NeoRequestFilterFlags`.

Anknüpfung an den Entwurf:

- `NeoAstraOptions.RequestFilter`.
- `NeoAstra.SetRequestFilterAsync(filter, cancellationToken)` und
  `NeoAstra.RequestFilter` als letzter erfolgreich **nativ installierter**,
  unveränderlicher Snapshot; ohne Filter `NeoRequestFilter.Empty`.
  Der Getter behauptet keine Bestätigung aus jedem Engine-Prozess.
- `NeoCapability.RequestFilter`, Runtime-Build-Feature und Versionsschutz.
- XML-Dokumentation einschließlich Argument-, Unsupported-, Disposal-,
  Cancellation- und nativer Fehlerfälle.

`NeoRequestFilter.RequiredFlags` bleibt für ausdrücklich verlangte
Zusatzabdeckung erhalten. Der Standard ist nur die belegte Basis:
**`Subresources | Frames | PerView`**, einschließlich HTTP(S)-Hauptdokumenten.
WebSockets, Redirects und Worker sind keine pauschalen Standardvoraussetzungen.
Fordert der Host entsprechende Flags ausdrücklich, führen fehlende Flags
vor Installation zu `NotSupportedException`; der bisherige Filter bleibt
erhalten. Das ersetzt keine Scheme-, Kind- oder syntaktische Validierung.

`DedicatedWorkers` ergänzt die vorgeschlagene Flags-Liste, damit diese im
Entwurf geforderte Klasse nicht in einer unklaren Subresources-Zusage verschwindet.
Bestehende vorgeschlagene Bitwerte bleiben erhalten, neues Bit danach.
Die Basis ist nicht abwählbar. Initiale Installation und geordnete lokale
Ersetzung gehören zum API-Vertrag, nicht zu einer globalen Netzwerk-Barriere.
Keine neue per-Profil-Filter-API und keine heimliche Änderung gemeinsam
genutzter Profile.

`None` bedeutet fehlende Basis; `Limited` bedeutet belegte Basis mit fehlenden
Zusatzklassen oder Regelkriterien. Den bestehenden Wert `Native` nur bei
nachgewiesener vollständiger Abdeckung des hier definierten Filtervertrags
verwenden. Auch `Native` verspricht
keine Zero-TCP- oder atomare Update-Garantie. Die Maske entscheidet über eine
konkrete Zusatzanforderung; optionale Refusal-Telemetrie ist dafür nicht nötig.

`ReportsRefusals` bleibt reserviert und unset. Die im Entwurf ausdrücklich
optionale Beobachtung samt `RequestRefused`-Event wird nicht implementiert:
kein Event-ABI-Ausbau und kein Callback auf dem kritischen Pfad nur für Telemetrie.

Leere Liste bedeutet ausschließlich „Filter entfernen“. Ohne Build-Feature
sind `null`/leer beim Erstellen kompatible No-ops; leeres Setzen auf einer alten
Library ebenfalls ohne neuen Export. Eine nicht leere Liste darf dort nie
als erfolgreicher No-op behandelt werden.

### 2.3 Aktivierung, Fehler und Lebensdauer

1. Validieren und Snapshot erstellen; Basis und angeforderte Zusatzabdeckung prüfen.
2. Für Updates pro View FIFO serialisieren. Parallel kompilierte Ergebnisse
   dürfen niemals eine neuere Generation überschreiben; zunächst keine
   parallele Kompilierung oder globaler Cache.
3. Alten installierten Filter während der Vorbereitung behalten.
4. Neue Regeln kompilieren und über die öffentlichen Engine-APIs installieren.
   Bei Content Lists neue Liste vor Entfernung der alten hinzufügen; bei
   synchronen Matchern den besitzenden Snapshot sicher ersetzen. Anschließend
   den nativ installierten Snapshot publizieren und die Operation abschließen.
   **Erfolg bedeutet:** Vorbereitung abgeschlossen und die lokalen
   Installations-/Entfernungsaufrufe ohne bekannten Fehler durchgeführt.
   Die Engine kann ihre Prozesskopien noch aktualisieren; in diesem Übergang
   können alte, neue oder vorübergehend beide Sperrlisten wirksam sein.
   Keine feste Wartezeit oder beliebige JS-Rundreise als Barriere ausgeben.
5. Validierungs-/Kompilier-/Installationsfehler erhalten den alten Zustand.
   Initialfehler zerstören die noch unveröffentlichte View; keine Navigation.
   Ist nach einem Engine-Fehler ein sicherer Zustand nicht wiederherstellbar,
   View schließen statt Erfolg oder ungefilterten Weiterbetrieb melden.
6. Cancellation vor dem lokalen Installationspunkt verhindert die Installation;
   danach gewinnt deren erfolgreicher Abschluss. Ein abgebrochenes Task darf
   nicht später unbeobachtet eine weitere Liste installieren. Dafür einen eng
   begrenzten Modus für native-bestätigte Cancellation im bestehenden
   `NativeOperation` prüfen; andere Operationen nicht umstellen. Dies ordnet
   die lokalen Aktionen, nicht die Propagation im Browser.
7. Disposal/Shutdown beenden ausstehende Operationen genau einmal.
   Späte Engine-Completions dürfen keine zerstörte View benutzen, keine Regeln
   nachinstallieren und keinen alten Snapshot veröffentlichen.

Bei Erstellung Filter vorbereiten und an der View-Konfiguration registrieren,
**bevor** die View für eine erste explizite Navigation oder als Popup-Ziel
freigegeben wird. Nicht erst nach Rückgabe von `CreateWebViewAsync` nachrüsten.
Ein Kompilierfehler darf kein ungefiltertes Ersatzfenster erzeugen.

Laufzeitwechsel müssen im Test auch in bestehenden Dokumenten wirksam werden;
ein „nur kompiliert, nie installiert“ ist kein Erfolg. Es gibt aber keinen
zugesagten globalen Cut-over-Zeitpunkt und keine zugesagte maximale Verzögerung.
Nach `Empty` ist die Liste lokal entfernt, einzelne Prozesse können während
der Propagation noch alte Sperren anwenden; keine zeitliche Obergrenze behaupten.
In-flight Requests und bestehende WebSockets werden nicht nachträglich beendet.
Hosts mit einer harten sofortigen Sperranforderung müssen diese unabhängig
vom Filter durchsetzen. Für CodeAlta übernimmt dies der Server-Guard.

Tracked Popups erben standardmäßig den installierten Snapshot des Openers vor
ihrer ersten Navigation; anschließend sind sie unabhängige Views. Ein eigener
Filter wird ebenfalls vor Freigabe des Popup-Ziels installiert. Beim synchronen
Apple-Popup-Pfad bereits kompilierte Regeln nutzen, nicht die UI blockieren. Falls die
Engine keinen sicheren unabhängigen Controller zulässt, betreffendes Popup
ablehnen. Ein gesperrtes `window.open` darf keine Ersatznavigation auslösen.

## 3. Technischer Zuschnitt

### Gemeinsamer nativer Kern und ABI

- `native/src/common/request_filter.hpp/.cpp`: besitzender validierter
  Regelsnapshot, URL-Zielzerlegung für die vier unterstützten Schemes,
  Matching und WebKit-JSON-Compiler; kein zweiter Matcher in Java/C#.
- Engine-gelieferte kanonische URLs als Eingang nutzen und deren Eigenschaften
  testen. Keinen vollständigen WHATWG-Parser aus dem Gedächtnis nachbauen.
  Zusätzliche Normalisierung nur in diesem gemeinsamen Kern und anhand
  gemeinsamer Testvektoren. Ein nicht sicher parsebares HTTP(S)/WS(S)-Ziel
  bei aktivem Filter darf nicht durch einen „kein Match“-Fallback passieren.
- JSON strukturiert und korrekt escaped erzeugen. Restricted Regex:
  verankerte Scheme-/Authority-Grenzen, Alternativen in getrennten Regeln,
  kein unsupported `|`/`{n}`. `if-domain` nicht als Zielhost-Filter missbrauchen.
  Default-Ports, Userinfo, Query-Hostattrappen, IPv6-Hexformen und Trailing-Dots
  durch dieselben Vektoren wie den Matcher absichern.[^webkit-format]
- Kompilierte Ausgabe auf 16.384 Teilregeln und 4 MiB UTF-8 begrenzen;
  Überschreitung vor Engine-Aufruf mit klarem Fehler melden. Grenztests
  verhindern unkontrollierte Expansion trotz 256 zulässiger Eingaberegeln.
- `neoastra_request_rule_t` wie vorgeschlagen mit Size/Version, Host-Modus,
  Host, Port, Schemes und Kinds. Öffentliche C-Dokumentation zu Ownership,
  ASCII-Hosts, Threading, Callback und Rückgabefehlern ergänzen.
- Neue `neoastra_request_filter_t` mit Size/Version, Regelzeiger/-zahl und
  `required_flags`, damit C-Aufrufer dieselbe Fail-closed-Semantik erhalten.
  Die Basis gilt auch bei `required_flags == 0` implizit; weitere gesetzte Bits
  verlangen Zusatzabdeckung. `neoastra_view_set_request_filter_async` nimmt
  diese Beschreibung entgegen.
- View-Optionen Version 2 um einen Zeiger auf diese Filterbeschreibung ergänzen;
  tief kopieren, bevor der Entry Point zurückkehrt. Version 1 weiterhin annehmen.
  Dies konkretisiert/ersetzt die losen `request_rules`-/`request_rule_count`-
  Felder des Entwurfs und hält die Filterdarstellung an beiden Eingängen gleich.
- Fähigkeit nach `SCRIPT_WORLD_CHANNEL`, Build-Feature Bit 6 nur sofern beim
  Implementierungsstart noch frei; bestehende Werte niemals umnummerieren.
  Feature bedeutet verfügbare ABI, nicht volle Backend-Abdeckung.
- Alte Größe als eingefrorene Prefix-Grenze prüfen; keine neuen Felder lesen,
  bevor Version und Size es erlauben. Neue Managed-Library sendet bei alter
  Native-Library weiterhin die echte Version-1-Größe.
- Generator erzeugt Interop und `NeoAstra.Core.NativeSymbols.props`.
  ABI-Layouttests erweitern, eingefrorenen ABI-1.7-Consumer nicht modernisieren.

### Backend-Strategie

| Backend | Implementierung | Freigabebedingung / bewusste Grenze |
| --- | --- | --- |
| macOS, anschließend iOS | Gemeinsames JSON; `WKContentRuleListStore`, eigener Content Controller, je View installierter Filter und Generation | Initiale Installation, spätere Wirksamkeit und Request-Abdeckung testen; keine globale Update-Barriere versprechen. Keine Übertragung eines macOS-Testresultats auf iOS. |
| Windows | Vor erster Navigation breiter Request-Filter `"*"` für die unterstützten Source Kinds; präzises Matching im nativen Handler. Zusätzliche Checks bei Haupt-/Frame-Navigation und New Window. | `_22` zur Laufzeit abfragen. Ältere Runtime nur mit separat bewiesener Teilabdeckung. Lokal erzeugtes HTTP 403 ohne Body ist zulässig; der Testserver darf den verweigerten Anwendungsrequest nicht empfangen. |
| Linux | Gleiches JSON über `WebKitUserContentFilterStore`, eigener Content Manager | Eigene Nachweise für Installation, Wirksamkeit und Abdeckung; keine Gleichsetzung mit macOS. Temporären privaten Store erst nach Abschluss ausstehender Compiles bereinigen. |
| Android | JNI-Prüfung vor normaler Resource-Behandlung; nativer unveränderlicher Snapshot im Request-Thread; zusätzliche Navigationschecks | Keine UI-Thread-Marshal-Roundtrips. Redirects zunächst unset; WebSockets, Worker und Kinds nur mit Nachweis. Kein globaler `ServiceWorkerClient` als Ersatz für per-View-Filter. |

Auf Windows nicht blind `SOURCE_KINDS_ALL` installieren: zunächst dokument-
bezogene Requests einschließlich separat geprüfter Dedicated Worker behandeln.
Service/Shared Worker bleiben unset, solange eindeutige Zuordnung und Isolation
im selben Profil nicht nachgewiesen sind. Tests müssen gerade diesen Fall
abbilden, nicht nur zwei isolierte Umgebungen.

Keine CDP-Fallback-Schicht in dieser Umsetzung. Fehlen Redirect-/WebSocket-
oder Worker-Klassen, bleiben deren Flags unset; nur ein Aufruf, der sie
ausdrücklich verlangt, scheitert daran. Fehlt die notwendige Frame-Basis,
bleibt das Backend `None`. Ein CDP-Fallback mit Target-Attach-/Crash-/Lifetime-
Management braucht einen eigenen freizugebenden Entwurf.

Für Apple/GTK kompilierte Listen nur so lange aufbewahren, wie benötigt.
Eigene zufällige Store-/Listen-IDs verhindern View- und Prozess-Kollisionen;
keine Roh-URLs oder Hostnamen als Dateinamen. Private Stores temporär halten,
Cleanup nach Abschluss/Abbruch ausstehender Compiles testen. Keine fremden
Content-Listen oder User Scripts durch `removeAll...` entfernen.

## 4. Prüfungen und Freigabegrenzen

G1–G4 prüfen NeoAstra. G5 prüft die Sicherheitsgrenze der CodeAlta-Integration.
Ihre Ergebnisse werden getrennt dokumentiert; weder ein Filtertest noch ein
MCP-Guard-Test ersetzt den jeweils anderen.

**G1 – Scope und Isolation:** Dokument, Subresource, same-/cross-origin Frame,
Dedicated-/Shared-/Service-Worker und Popup getrennt prüfen. Gegenprobe mit
ungefilterter View im selben Environment und Profil; zusätzlich getrennte Profile.
Kein Worker-Flag aus bloßer Beobachtung eines Events ableiten.

**G2 – URL und Redirect:** Native Matcher und WebKit-Compiler mit identischen
Vektoren vergleichen. Jede Redirect-Stufe einschließlich Schemawechsel prüfen;
keine Aussage aus Navigation allein auf `<img>`/fetch/Worker übertragen.

**G3 – Initialisierung, lokale Installation und Propagation:** Registrierung
vor der ersten Navigation einschließlich Popup nachweisen. Auf bereits aktiven
Seiten mit Timern/Frames Hinzufügen, Ersetzen und Entfernen untersuchen.
Lokale Aufrufreihenfolge, Snapshot/Task-Zustand und tatsächliche spätere
Filterwirkung separat prüfen. Im Test dürfen Proberequests und eine Deadline
die Beobachtung begrenzen; sie werden weder zum Produktions-Wartealgorithmus
noch zu einer garantierten Verzögerung. Einzelne erfolgreich blockierte
Requests beweisen keine globale Prozessbarriere. Bleibt eine Änderung im
isolierten Test unwirksam, ist das ein Fehler, kein zulässiger No-op.

**G4 – Anwendungsrequests statt TCP als Abnahmekriterium:** TCP-Accepts,
Transportbytes, HTTP-Request-Bytes, geparste HTTP-Requests und WebSocket-
Upgrades getrennt erfassen. Bei HTTPS HTTP nach TLS-Terminierung zählen.
Eine leere Verbindung oder TLS-Aushandlung allein lässt den Filtertest nicht
scheitern; Header-/Body-Daten eines passenden Requests am Ziel innerhalb einer
zugesagten, wirksamen Klasse dagegen schon. Nicht erst einen vollständig
geparsten Request als Leck zählen. Messungen während einer laufenden Änderung
separat ausweisen, nicht mit dem installierten Anfangszustand gleichsetzen.
Preconnect/DNS-Verkehr bleibt diagnostisch, ohne Firewall-Zusage.

**G5 – CodeAlta-Server-Guard mit echten Browsern:** Frühzeitig parallel zu den
Backend-Prüfungen als getrennte Host-Abnahme einplanen. In einem ausdrücklich
freigegebenen CodeAlta-Testworktree eine wegwerfbare MCP-Instanz
mit dem **echten `DesktopMcpGuard` und der echten Middleware-Reihenfolge**
verwenden. Nur Fake-Tools mit Aufrufzählern, zufällige Ports und Testdaten;
keine produktiven Dienste, Credentials oder Nutzerprofile. Den Guard nicht
als vereinfachte Kopie in NeoAstra nachbauen und keine neue Produktabhängigkeit
von CodeAlta einführen.

- Zuerst **ohne Browserfilter** prüfen: fremde Seite, lokale Entwicklungsseite
  auf anderem Port, same-origin-Testfixture, opake Origin, Frame, fetch/XHR,
  einfache Form-/no-cors-POSTs, JSON-POST mit Preflight, WebSocket-Upgrade und
  verfügbare Worker. Tatsächlich gesendete Header protokollieren; fehlende
  Fetch-Metadata nicht per Annahme ergänzen.
- Den echten MCP-Handlerpfad prüfen: Kein unautorisierter Tool-Aufruf darf
  stattfinden. Ein normaler Testprogramm-Client muss den Fake-Tool-Zähler
  dagegen erhöhen können. Ankommende/abgewiesene Requests, Guard-Entscheidung
  und Tool-Aufrufe getrennt zählen. Ein Browser, der den Versuch schon selbst
  blockiert, ist kein Nachweis dafür, dass der Server-Guard ausgeübt wurde.
- Mit bekanntem Portfilter wiederholen. Zusätzlich freien neuen MCP-Port erst
  während laufender Seiten starten und vor Kenntnis des Filters ansprechen:
  der Server-Guard muss vom Listenerstart an schützen. Cache-Verzögerung und
  Regel-Propagation dürfen diesen Test nicht zu einem MCP-Aufruf führen.
- Bestehende `DesktopMcpTests` mit künstlichen HttpClient-Headern behalten;
  diese echten Engine-Tests ergänzen sie. Tests mit fehlender Fetch-Metadata
  müssen den Kompatibilitätspfad ausdrücklich bewerten, nicht nur `Origin`
  pauschal als ausreichenden Schutz ansehen.

**Stopp-/Fortsetzungsregeln:**

- Defekte Validierung/Ownership, wirkungsloser Matcher, fremdgefilterte View,
  fehlende HTTP(S)-/Frame-Basis oder ungefilterte Erstellung nach einem bekannten
  Installationsfehler blockieren das betroffene Backend.
- Fehlende Zusatzklassen führen zu `Limited`; allein fehlende Zero-TCP- oder
  globale Update-Barrieren sind **keine** Blocker des neuen Vertrags.
- Fehlender Windows-/Linux-/Mobil-Testhost blockiert dessen Zusage, nicht die
  weitere Implementierung und Prüfung eines verfügbaren Backends. Ungetestete
  Backends bekommen keine erfundenen Flags; gegebenenfalls `None` mit Grund.
- Ein unautorisierter Fake-MCP-Tool-Aufruf in G5 blockiert den CodeAlta-Rollout
  in der betreffenden Konstellation. Dann den Host-Guard separat beheben oder
  diesen Einsatz begrenzen; ein verzögert aktualisierter Filter ist kein Ersatz.
- NeoAstra-Implementierung und CodeAlta-Rollout getrennt bilanzieren. Ohne G5
  keine neue Behauptung lückenloser CodeAlta-Sicherheit. Der externe Nachweis
  autorisiert weder Änderungen an einem fremden Hauptcheckout noch stilles
  Erweitern dieses Plans um eine Server-Sicherheitsüberarbeitung.

## 5. Ausführungscheckliste

Jeder Schritt umfasst zugehörige Tests und einen fokussierten Diff-Review.
Bei Ausführung Planstatus `in-progress` setzen; bei Gates Blocker dokumentieren.
Commits nur gemäß dann geltender Nutzerfreigabe, sonst uncommitted zur Review.

- [x] **1 — Baseline und Probe-Harness.** Git-/ABI-/SDK-Stand erneut prüfen;
  bestehende `NavigationRequestTests` und Loopback-Helfer wiederverwenden oder
  eng erweitern. Zwei Ursprünge, frische Listener, Accept-/Byte-Zähler,
  Redirect-/WebSocket-Endpunkte und klare Deadlines aufbauen.
  Engine-/OS-Version pro Lauf erfassen. Ungefilterte Positivkontrollen zuerst.
- [x] **2 — Probes auf den revidierten Vertrag ausrichten.** Bestehende Messwerte
  behalten; im Harness TCP und Anwendungsrequests getrennt bewerten. Den
  Frame-Fall mit beobachtetem Navigationsversuch, positiven Kontrollen und
  begrenztem Beobachtungsfenster prüfen statt `load/error` vorauszusetzen.
  Einen Timeout ohne weiteren Nachweis nicht als Blockiererfolg zählen.
  `doc/request-filter.md` auf den neuen Vertrag umstellen, die alte Gate-
  Auswertung als solche kennzeichnen. Die vorhandenen Engine-Probes zuerst
  lokal wiederholen; G1–G4 im Zuge der Schritte 3–8 vervollständigen, sobald
  Matcher und API vorhanden sind. Weitere Plattformen vor deren Freigabe
  prüfen, nicht als Voraussetzung jeder lokalen Kernarbeit. G5-Testpaket früh
  für eine separat freizugebende CodeAlta-Testaufgabe vorbereiten.
- [x] **3 — Gemeinsamer Kern samt Tests.** Validierung, Host-/Port-/Scheme-
  Matching und JSON-Compiler implementieren; deterministische Vektoren,
  Gegenbeispiele und Expansion-Limits testen. Neues CMake-Testtarget
  `neoastra_request_filter_tests` anlegen und Compiler-Tests in dasselbe Target
  aufnehmen, sofern kein separates Fixture erforderlich ist.
- [x] **4 — Versionierte native ABI und Lebensdauer.** Neue Structs/Export/
  Capability und View-Optionen V2; V1-Kompatibilität, Kopieren der Eingaben,
  FIFO, Cancellation/lokaler Installationspunkt und Cleanup implementieren.
  `completion_test_backend.cpp` erweitern; Startfehler, späte Completion,
  Shutdown und Disposed-View durch browserfreie Tests abdecken.
- [x] **5 — Apple-Backends.** Gemeinsamen Compiler in Cocoa/UIKit einbinden;
  initiale Registrierung, geordnete lokale Ersetzung, unabhängige Popups und
  Store-Cleanup umsetzen. `macos_request_filter_tests.mm` sowie iOS-
  Engine-Testpfad anlegen; nur jeweils nachgewiesene Flags freigeben.
  Die asynchrone Prozess-Propagation ausdrücklich dokumentieren.
  **Nachweis:** macOS und UIKit mit gemeinsamem Apple-Installationscode und
  eigenen Engine-Prüfungen fertig. Shared-Probe `apple_request_filter_probe.mm`,
  iOS-Runner `eng/run_ios_request_filter_probe.py`; vollständiger Simulator-
  Lauf `job-f895d81b` bestanden. UIKit bietet weiterhin keine tracked Popups,
  verhindert aber gefilterte Ersatznavigation einschließlich deferred Decisions.
  Beide Apple-Backends melden nur belegte `Limited`-Basis; iOS-Gerätearchiv
  crossgebaut, kein physischer Geräte- oder iOS-Managed-Host-Test.
- [ ] **6 — Windows.** Filter-/Eventregistrierung, `_22`-Runtime-Gating,
  präzisen Matcher und Navigationschecks integrieren. Vorhandene Custom-
  Scheme-/Navigation-Logik weiter funktionsfähig halten.
  Reale WebView2-Tests ergänzen, nicht nur Callback-Mocks; Isolation und
  Request-Source-Scope besonders prüfen.
  **Teilstand:** Standalone-WebView2-Preflight mit9PASS/0FAIL abgeschlossen.
  Native-only-Quelloverlay des aktuellen Worktrees (Basis c6639dc) mitSHA256
  an Windowskind6d2e8be6-dafd-4306-9919-bbce4a25f77a übertragen; Implementierung
  und echteCABI-Tests nur in eigener neuer Sourcekopie. KeinCommit/Push oder
  ReleaseStaging ohneParentreview. Worktreebackend bleibt bisIntegration None.
- [ ] **7 — Linux und Android.** GTK-Compile/Install/Cleanup und JNI-Snapshot-
  Zugriff ergänzen. Android-Dispose-vs-Interception-Races testen;
  Shared-/Service-Worker nicht appweit beeinflussen.
  Java-Checks ergänzen, echte Browser-Tests separat ausführen.
  **Teilstand:** Android-Engine-Preflight mit je 24/24 strengen Prüfungen auf
  API36/WebView133 und API37/WebView145 bestanden. Subresource-Redirectziele
  umgehen den Hook nachweislich; Redirects bleiben unset. JNI-Snapshotprüfung,
  Java-Dispose-Gate und Navigationchecks implementiert und crossgebaut;
  JVM-Gatecheck und 78 fokussierte Java-/Managed-Tests bestanden.
  Echte NeoAstra-C-ABI/JNI/WebHost-Probes auf API36/WebView133 und
  API37.0/WebView145 bestanden (`job-0b7db1be`, `job-1b6b40a5`): Hostzähler,
  Positivkontrollen, Isolation, Update/Clear/Cancel/Unsupported, Custom Scheme
  und Disposal-Race. Android ARM64+x64 gebaut, exportgeprüft und gestagt
  (`job-70000d3c`); x64 nur Crossbuild. Erweiterung `job-bc407caf` besteht
  HTTP+HTTPS auf beiden APIs, je55Hostzähler: zusätzliche Ressourcen,
  FormGET/POST, Reload, Popup-/DeferredFallback. Kein öffentlicher Origin-,
  physischer Geräte- oder Android-Managed-Host-Nachweis.
  Android-Teil implementiert; GTK und breitere Matrix bleiben offen.
- [x] **8 — Managed API und Interop.** Generator ausführen; Rule-Typen,
  Snapshot, Basis-Default für `RequiredFlags`, HTTP(S)-Default der Schemes,
  Getter, Setter und Create-Option integrieren.
  Capability-/Build-Feature-Guard und eng begrenzte Cancellation-Anpassung
  testen. XML-Dokumentation muss Installation und globale Wirkung unterscheiden.
  `RequestFilterTests.cs`, `AbiLayoutTests.cs` und Versionsschutz-Tests ergänzen.
  Keine unbedingte Referenz auf neue Exporte bei alter Library.
  **Nachweis:** Generator, Release-Repository-Build/-Tests sowie fokussierte
  echte Managed-Conformance einschließlich alter Native-Library bestanden.
- [ ] **9 — End-to-End, Host-Abnahme und Dokumentation.** Nachstehende Abnahmematrix
  je zugesagtem Backend ausführen. `readme.md`, `doc/capabilities-and-security.md`,
  `doc/known-limitations.md`, `doc/platform-support.md`,
  `doc/neoastra_v2/09-advanced-platform-services.md` inhaltlich abgleichen;
  Details und Host-Beispiel in `doc/request-filter.md` zentral dokumentieren.
  G5-Testpaket mit konkreten Fällen an die getrennte CodeAlta-Testaufgabe
  übergeben; deren tatsächlichen Status erfassen. Ohne Testworktree/Freigabe
  ausdrücklich „CodeAlta-Abnahme ausstehend“, nicht „Sicherheitsnachweis erfüllt“.
  **Teilstand:** macOS-/UIKit-/Android-JNI-/Managed-Prüfung, Dokumentationsabgleich
  und konkretes G5-Testpaket in `doc/request-filter.md` vorhanden. Windows/GTK sowie
  tatsächliche CodeAlta-Abnahme bleiben offen.
- [ ] **10 — Vollprüfung und Übergabe.** Native Tests, Release-Build und
  Managed-Tests, bestehende Navigation/Custom-Scheme/Popup/Transport-Tests,
  ABI-Kompatibilität und betroffene Runtime-Artefakte prüfen.
  Capability-Matrix gegen Testbelege reviewen; fehlende Plattformnachweise und
  Host-Abnahme ausdrücklich ausweisen. Keine Gesamtfreigabe für CodeAlta bei
  offenem G5; Abschlusskriterien in Abschnitt 8 beachten.
  **Teilstand:** Native-/Managed-Release-Prüfung, ABI-Kompatibilität und Apple-/
  Android-Artefakte geprüft; Gesamt-Conformance nicht grün (Exit 137). Kein
  plattformübergreifender Abschluss und kein Commit.

## 6. Abnahmematrix

### Browserfreie Tests

- Regelgrenzen 0/1/256/257, Hostlänge, IPv4/IPv6, IDN, Trailing-Dot, Suffix-
  Grenzen; Port 0/-2/65536, Null-/Pointer-/Count-Inkonsistenzen, defekte UTF-8,
  unbekannte Versionen und Flag-Bits. Managed und native Fehler müssen
  denselben Vertrag durchsetzen.
- Scheme-/Port-Kombinationen, explizite/implizite Defaults, keine Matches
  durch Hosttext in Pfad, Query oder Userinfo; Hostnamen mit Regex-Metazeichen.
- Aliasvektoren aus dem Entwurf plus oktale IPv4, Prozentkodierung im Host,
  ausgeschriebene IPv6 und `::ffff:7f00:1`; auch ähnlich aussehende
  Nicht-Loopback-Ziele und DNS-Namen, deren Suffix nur textuell passt.
- Compiler-Goldenfiles und echte Engine-Kompilierung: korrektes JSON,
  erlaubte Regex-Syntax, Case-Semantik, Kind-Mapping und harte Limits.
  Ein Goldenfile ersetzt keinen Matching-Test im Engine-Parser.
- Caller mutiert Collection nach Create/Set: installierter Snapshot bleibt gleich.
  Gettersnapshot ist nicht als Array/List nachträglich beschreibbar.
- FIFO A/B/C, Fehler in B, abgebrochenes B, Cancellation-vs-Installation, Disposal
  während Compile, Callback vor Operation-Attach, doppelte/späte Completion.
  Kein Zustandssprung auf abgebrochene oder ältere Generationen.
- Neue Managed-/alte Native-Library, alter C-Consumer/neue Library, neue
  Structs mit zu kleiner Size, leere Filter ohne Feature; keine fehlenden
  Entry Points und keine Änderung eingefrorener Layout-Erwartungen.

### Reale Browser-Tests pro zugesagter Klasse

| Fall | Erwartung |
| --- | --- |
| Hauptdokument, Host-Navigation, Reload, Formular-GET/POST | Bei vorher installierter passender Regel erhält das Ziel keinen HTTP-Anwendungsrequest. TCP/TLS separat dokumentieren, nicht als Request-Leck zählen. |
| Bild, Script, CSS, Font, Medien, fetch, XHR, EventSource, Beacon/Ping, Preload/Prefetch | Kein passender Anwendungsrequest im ausgewiesenen Scope; ungefilterte Positivkontrolle muss tatsächlich senden können. |
| Same-origin, cross-origin, verschachtelte und – wo vorhanden – OOPIF-Frames | Frame-Dokument und Requests des Frames beide geschützt. |
| `window.open`, `target=_blank`, tracked Popup und `about:blank`-Popup mit späterem Request | Erster Request geschützt; eigener Controller; kein Escape durch Popup-Defaultpfad. |
| WS/WSS und HTTP-Upgrade | Kein HTTP-Upgrade-Request zum gesperrten Ziel; TLS-Aushandlung separat zählen. Nur bei bewiesenem WebSockets-Flag als unterstützt führen. |
| 301/302/303/307/308, mehrstufig und Schemewechsel | Jeder Hop geprüft, sowohl Dokument als auch Subresource und zugesagter Worker. |
| Dedicated-/Shared-/Service-Worker | Bootstrap und anschließende Requests getrennt; bestehende/neu registrierte Worker, zwei Views im selben Profil, auch nach Schließen der Ursprungsview. |
| Laufzeitwechsel und `Empty` | Nach `await` lokaler Snapshot korrekt installiert/entfernt; tatsächliche spätere Wirkung in aktiven Dokumenten nachweisen und Übergangsrequests separat protokollieren. Keine sofortige globale Sperre ableiten. |
| Zweite View im selben Environment/Profil | Ohne Filter unverändert; Filterwechsel der ersten View beeinflusst sie nicht. |
| Kompilierfehler/alte Engine/fehlende RequiredFlags | Eindeutiger Fehler; kein ungefiltertes Ersatzfenster, alter installierter Filter bleibt erhalten. |
| Aliasnamen und numerische Formen | Loopback-Regeln kanonisch richtig; port-only unabhängig vom Host; öffentliche DNS-Aliase nicht als Loopback-Schutz ausgeben. |
| CodeAlta-Host-Abnahme G5 | Kein unautorisierter Fake-MCP-Tool-Aufruf, auch ohne Filter bzw. vor Kenntnis eines neu gestarteten Ports; tatsächliche Browser-Header und Guard-Entscheidung nachweisen. |

Harness-Regeln: frische Origins/Ports bzw. eindeutig isolierte Testfälle gegen
Connection-Reuse; Caches, Service Worker und HTTP/2-Coalescing kontrollieren.
HTTPS/WSS mit lokalem Testzertifikat und ausschließlich testlokaler Freigabe,
keine global abgeschaltete TLS-Prüfung. Ports vermeiden, die der Browser selbst
verbietet; CORS, Mixed Content, CSP und Local-Network-Checks dürfen keine
falsch positiven Blocking-Erfolge erzeugen. Negative Tests benötigen
nachweislich ausgelöste Requests und erfolgreiche ungefilterte Gegenproben.
`127.0.0.1` und `localhost` sind unterschiedliche Origins, aber kein Ersatz
für eine echte öffentliche-zu-lokaler-Netzwerk-Konstellation.

Plattform-Skips nur mit eindeutigem Grund und fehlender Zusage; ein gesetztes
Capability-Flag darf den eigenen Test nicht überspringen. Ein `Limited`-
Backend darf fehlende Klassen testen und dokumentieren, ohne deren Erfolg
zu behaupten. Native Compile-/Java-Unit-Tests sind kein Browser-Nachweis.

## 7. Verifikation und Artefakte

Reihenfolge: Kern-/Completion-Tests → native Engine-Tests → generierte Bindings
und Managed-Tests → vollständige Release-Prüfung. Folgende Befehle beschreiben
die weitere Umsetzung; bereits erfolgte Läufe stehen im Ausführungsstand.
Die lokal umgesetzte macOS-/Managed-Fassung ist geprüft; andere Plattformen
bleiben offen. Für sämtliche .NET-Befehle dieser Session zuerst:

```sh
export DOTNET_ROOT="$HOME/.dotnet/neoastra-sdk-10.0.114"
export DOTNET_ROOT_ARM64="$DOTNET_ROOT"
export PATH="$DOTNET_ROOT:$PATH"
```

Reproduktionsbefehle (Release-Staging darf das bestehende macOS-Mindestziel
nicht anheben):

```sh
# macOS arm64; zunächst ohne Staging in eingecheckte Runtime-Dateien
cmake --preset macos-arm64-debug
cmake --build --preset macos-arm64-debug
ctest --test-dir artifacts/native/macos-arm64-debug -R 'request_filter|completion|abi' --output-on-failure

# Bindings und NativeSymbols nach Header-Änderung regenerieren
dotnet run --project src/NeoAstra.CodeGen -c Release

# Für Managed-End-to-End-Tests zuerst passende lokale Runtime bauen/stagen
cmake --preset macos-arm64-release -B artifacts/native/osx-arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=26.0
python eng/build_native.py --rid osx-arm64
dotnet test src/NeoAstra.Tests -c Release --filter 'FullyQualifiedName~RequestFilter'
dotnet run --project src/NeoAstra.Conformance -c Release -- --run --request-filter-only

# Abschließende Projektprüfung
cd src
dotnet build -c Release
dotnet test -c Release
```

Andere Desktop-Hosts verwenden ihre bestehenden Windows-/Linux-Presets;
Linux-Engine-Tests bei Bedarf unter `xvfb-run`. Mobile CMake-Presets schalten
native Tests derzeit ab: für iOS/Android einen tatsächlichen App-/Simulator-
bzw. Emulator-Testpfad ergänzen und ausführen, nicht nur Cross-Compile melden.
Bestehende native CI-Matrix und Exportprüfung nutzen.

Builds/Tests über etwa 30 Sekunden als CodeAlta-Background-Job starten.
Keine Releases, Pushes, fremden Worktrees oder produktiven Browserprofile.
Lokale Staging-Änderungen nur bewusst übernehmen; ausgelieferte RIDs und
NativeSymbols müssen zur neuen ABI passen. Fehlende native Assets nicht
durch Kopieren eines anderen RIDs vortäuschen.

## 8. Definition of Done und Review

- API und C-ABI sind dokumentiert, kompatibel und durch Tests abgesichert.
- Mindestens ein Desktop-Backend hat die HTTP(S)-/Frame-/PerView-Basis
  nachweislich implementiert; für jedes weitere werden Laufzeitnachweis oder
  expliziter Nicht-Support und dessen Fehlerpfad ausgewiesen.
- Kein gesetztes Flag beruht nur auf Vermutung, Header-Verfügbarkeit,
  Goldenfile, Mock oder dem Verhalten einer anderen Plattform.
- Der Standard funktioniert mit belegter Basis; ausdrücklich verlangte
  fehlende Zusatzabdeckung wird abgelehnt. Es gibt keinen stillen No-op für
  eine nicht leere Liste oder ein nicht darstellbares Regelkriterium.
- Initiale Registrierung, lokale Ersetzung und anschließende Filterwirkung
  sind getestet. Cancellation, Popups und konkurrierende Views verletzen
  weder Ownership noch die ausgewiesene View-Isolation.
- Dokumentation erklärt fehlende Klassen, DNS/Rebinding, In-flight Requests,
  Transportverkehr, Update-Propagation, Scope und getestete Engine-/OS-Versionen.
  Sie verspricht weder Zero-TCP noch eine atomare globale Umschaltung.
- Die Isolationsanforderungen aus #490 und die geschützte Annotation-Toolbar
  bleiben unverändert. Keine Gleichsetzung des Filters mit einer Sandbox.
- Relevante Build-/Testsuites sind grün; Fehler im zugesagten Scope sind
  Blocker, fehlende Nachweise keine als bestanden gemeldeten Skips.
  Nicht zugesagte Plattformen/Klassen separat ausweisen statt sie als getestet
  darzustellen. Nach vollständigem Diff-Review Nutzerreview gemäß aktueller
  Einstellung anfordern; erst dann gegebenenfalls committen.
- Der **NeoAstra-Teil** kann nach seinen Implementierungs-/Prüfschritten und
  dokumentierter Übergabe des G5-Testpakets abgeschlossen werden. Für eine
  **CodeAlta-Integrationsfreigabe** ist zusätzlich G5 erforderlich. Einen
  ausstehenden externen Nachweis separat benennen; die Bibliotheksfertigstellung
  ist keine Erklärung, alle CodeAlta-Sicherheitsfälle seien geprüft.

### Was mit dieser Revision erneut freigegeben wird

- Request- statt Verbindungsverweigerung; leere TCP-Verbindungen und TLS-
  Aushandlung sind kein Filterfehler.
- Abschluss nach lokaler Installation statt global bestätigter Engine-Wirkung;
  keine zugesagte Propagationsfrist und kein erfundener Sleep-/JS-Barrierentrick.
- HTTP(S) als Scheme-Default und Basis statt Maximalabdeckung als
  `RequiredFlags`-Default; WebSockets/Worker/Redirects bleiben ausweisbar und
  ausdrücklich anforderbar.
- Getrennte Bibliotheks- und Host-Abnahme, mit echten Browser-/MCP-Guard-Tests
  als G5. Fehlende Testhosts blockieren nicht mehr jede andere Implementierung.

Unverändert bleiben Validierung, unveränderliche Snapshots, Versionsschutz,
Fehlerbehandlung, das Dedicated-Worker-Bit und die gemeinsame C-Filterbeschreibung.
Weiterhin kein per-Profil-Fallback, keine optionale Refusal-Telemetrie und kein
CDP-Ersatzpfad. Andere Änderungen am Sicherheitsumfang benötigen neue Freigabe.

### Vorgesehene Verwendung durch CodeAlta

CodeAlta bildet die bekannten reservierten Ports auf Regeln mit `HostMatch.Any`
ab. So hängt die Sperre dieser Ports nicht vom Schreibweisenvergleich eines
Loopback-Hosts ab; derselbe Port auf einem entfernten Host wird bewusst ebenfalls
gesperrt. Der Projektdatei-Server und normale Dev-Server bleiben außerhalb der
reservierten Liste.

Vor View-Erstellung Capability lesen, Basis prüfen und bei belegtem
`WebSockets`-Flag die WS/WSS-Schemes ergänzen. Fehlende Zusatzklassen sichtbar
machen. Bei `None` kann CodeAlta den Browser nach seinem bereits bestehenden
Isolationsmodell ohne diesen zusätzlichen Filter anbieten, **sofern** die
Host-Abnahme der betreffenden Engine-Konstellation erfüllt ist; keine
allgemeine Sicherheitsfreigabe aus dem Fehlen des Filters ableiten.

Die Liste vor Freigabe der View installieren; bei später bekannt gewordenen
Ports ersetzen. Die Erkennung von Portänderungen und die Nutzeranzeige gehören
zur getrennten CodeAlta-Integration. Der Server-Guard bleibt ab Listenerstart
unabhängig davon aktiv. Weder ein `await` noch eine Filter-Capability darf dazu
verwendet werden, ihn auszuschalten oder einen temporär ungeschützten MCP-
Endpunkt freizugeben.

## Primärquellen

Am 24. November 2026 direkt gelesen. Aktuelle Dokumentation und WebKit-/AOSP-
Header belegen verfügbare Mechanismen, nicht das Verhalten jeder ausgelieferten
Engine-Version. Die Implementierung muss diese Quellen auf ihre tatsächlich
getesteten Versionen beziehen.

#490 beschreibt bei den technischen Lücken den damaligen Stand NeoAstra
`1.13.2`; seine Checkboxen ersetzen keine Prüfung des jetzigen Codes. Das Issue
verlangt keine Zero-TCP- oder atomare Filter-Update-Garantie. Seine Isolation
und sein Ziel, externen und eingebauten Browser als Alternativen anzubieten,
werden dadurch nicht abgeschwächt.

[^issue490]: CodeAlta, „Integrated browser as a tab of CodeAlta Desktop“, insbesondere Goal und Isolation: `https://github.com/CodeAlta/CodeAlta/issues/490`.
[^issue487]: CodeAlta-5.0-Diskussion: integrierter Browser als zweite Option; bekannte native Überlagerungsgrenzen werden akzeptiert: `https://github.com/CodeAlta/CodeAlta/issues/487#issuecomment-7065412314`.
[^issue355]: Annotation-Workflow und geschützte, isolierte Toolbar: `https://github.com/CodeAlta/CodeAlta/issues/355`.
[^codealta-browser]: Geprüfter CodeAlta-Checkout `/Users/bwi/git/CodeAlta`, Commit `64f5408b0`: `src/CodeAlta/Desktop/DesktopBrowser.cs:37-46`, `Desktop/Browser/NeoBrowserViews.cs:225-240`, `Desktop/Browser/Embedded/EmbeddedBrowser.cs:23-35`. Der integrierte Adapter übernimmt den allgemeinen Request-Gate nicht in die Engine.
[^codealta-guard]: Derselbe Checkout: `src/CodeAlta/Desktop/Mcp/DesktopMcpServer.cs:311-331`, `DesktopMcpEndpoint.cs:151-189`; vorhandene künstliche Header-/Server-Tests in `src/CodeAlta.Desktop.Tests/DesktopMcpTests.cs:267-313`. Bei der Untersuchung keine Requests an laufende Nutzerdienste gesendet und keine echten Browser-Guard-Tests ausgeführt.
[^codealta-ports]: Derselbe Checkout: `src/CodeAlta.Core/IO/McpPortRecord.cs:18,111-120` und `src/CodeAlta/Desktop/Mcp/DesktopMcpServer.cs:237-249`; Serverstart vor Portpublikation, Fremdport-Cache bis zu eine Sekunde.
[^webview2]: Microsoft, `ICoreWebView2_22`, Einführung, Filterregistrierung vor Navigation und profilbezogene Worker-Events: `https://learn.microsoft.com/en-us/microsoft-edge/webview2/reference/win32/icorewebview2_22`.
[^apple]: WebKit, öffentliche Header `WKContentRuleListStore.h` und `WKUserContentController.h`: `https://raw.githubusercontent.com/WebKit/WebKit/main/Source/WebKit/UIProcess/API/Cocoa/WKContentRuleListStore.h` und `https://raw.githubusercontent.com/WebKit/WebKit/main/Source/WebKit/UIProcess/API/Cocoa/WKUserContentController.h`.
[^android]: AOSP, `WebViewClient.java`, Dokumentation zu `shouldInterceptRequest`: `https://raw.githubusercontent.com/aosp-mirror/platform_frameworks_base/master/core/java/android/webkit/WebViewClient.java`.
[^gtk]: WebKitGTK API 6.0, `UserContentFilterStore.save` und `UserContentManager.add_filter`: `https://webkitgtk.org/reference/webkitgtk/stable/method.UserContentFilterStore.save.html` und `https://webkitgtk.org/reference/webkitgtk/stable/method.UserContentManager.add_filter.html`.
[^webkit-format]: WebKit, „Content Blockers: First Look“, deklaratives Format, Resource-Typen und eingeschränkte Regex: `https://webkit.org/blog/3476/content-blockers-first-look/`. Historische Formatbeschreibung; heutige Engine-Kompilierung und Matching bleiben verpflichtende Tests.
