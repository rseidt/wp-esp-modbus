# Pool-Wärmepumpe „Evolution Aqua" — ESP32 Modbus/MQTT-Bridge

Projektkontext für Claude Code. Übernommen aus einer vorangegangenen Konversation in der Claude-Chat-Oberfläche.

## Ziel des Projekts

Eine selbstgebaute ESP32-basierte Modbus-RTU/MQTT-Bridge, die eine günstige „Evolution Aqua" Pool-Wärmepumpe lokal steuerbar und auslesbar macht (statt nur über Tuya-Cloud/App). Arduino-Code ist selbst geschrieben, Register wurden per Reverse Engineering ermittelt.

## Hardware

- **Wärmepumpe:** „Evolution Aqua" (Markenname außen)
- **Controller-Board:** OEM/Whitelabel, Aufdruck `SP.KYZ1.5-4.1`. Mit hoher Wahrscheinlichkeit ein Fairland/AquaForte/SPRSUN-Klon auf Tuya-Plattform.
- **Bus:** RS485, Anschlussbuchse mit Belegung `12V- / 12V+ / A / B` (Signalleitungen A und B; G/Masse und +12V Versorgung). Zweite RS485-Buchse auf der Platine liegt physisch **parallel** zum selben Bus.
- **ESP32:** hängt als zweiter Modbus-Master am selben A/B-Paar.
- **WiFi-Modul der WP:** Tuya **WBR3D** (W701D-Chip), sitzt auf der Display-Platine.

## Bus-Topologie (wichtig!)

- Modbus RTU ist ein **Single-Master-Protokoll**. Am Bus hängen effektiv zwei Master:
  1. Das **Bedien-Display** (Master-Einheit, enthält integriertes Tuya WBR3 WiFi-Modul). Steckt an der 12V/A/B-Buchse.
  2. Der **ESP32** (unsere Bridge).
- Der eigentliche **Slave** ist die Wärmepumpen-Hauptplatine (Kompressor, Sensoren, Ventile).
- **Symptom:** ESP32 funktioniert einwandfrei, solange die Hersteller-App geschlossen ist. Sobald die App das Tuya-Modul aufweckt, kollidieren die Anfragen → ESP32 bekommt „Invalid Slave ID" (verstümmelte Antwortframes durch Buskollision).
- **Praxisbefund (WBR3 stillgelegt):** Das Display selbst ist ein eigener Master und pollt weiter gelegentlich über A/B (nur das WiFi-Modul ist abgeschaltet, nicht die Display-MCU). Dadurch bleiben **seltene Restkollisionen** bestehen — viel weniger als bei aktiver App, aber nicht null. Im Betrieb unkritisch: die hohen Retries (`MODBUS_RETRIES_BUS_COLLISION`) fangen sie ab, die Dekodierung bleibt korrekt. Bewusst so belassen.

### These (2026-06-09): Hauptursache des Dauer-Rauschens = fehlende gemeinsame Masse
Wichtige Korrektur des Ursachenbilds. Symptom war **durchgehend** „Invalid Slave ID" und **gar keine** gültigen Reads (nicht nur seltene Restkollisionen), auch ohne dass der ESP selbst schrieb.

- **Befund:** Der ESP wurde aus einer **separaten 5V-Quelle auf der Platine** gespeist, deren GND **keinen Durchgang** zur Busmasse (`12V-` am A/B/12V-Stecker) hatte (per Multimeter bestätigt). RS485 ist differentiell, braucht aber eine **gemeinsame Masse-Referenz** innerhalb des Common-Mode-Bereichs. Ohne sie driftet die Gleichtaktspannung weg → der Empfänger liest Müll → „Invalid Slave ID". Das ist **kein** Zweit-Master-Problem.
- **Konsequenz:** Erst Masse/Verkabelung/Termination prüfen, bevor man Timing/Retries dreht. Verworfen wurden in diesem Zuge: ein **listen-before-talk-Gate** (busy-wait vor jedem Senden — hungerte den synchronen WebServer aus, bei korrekter Masse überflüssig) und die Sorge um **ESP-Selbstkollision** Poller(loop) vs. MQTT-Write(AsyncTCP) (laut Praxis durch Retries harmlos).
- **Geplante Lösung (Stand 2026-06-09, Buck-Wandler noch nicht da):** ESP aus `12V+/12V-` des Bus-Steckers über einen **Buck-Wandler 12V→5V** speisen (Serienwiderstand ist KEINE Option — kein Konstantstrom; LDO würde 12→3,3 V verheizen). Dann liegt ESP-GND fest an `12V-` = Busmasse. Nach Umbau verifizieren: Durchgang ESP-GND ↔ `12V-` ≈ 0 Ω. Die platinenseitige 5V-Domäne ist evtl. galvanisch getrennt (Comms-/Display-Seite) → nicht per Drahtbrücke an die Busmasse zwingen, sondern sauber aus dem Bus-Stecker versorgen.
- **Noch offen:** Erst nach dem Umbau mit gemeinsamer Masse zeigt sich, ob danach noch echte Restkollisionen vom Display übrig bleiben (siehe Praxisbefund oben) oder ob der Bus dann sauber ist.

### AUFLÖSUNG (2026-06-15/16): Masse gefixt → Resttimeouts waren NICHT elektrisch, sondern Inter-Transaktions-Timing
Die obige These zur Masse war richtig für die **CRC-/„Invalid Slave ID"-Fehler**: nach Herstellen der gemeinsamen Masse (ESP-GND ↔ `12V-`) sind diese **weg**. Display ist stillgelegt (kein Zweit-Master-Verkehr im Test). Danach blieben aber **„Response Timed Out"** — deren Ursache war eine **andere** und wurde schrittweise eingegrenzt. Reihenfolge der Fixes:

1. **RS485-Modul HW-519 → MAX3485 ersetzt** (Commit „RS485: HW-519 durch MAX3485…"). Das HW-519 ist ein **Auto-Direction-Modul** (RC-getimte TX/RX-Umschaltung); seine Nachlaufzeit verschluckte den Anfang der Slave-Antwort → Timeouts. MAX3485-Modul („RS485 V2.0", **EN-Pin = DE+/RE**) mit GPIO-gesteuerter Richtung: `RTS = GPIO22` (vorher `NOT_A_PIN`), `pre/postTransmission()` schalten EN, Umschaltung TX→RX nach `flush()` → deterministisch. **5 kΩ Pulldown an EN** (Boot-Float-Schutz). Modul-Silkscreen RXD/TXD ist aus Modul-Sicht → **RX/TX im Code getauscht** (GPIO17=RX, GPIO16=TX), sonst kamen gar keine Daten.
2. **Eigentliche Hauptursache der Timeouts = zu kurzer Abstand zwischen zwei Transaktionen.** Per Datei-Log bewiesen: die jeweils **erste** Range im Zyklus (direkt nach der Pause) klappte immer, jede **folgende** (~100 ms später) lief im 1. Versuch in einen Timeout. **Reorder-Test** (Range-Reihenfolge umgedreht) zeigte: der Fehler klebt an der **Position**, nicht an der Adresse. Fix: Poll-Tick **100 ms → 500 ms** (`MODBUS_POLL_INTERVAL_MS` in `main.cpp`). 500 ms ist empirisch ausreichend; 200 ms ungetestet (der erfolgreiche „2. Versuch" kam nach dem ~2-s-Timeout, also mit ~2 s Abstand — kein Beleg für 200 ms). **Bei Timeout wird der ganze Zyklus verworfen**, nicht im nächsten Tick fortgesetzt.
3. **Fehlerregister 26/27 (new_fault_01) in den getakteten Poller gezogen.** Sie wurden vorher in `writeFaultStatusToJson()` **live und ungetaktet** direkt nach dem Zyklus gelesen → jedes Mal genau ein Timeout. Jetzt: erste Poll-Range `{39,37}` → **`{26,50}`**, ein **Fault-Cache** (`faultRegValue/Valid`, gefüllt via `distributeFaultBlock()`), `writeFaultStatusToJson()` liest aus dem Cache statt vom Bus.
4. **Pause `MODBUS_SCANRATE` 2 s → 1 s** und **Sofort-Feedback nach Write:** nach erfolgreichem (vom Slave bestätigtem) Write aktualisiert `writeModbusRegister()` den Cache `register_values[]` und `onMqttMessage()` ruft sofort `publishModbusData()` (baut `/data` **aus dem Cache, ohne Bus-Read**). Killt den Feedback-Loop/das Flackern beim Umschalten in Home Assistant.

**Konsequenz fürs Ursachenbild:** Für die **Timeouts** sind **Bias/Termination/Common-Mode NICHT die Ursache** (die hätten zufällig über alle Ranges gestreut; hier war es deterministisch positions-/timingabhängig). Bias/Termination bleiben nur eine **Option, falls je wieder zufällige** CRC-/Streufehler auftauchen. Buck-Wandler-Plan (gemeinsame Masse) ist umgesetzt genug, dass keine CRC-Fehler mehr auftreten.

### Debug-Infrastruktur (2026-06-15): persistentes Datei-Log
Zur Ferndiagnose (ESP verbaut, kein Serial-Zugriff) schreibt `log()` jetzt zusätzlich ins LittleFS: `/log.txt` (aktueller Boot) + `/log_prev.txt` (vor letztem Reboot, per Rotation in `initFileLog()`), je 32 KB rollierend, NTP-Zeitstempel, Boot-Banner mit `esp_reset_reason()`. Web: `/logs` (Level-Umschalter), `/log/current`, `/log/previous`. Datei-Log-Level (`fileLogLevel`) ist zur Laufzeit umschaltbar, **getrennt** von `MAX_LOG_LEVEL` (Serial). Diagnose-Trick: kurz auf INFO stellen → „Filling range X..Y" zeigt, welche Range scheitert.

### Debug-Infrastruktur (2026-06-23): Flash-Coredump (Panic-Backtrace ohne Serial)
> **⚠️ ZURÜCKGENOMMEN am 2026-06-23 — Flash-Coredump ist NICHT aktiv.** Das `custom_sdkconfig`, das Coredump-to-Flash aktiviert, zwingt die pioarduino-Plattform zum **From-Source-Rebuild** der Arduino-IDF-Libs. Dieser Build **bootet auf diesem Chip nicht**: früher Bootloop mit `Guru Meditation Error: Cache disabled but cached memory region accessed` im **IDF-Core-Init** (`call_start_cpu0` → `do_core_init` → `init_flash` → `esp_flash_init_main` → `esp_flash_read_chip_id` → `read_id_core` → `memset`). Ursache: der From-Source-SPI-Flash-Treiber wird mit abgeschalteter Cache angefasst, `memset` liegt aber im gecachten Flash → Trap (noch vor der App; per addr2line gegen die From-Source-`firmware.elf` aufgelöst). Der **vorkompilierte Core hatte das nie**.
> **Wichtige OTA-Lehre:** Der erste Flash nach Aktivierung von `custom_sdkconfig` erzeugt auch einen **neuen Bootloader** — ein OTA-`/update` flasht aber NUR die App-Partition. Deshalb hier **nie per OTA** auf einen From-Source-Build wechseln, immer voller Serial-Flash (`pio run -t upload` = Bootloader+Partitions+App). In diesem Fall war zwar der From-Source-Build selbst defekt (s.o.), aber die OTA-Bootloader-Falle wäre die nächste gewesen.
> **Recovery (durchgeführt):** `custom_sdkconfig` aus `platformio.ini` entfernt → zurück auf vorkompilierten Core, `sdkconfig.*` + `.pio/build/ESP32_dev_kit` gelöscht, neu gebaut, voller Serial-Flash. Bootet sauber. Die `/coredump`-Endpunkte bleiben im Code, sind aber per `#if CONFIG_ESP_COREDUMP_*` No-ops (auch der `esp_core_dump_image_check()`-Aufruf in `handleCoredump` musste in den `#if` gezogen werden — dieses Symbol linkt im TO_NONE-Core nicht, `image_get`/`erase` schon).
> **ROOT CAUSE bewiesen (2026-06-23, per `nm`/`addr2line`):** Das ROM liefert ein cache-sicheres `memset` @ `0x4000c44c` (`esp32.rom.newlib-funcs.ld`). Der From-Source-Build linkt aber ein **Flash-`memset` @ `0x401630b0`** (gecachte Region) und leitet `memset` **NICHT** auf die ROM-Adresse um — der vorkompilierte Core tut das. `read_id_core` liegt korrekt im IRAM (`0x40080f40`), ruft aber dieses Flash-`memset` → Trap, sobald die Cache fuer den Chip-ID-Read aus ist.
> **SACKGASSE `CONFIG_SPI_FLASH_ROM_IMPL=y` (2026-06-23 getestet):** Der naheliegende Workaround — den esp_flash-Treiber per ROM-Implementierung umgehen — ist auf dem **ESP32-CLASSIC nicht verfuegbar**: das Symbol `depends on ESP_ROM_HAS_SPI_FLASH`, das nur die neueren Chips (C2/S3/H2/C5/C6) haben. Auf dem klassischen ESP32 enthaelt das ROM keinen esp_flash-Treiber. Kconfig verwirft die Zeile still (nicht im generierten `sdkconfig`), der Build ist dann identisch mit dem brickenden — daher gar nicht erst geflasht.
> **Wiederaufnahme** nur loesbar, wenn der From-Source-Link `memset` (und ggf. weitere cache-disabled-genutzte libc-Funktionen) auf die ROM-Adressen umbiegt wie der vorkompilierte Core — offenes arduino-esp32-Linker-Config-Problem. **Separat ausserhalb der verbauten WP** testen, nicht am Produktivgerät. Realistische Empfehlung: Flash-Coredump auf diesem Chip ad acta, Crash-Diagnose ueber Serial-Decoder + Datei-Log-Breadcrumbs.

Das Datei-Log zeigt nur Breadcrumbs bis zum Crash, **nie** den Panic-Backtrace — der IDF-Panic-Handler dumpt Register/Backtrace klassisch nur auf UART. Da der ESP verbaut ist (kein Serial), war der Plan, den Backtrace persistent in die Flash-**coredump-Partition** zu schreiben und per HTTP abzuholen (siehe Warnung oben — verworfen). Beschreibung des ursprünglichen Plans:

- **Aktivierung** (`platformio.ini`, `custom_sdkconfig`): `CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y` + `DATA_FORMAT_ELF` + `CHECKSUM_SHA256`. **Wichtig:** Die Plattform ist bereits die **pioarduino/tasmota-Fork** (`platform = espressif32` → `espressif32@2026.4.30`, arduino-esp32 3.3.7), die bei gesetztem `custom_sdkconfig` die Arduino-IDF-Libs **aus Quelle rekompiliert** (`SConscript espidf.py`). Der offizielle platformio-Core (vorkompiliert, `COREDUMP_ENABLE_TO_NONE` fest) konnte das NICHT — daher war Coredump dort prinzipiell unmöglich (nur Serial-Decoder).
- **Kconfig-Choice-Falle:** `ENABLE_TO_NONE/TO_FLASH/TO_UART` sind ein *choice*. Der Basis-sdkconfig setzt `TO_NONE=y`; ein blosses `TO_FLASH=y` reicht NICHT (TO_NONE überlebt das In-Place-Merge und gewinnt → Coredump bleibt aus, Symbol `esp_core_dump_image_check` undefined → Linkfehler). Lösung: `CONFIG_ESP_COREDUMP_ENABLE_TO_NONE=n` **explizit** mit angeben. Verifikation: `sdkconfig.ESP32_dev_kit` muss `TO_FLASH=y` + `# ...TO_NONE is not set` zeigen, und `firmware.elf` muss `esp_core_dump_get_summary`/`image_check` als `T` definieren.
- **Erster Build nach Aktivierung kompiliert den Core neu** (~1,5–9 min, einmalig; danach gecacht). pioarduino legt `sdkconfig.defaults` + `sdkconfig.<env>` im Projekt ab (gitignored).
- **Partition:** `default.csv` reserviert die coredump-Partition (`0x3F0000/0x10000`, 64 KB) bereits → **KEINE** Repartitionierung, LittleFS-Logs bleiben erhalten.
- **Web-Endpunkte** (`setupWebserver.cpp`): `GET /coredump` (lesbare Summary: PC, crashed task, Backtrace-Adressen, exc_cause/vaddr, App-SHA256), `GET /coredump.elf` (Roh-ELF, chunked aus der Partition gestreamt → `espcoredump.py info_corefile -t elf -c coredump.elf firmware.elf` oder `addr2line -e firmware.elf <PCs>`), `POST /coredump/erase`. Boot-Banner (`initFileLog`) vermerkt zusätzlich „Coredump: vorhanden …" wenn einer anliegt.
- **WICHTIG fürs Dekodieren:** Die exakt geflashte `firmware.elf` (`.pio/build/ESP32_dev_kit/firmware.elf`) **aufheben** — Backtrace-Adressen sind nur damit (SHA muss matchen) auflösbar.

### Crash behoben (2026-06-16): wiederkehrende Task-Watchdog-Resets durch blockierenden Write-Retry-Loop
Per persistentem Datei-Log (`/log/previous`) als **`Reset reason: Task watchdog`** identifiziert — und kein Einzelfall (auch das vorherige Boot-Banner zeigte denselben Grund). **Nicht** der Heap (durchgehend ~254 k gesund → Speicherleck ausgeschlossen) und **nicht** der Read-Poller.

- **Ursache = Write-Pfad** `writeModbusRegister()` in `modbus_base.cpp`: enge `for i=1..N`-Retry-Schleife in EINEM Aufruf, Budget war das hohe `MODBUS_RETRIES_BUS_COLLISION` (30). Jeder `writeSingleRegister()` blockiert ~1 Modbus-Timeout (~1–2 s) per **Busy-Wait ohne yield**. Lief ein write-naher Read/Write in den Timeout (siehe Inter-Transaktions-Timing oben), summierten sich bis zu ~30–60 s CPU-Blockade am Stück → die IDLE-Task verhungert → Task-Watchdog-Reset.
- **Log-Signatur:** `Writing data` **ohne** folgendes `Data written` + danach ~25 s Funkstille bis zum nächsten Boot-Banner. Die `Trial i/N`-Zeilen sind INFO und stehen bei `fileLogLevel=WARNING` nicht im Datei-Log → der Sturm ist im Datei-Log unsichtbar, nur Anfang/Stille sieht man.
- **Abgrenzung:** Der **Read-Poller** `fillRegisterValues()` ist NICHT betroffen — 1 Transaktion pro Tick, `currentTryIndex` über Ticks verteilt, blockiert nie lange. Sein hohes Budget (30) bleibt bewusst.
- **Fix:** eigenes kleines `MODBUS_WRITE_RETRIES_BUS_COLLISION` (= 6) nur für den Write; zwischen den Versuchen `delay(MODBUS_TX_SPACING_MS)` (yieldet via `vTaskDelay` → IDLE läuft → WDT gefüttert; gibt dem Slave zugleich Abstand) + `esp_task_wdt_reset()` (greift wenn die Task registriert ist, no-op im AsyncTCP-/MQTT-Callback-Kontext). Der eigentliche Schutz ist das **yield**, der Cap begrenzt zusätzlich die Latenz.
- **Begünstigend im Crash-Log** (nicht Ursache, aber Auslöser-Häufung): MQTT-Flapping (`Disconnected from MQTT: 0`), **leere Write-Payloads** aus Home Assistant (`write_register ohne gueltiges 'name=value' (Payload='')`) und Reg-92-Salven im ~1,5-s-Takt. Lohnt separat anzusehen.

### Architektur-Refactor (2026-06-17): dedizierter Modbus-Worker-Task statt Poll-Timer
**Ersetzt das alte Timer-/Pause-Pollmodell** (`modbus_poller_timer`, `runModbusPollerTask`, `modbus_poller_inprogress/paused`, `startModbusPoller/stopModbusPoller`, der Pause/Resume-Block in `loop()` — alle entfernt). Grund: Der Bus wurde aus **zwei FreeRTOS-Tasks** angefasst — Loop-Task (Poll-Read via Timer-Tick) und **AsyncTCP-Task** (`writeModbusRegister` direkt aus `onMqttMessage`) — ohne Mutex. Der Watchdog-Crash (Write-Busy-Wait im AsyncTCP-Kontext), das Flackern und latente Cache-Races waren Symptome dieses einen Strukturproblems.

- **Worker = alleiniger Bus-Owner** (`modbusWorkerTask` in `modbus_base.cpp`, `xTaskCreatePinnedToCore` auf **Core 0**, Stack 4096). NUR dieser Task ruft je `readHoldingRegisters`/`writeSingleRegister`. Loop: App-Modus-Check → Queue (Requests haben Vorrang) → sonst eine Poll-Range. Zwischen Transaktionen `vTaskDelay(MODBUS_POLL_INTERVAL_MS)`, zwischen vollen Zyklen `MODBUS_SCANRATE_MS` (beide Konstanten jetzt in `modbus_base.h`).
- **Requests statt direktem Bus-Zugriff** (`QueueHandle_t modbusRequestQueue`, Länge 8):
  - **Write:** `onMqttMessage` (AsyncTCP) ruft nur noch `enqueueModbusWrite()` (non-blocking) → **kein Blockieren mehr im AsyncTCP-Callback**. Der Worker führt den Write aus, aktualisiert den Cache und setzt `requestPublish()`.
  - **Dump:** `/modbusdump` ruft `modbusDump()` → Request + Warten auf Completion-Semaphore (Timeout 20 s). Blockiert nur den HTTP-Handler, nicht mehr den Bus außerhalb des Owners.
- **Cache hinter Mutex** (`registerCacheMutex`): nur der Worker schreibt `register_values[]`/Fault-Cache (`distributeBlock`/`distributeFaultBlock`/Write-Update, je kurz unter Lock); `publishModbusData()` liest unter demselben Lock einen konsistenten Snapshot.
- **Publish zentralisiert im Loop-Task:** Worker setzt `g_modbusPublishRequested`; `loop()` ruft via `consumeModbusPublishRequest()` → `publishModbusUpdate()`. So bedient genau **ein** Task den AsyncMqttClient (kein Cross-Task-Publish mehr).
- **App-Modus** (`appControlMode`, jetzt `volatile`): `setControlMode()` setzt nur noch Flag + `WBR3_EN_PIN` (kein Timer-Gefummel). Der Worker prüft `isAppControlMode()` und fasst den Bus dann nicht an; anstehende Requests werden sofort fehlschlagend quittiert.
- **Watchdog:** Der Worker ist **bewusst NICHT** beim Task-WDT registriert (ein langer Dump bräuchte sonst evtl. >5 s ohne Reset → falscher TWDT-Reset). Schutz kommt vom Yielden (`vTaskDelay` je Iteration + `modbusIdle`/`delay(1)` während der Bus-Wartezeit → IDLE-Task läuft). Das Retry-Cap + yield aus dem 2026-06-16-Fix bleibt als Sicherheitsnetz.
- **Verbleibend (nicht in diesem Refactor):** Dedup `getModbusBlock`/`readHoldingRange`, Cache-Staleness/Zeitstempel, Config-Leak `serializeJson(json, Serial)` in `setupWifiManager.cpp` (MQTT-Config auf Serial), positionsbasierte Register-Indexierung. Siehe Plan-Datei `mache-ein-gesamt-asessment-...md` (Tier 2–4).

## Lösung des Master-Konflikts: WBR3 deaktivieren

Da auf die App verzichtet werden kann (Steuerung künftig über MQTT/Home Assistant), wird das Tuya-Modul stillgelegt. Damit ist der ESP32 alleiniger aktiver Master.

- Das WiFi-Modul ist ein **Tuya WBR3D** (FCC-ID 2ANDL-WBR3), sitzt auf der **Display-Platine** (nicht separat ausbaubar ohne LCD-Risiko).
- **Lösung (HARDWARE FERTIG):** WBR3D EN-Pin (Pin 3) über einen Transistor schaltbar auf GND gelegt → Tuya-Modul lässt sich vom ESP32 deaktivieren/aktivieren.
  - EN ist active-high und extern per Pull-up hochgezogen → muss aktiv auf GND gezogen werden, „abknipsen" reicht nicht.
  - Pin 3 liegt am Modulrand (3. Pin von einem Ende), gut erreichbar. Pin 9 (GND) liegt gegenüber.
  - Verbrauch im disabled-Zustand: ~1,5 µA → Modul komplett stumm.
  - Modul NICHT auslöten (LCD-Beschädigungsgefahr). Modul bleibt stecken.
  - Kein SMD-Rework nötig: Feinlötkolben (1–2 mm Spitze), dünner Draht (Kynar/Litze), Drahtbrücke Pin3→Pin9.
  - **Reversibel** möglich: kleinen Schalter/Jumper zwischen EN und GND statt fester Brücke.

### Gelötete EN-Steuerschaltung (FERTIG, verbaut)
BC547 NPN als Low-Side-Switch:
- **Kollektor → WBR3D EN (Pin 3)**
- **Emitter → GND** (WBR3D Pin 9 + ESP32 GND gemeinsam)
- **Basis → R_B 4,7 kOhm → ESP32-GPIO**
- **R_PD 10 kOhm** von Basis nach GND (definierter Sperr-Zustand beim Boot)
- WBR3D-seitiger Pull-up auf EN (~9,1 kOhm gegen VCC) ist herstellerseitig vorhanden und wird mitgenutzt.

**Logik:**
- GPIO HIGH -> BC547 leitet -> EN auf GND -> **WBR3D AUS**
- GPIO LOW / waehrend Boot -> BC547 sperrt -> Pull-up zieht EN auf 3,3 V -> **WBR3D AN**

**WICHTIG fuer den Code:**
- Gewuenschter Normalzustand = **WBR3D AUS** (fuer konfliktfreien Modbus). Daher in `setup()` so frueh wie moeglich: `pinMode(EN_PIN, OUTPUT); digitalWrite(EN_PIN, HIGH);`
- GPIO muss ein freier Pin sein, KEIN Strapping-Pin (nicht GPIO0/2/12/15), kein input-only (nicht 34-39). Verwendet wird Pin 21
- Wenn WBR3D aktiviert wird (z.B. fuer WP-Firmwareupdate via Hersteller-App), MUSS das ESP32-Modbus-Polling pausiert werden, sonst kehrt der Buskonflikt zurueck.


## Register-Mapping (Reverse Engineered)

Die Modbus-Holding-Register sind eine **Spiegelung der Tuya-Datapoint-(DP-)IDs**. Setpoints zeigen ein **+5-Offset** zur DP-ID. **Skalierung der Temperaturen: roher Wert ÷ 10 = °C.**

### Bestätigte Holding-Register (0-Basiert)
| Modbus-Adr | Funktion | Tuya-DP | Skalierung/Werte |
|---|---|---|---|
| 39 | Status-Bitmap | – (zusammengesetzt) | Bit 0 = Wasserpumpe, Bit 1 = Kompressor läuft („Running" vs. „Standby"), Bit 3 = 4-Wege-Ventil/Magnetventil (vermutet), Bits 4+5 = Heizmodus aktiv. Typische Werte: 0=Aus, 1=nur Wasserpumpe, 49=Heat-Modus idle/Standby, 59=Heat-Modus aktiv heizend |
| 26 | Fehlertabelle new_fault_01 LOW (E01–E16) | DP103-Spiegel (lo) | Bitmap, Bit N = E(N+1). Im Dump-Diff = 0 (kein E01–E16 aktiv) |
| 27 | Fehlertabelle new_fault_01 HIGH (E17–E30) | DP103-Spiegel (hi) | Bitmap, Bit N = E(N+17). **Bestätigt:** Flow-Fehler → 1 (Bit 0 = E17, deckt sich mit Display) |
| 47 | Wassertemperatur (Spiegel von Reg 50) | (DP16-Spiegel) | ÷10 °C, ≈ Reg 50 (Dump: 208 vs. 210). Spiegelt Reg 50, nicht Reg 51 |
| 50 | Wassertemperatur aktuell | DP16 (temp_current) | ÷10 °C (212 → 21,2 °C). Bestätigt via Kreuzvergleich (Aus: 21,5 °C kalt, Heizbetrieb: 22,8–25,2 °C) |
| 92 | Ein/Aus | DP1 (switch) | bool 0/1 |
| 93 | Modus | DP2 (mode) | 2=Heat, 0=Auto, 1=Cool (in allen Scans 2, da nur Heizbetrieb getestet) |
| 105 | Solltemperatur Heizen | DP101 (set_heating_temp) | °C, Bereich 5–55, in Scans als ganze °C (24, 28 — nicht ×10!). Achtung: Skalierung weicht von DP-Definition ab |
| 106 | Solltemperatur Kühlen | DP102 (set_cold_temp) | °C, Bereich 5–35 (vermutet, in Scans = 27) |
| 108 | Solltemperatur Auto | DP104 (set_auto_temp) | °C, Bereich 5–40 (vermutet, in Scans = 30) |
| 132 | Sub-Mode (Boost/Eco/Silent) | DP117 (frequency) | 0=Silent(?), 1=Eco/Smart, 2=Boost/Powerful |

### Status-Bitmap Reg 39 — Beobachtete Werte
| Zustand | Reg 39 dez | binär | Interpretation |
|---|---|---|---|
| Pumpe komplett aus | 0 | `00000000` | nichts läuft |
| Standby, nur Wasserpumpe | 1 | `00000001` | Bit 0 |
| Heat-Modus, Kompressor idle | 49 | `00110001` | Bits 0,4,5 |
| Heat-Modus, aktiv heizend | 59 | `00111011` | Bits 0,1,3,4,5 |

Praktische Regeln:
- `reg39 & 0x01` → Wasserpumpe läuft
- `(reg39 >> 1) & 0x01` → Kompressor heizt aktiv (= „nicht in Standby")

### Vermutete Register (noch nicht eindeutig zugeordnet)
| Modbus-Adr | Vermutung | Begründung aus Scans |
|---|---|---|
| 41 | Soll-Frequenz/Kompressor-Leistung | 0=Aus, 67=Eco-modulation, ~8270=Standby idle, 16451=volle Heizleistung |
| 48 | Kompressor-Ist-Frequenz/-Last | 0 wenn nicht aktiv, 13–21 bei aktiver Heizung |
| 64 | Lüfter-Drehzahl/Sensor | variiert mit Last (100=Eco/Aus, 230–350=aktiv) |
| 75 | Kompressor-Verbrauch o.ä. | 0 in Standby, 5–15 nur in aktiv heizenden Zuständen |

### Widerlegte Zuordnungen (aus Datenmodell entfernt)
| Modbus-Adr | Frühere Vermutung | Warum widerlegt |
|---|---|---|
| 15 | Umgebungstemperatur (DP26) | lieferte ~9,1 °C, reale Umgebungstemperatur lag aber > 20 °C → passt nicht |
| 17 | weiterer Umgebungssensor | ~9,2 °C, dito |
| 19 | Abgastemperatur (DP24, venting_temp) | ~66,2 °C, aber im Dauer-Heizbetrieb eher unter Umgebungstemperatur erwartet → unplausibel |

Umgebungstemperatur bleibt **UNIDENTIFIZIERT** (around_temp, DP26) — auch der Quervergleich mit den alten Scans liefert keinen Treffer (siehe unten). Echte Abgas-/Heißgastemperatur (venting_temp, DP24) taucht ebenfalls in keinem Scan als plausibler Wert auf → offen.

### Temperatur-Block Reg 51–55 — Quervergleich alt vs. neu (Vollheiz-Zustand)
Wichtig: alte Scans sind **1-basiert**, neue Dumps **0-basiert** → `old_reg = new_reg + 1`.
Verglichen wurde der identische Betriebszustand (Status 59, Freq 16451 = Vollheizen),
der in alt **Boost-Heat**/**Wieder-Heizen** und im neuen `clean`-Dump vorliegt. Damals
~20 °C Außentemp, jetzt 14 °C → erwartete Differenz eines echten Umgebungssensors ≈ −6 °C (−60 roh).
| Reg (neu) | Boost (alt) | clean (neu) | Δ | Deutung |
|---|---|---|---|---|
| 53 | 150 (15,0) | 91 (9,1) | **−59** | trackt die −6 °C exakt, liegt aber im Heizbetrieb ~5 °C UNTER Umgebung → **Verdampfer/Spule** (coiler_temp), nicht Umgebung |
| 51 | 139 (13,9) | 98 (9,8) | −41 | dito, zweite Spulen-/Verdampferseite |
| 52 | warm | 223 (22,3) | – | wasser-/kondensatorseitig |
| 54 | 190 (19,0) | 203 (20,3) | +7 | liest ~20 °C in BEIDEN Sessions → nicht umgebungsgetrieben |
| 55 | 170 (17,0) | 140 (14,0) | −25 | trackt Umgebung nur schwach (−2,5 statt −6 °C); las 16–17 °C bei real 20 °C → unklar |

Befund: Kein Register liest echte Umgebungsluft (~20 alt / ~14 neu). Die einzigen zustands-
**unabhängigen** Temp-Register (neu 18 = const 173, neu 72 = const ~230) sind in BEIDEN
Sessions konstant, fielen also NICHT von 20→14 °C → ebenfalls nicht Umgebung. around_temp
ist mit den vorhandenen Daten nicht auffindbar; ggf. liefert die Pumpe DP26 gar nicht über Modbus.
Reg 51/53 als coiler-Kandidaten, 52/54 als Wasser/Gas — final per Kühlbetrieb-Diff zuordnen.

### Noch zu verifizieren (Live-Scan)
- Bit 3 und Bits 4/5 von Reg 39 einzeln zuordnen (Modus-Wechsel beobachten).
- Reg 41-Skalierung: ist das die Kompressor-Soll-Hz oder ein internes RPM-Maß?
- Reg 51–54, 72, 74 final als coiler/effluent/return zuordnen (Reg 55 = Umgebung bestätigt; siehe Temperatur-Block oben).
- Fehlertabellen jenseits new_fault_01 (Reg 26/27): Adressen von new_fault_02, fault_2/3 und Treiberfehler (F-/D-Codes) per Diff suchen; im modbus_faults.h stehen sie noch auf FAULT_ADDR_TODO.
- Magic-Value `32765` (0x7FFD) = Sensor nicht angeschlossen / Wert ungültig → ignorieren.
- `-1012` / `0xFC0C` (= 64524 unsigned, Reg 89) = Sensor offen / nicht belegt.
- Silent-Mode Scan, um Reg 132 = 0 zu bestätigen.

## Vollständige Tuya-DP-Liste (OEM-Referenzdesign „海外泳池机公版")

Quelle: GitHub make-all/tuya-local Issue #1712 (AquaForte Full Inverter Pool Heat Pump, product_id k5vqutj2llzox1gg).

| DP | Code | Bedeutung | Typ | Details |
|---|---|---|---|---|
| 1 | switch | Ein/Aus | bool | true/false |
| 2 | mode | Betriebsmodus (Inverter) | enum | Auto, Heating_Powerful, Cooling_Powerful, Heating_Smart, Cooling_Smart, Heating_Silent, Cooling_Silent |
| 5 | work_mode | Betriebsmodus (Festfrequenz) | enum | Cool_mode, Heat_mode, Auto_mode |
| 6 | temp_unit_convert | Temperatureinheit | enum | c, f |
| 15 | fault | Störungsmeldung | bitmap | EE, E01–E29 |
| 16 | temp_current | Aktuelle Temperatur | value | °C |
| 17 | work_state | Arbeitsstatus | enum | Running, Defrosting, Standby, Fault |
| 23 | coiler_temp | Heizregister-Temperatur | value | °C |
| 24 | venting_temp | Abgastemperatur | value | °C |
| 25 | effluent_temp | Auslaufwassertemperatur | value | °C |
| 26 | around_temp | Umgebungstemperatur | value | °C |
| 35 | temp_current_f | Aktuelle Temperatur | value | °F |
| 38 | around_temp_f | Umgebungstemperatur | value | °F |
| 39 | venting_temp_f | Abgastemperatur | value | °F |
| 40 | effluent_temp_f | Auslaufwassertemperatur | value | °F |
| 41 | coiler_temp_f | Heizregister-Temperatur | value | °F |
| 101 | set_heating_temp | Solltemperatur Heizen | value | min 50, max 550, °C (÷10) |
| 102 | set_cold_temp | Solltemperatur Kühlen | value | min 50, max 350, °C (÷10) |
| 103 | new_fault_01 | Fehlercodetabelle 1 | bitmap | E01–E30 |
| 104 | set_auto_temp | Solltemperatur Auto | value | min 50, max 400, °C (÷10) |
| 105 | set_heating_temp_f | Solltemperatur Heizen | value | °F |
| 106 | set_cold_temp_f | Solltemperatur Kühlen | value | °F |
| 107 | new_fault_02 | Fehlercodetabelle 2 | bitmap | E31–E43 |
| 108 | set_auto_temp_f | Solltemperatur Auto | value | °F |
| 109 | unit_type | Gerätetyp | value | 0/1 |
| 110 | new_driver_fault_01 | Treiberfehler-Tabelle 1 | bitmap | F01–F30 |
| 111 | new_driver_fault_02 | Treiberfehler-Tabelle 2 | bitmap | F31–F48 |
| 117 | frequency | Frequenzmodus | enum | Silent, Smart, Powerful |
| 118 | fault_2 | Störungsmeldung 2 | bitmap | E30–E59 |
| 119 | fault_3 | Störungsmeldung 3 | bitmap | E60–E88, display |
| 120 | driver_fault_1 | Treiberfehler | bitmap | D17–D46 |
| 121 | return_temp | Rückgastemperatur | value | °C |
| 122 | return_temp_f | Rückgastemperatur | value | °F |
| 123 | cool_coiler_temp | Kühlregister-Temperatur | value | °C |
| 124 | cool_coiler_temp_f | Kühlregister-Temperatur | value | °F |
| 125 | opening | EEV-Öffnung (Expansionsventil) | value | 0–1000, Einheit P |

Achtung: `_f`-Setpoints (DP105/106/108) sind in der Cloud-Definition Fahrenheit. Im Modbus-Spiegel dieser Pumpe scheinen 106/107/109 die °C-Sollwerte mit +5-Offset zu DP101/102/104 zu sein → beim Live-Scan verifizieren.

## Offene TODOs

- [x] WBR3 EN→GND-Schaltung (BC547 an GPIO 21) gebaut und in Software angebunden: `WBR3_EN_PIN 21` wird in `setup()` sofort auf HIGH gesetzt (WBR3D AUS, ESP alleiniger Master). Webserver-Umschalter `/control` schaltet zwischen MQTT-Steuerung (Pin HIGH, Modbus-Poll aktiv) und Hersteller-App (Pin LOW, Modbus-Poll pausiert) — kein Buskonflikt. Master-Konflikt beseitigt.
- [x] ~~Live-Scan Register 100–130 bei laufender Pumpe~~ → ersetzt durch 7 vollständige State-Scans 1–250 in `resources/modbus-scans/csv/`. Setpoints (Reg 105), Modus (Reg 93), Sub-Modus (Reg 132) bestätigt.
- [x] Geräte-Fehlertabelle new_fault_01 lokalisiert: **Reg 26 (lo) / Reg 27 (hi)**, Flow-Fehler = E17 (Reg 27 Bit 0), per Display bestätigt. In `modbus_faults.h` eingetragen.
- [ ] Umgebungstemperatur (around_temp, DP26) **weiterhin offen** — Quervergleich alt(20 °C)/neu(14 °C) ergab keinen Treffer; Reg 55 als Kandidat verworfen. Evtl. liefert die Pumpe DP26 nicht per Modbus. Reg 15/17 bleiben widerlegt.
- [ ] Restliche Fehlertabellen (new_fault_02, fault_2/3, F-/D-Treiberfehler) und Abgastemp (venting_temp, DP24) per weiterer Diffs lokalisieren — stehen noch auf FAULT_ADDR_TODO bzw. offen.
- [ ] Temperatur-Block Reg 51–55 final coiler/effluent/return zuordnen (Kandidaten als `temp_*_v` im Datenmodell, über MQTT mitloggen); Reg 51/53 = Verdampfer/Spule-Kandidaten.
- [ ] Reg 39 Bitmap durchprobieren (Modus auf Cool stellen, Standby/aktiv jeweils scannen → welche Bits ändern sich?).
- [x] Silent-Mode Scan, um Reg 133 = 0 zu bestätigen.
- [ ] Arduino-Code: Register-Konstanten + Skalierungs-/Enum-Dekodierung sauber strukturieren (Reg 39 als Bitmap, Reg 50÷10, Reg 105 in ganzen °C).

## Vorhandene Dateien im Projekt
- `resources/modbus-scans/*.png` — Screenshots der ModScan-Aufnahmen (7 States × 2 Hälften: Reg 1–125 und 126–250).
- `resources/modbus-scans/csv/*.csv` — kombinierte CSVs pro Zustand (Register 1–250):
  - `Status-Aus.csv` — Pumpe komplett aus
  - `Standby-Ohne-Wasserpumpe.csv` — Standby, Wasserpumpe aus
  - `Standby-Mit-Wasserpumpe.csv` — Standby, nur Wasserpumpe läuft
  - `Standby.csv` — Heat-Modus, Kompressor in Modulationspause
  - `Eco-Heat.csv` — Heat-Modus, Eco-/Smart-Frequenz
  - `Boost-Heat.csv` — Heat-Modus, Boost-/Powerful-Frequenz
  - `Wieder-Heizen.csv` — Standby → Heat (Sollwert hochgesetzt), Kompressor aktiv
- Arduino-Sketch (selbst geschrieben) unter `src/`.
