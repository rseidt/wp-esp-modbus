# Fehlercodes & Fehler-Datenstruktur

Diese Datei dokumentiert **ausschließlich** die Geräte-Fehlerbehandlung der Bridge:
die Datenstruktur im MQTT, die Decode-Mechanik, die bekannten Fehlercodes und ihr
Mapping auf die Modbus-Register/E-Codes.

> Abgrenzung: **Geräte-Fehler** (die Pumpe meldet einen Defekt, z. B. „Flow Control")
> sind hier beschrieben. **Bus-/Transport-Fehler** (Modbus-Timeout, CRC, Tuya-Kollision)
> sind ein anderes Thema und werden über das separate Topic `.../modbus_status` gemeldet,
> nicht hier.

Quelle der Bit-Struktur: tuya-local Issue #1712 (product_id `k5vqutj2llzox1gg`,
OEM-Referenzdesign „海外泳池机公版") sowie ein benanntes Public-Fault-Bitmap.

---

## 1. MQTT-Datenstruktur

Die aktiven Gerätefehler werden **in dasselbe JSON** geschrieben wie die Registerwerte
(Topic `.../data`) — kein eigenes Topic. Erzeugt von `writeFaultStatusToJson()`.

```json
{
  "...": "... übrige Registerwerte ...",
  "fault_active": true,
  "faults": ["E17"]
}
```

| Feld | Typ | Bedeutung |
|---|---|---|
| `fault_active` | bool | `true`, sobald **mindestens ein** Fehler-Bit gesetzt ist |
| `faults` | string[] | Liste der **aktiven** Codes/Labels. Nur gesetzte Bits; im Normalbetrieb leer (`[]`) |

Eigenschaften:
- Es werden **nur gesetzte Bits** aufgenommen → kein Dauer-Null-Ballast.
- Solange ein Fault-Register noch nicht lokalisiert ist (Adresse `FAULT_ADDR_TODO`),
  wird es **übersprungen** — es findet dafür **kein Bus-Zugriff** statt.
- Ein Lesefehler eines Fault-Registers (z. B. Buskollision) lässt diesen Zyklus für
  dieses Register aus; `faults` enthält dann ggf. nur die erfolgreich gelesenen Tabellen.

---

## 2. Decode-Mechanik

Jede Tuya-Fehlertabelle ist ein **sequenzielles Bitfeld**: Bit *N* = der *N*-te Fehlercode.
Eine Tabelle, die mehr als 16 Codes umfasst, verteilt sich auf **aufeinanderfolgende
16-bit-Modbus-Register** (lo-Wort = niedrige Codes, hi-Wort = höhere Codes).

Definition pro Register (`fault_register_t` in `src/modbus_faults.h`):

| Feld | Bedeutung |
|---|---|
| `modbus_addr` | echte Holding-Register-Adresse, oder `FAULT_ADDR_TODO` (0xFFFF) wenn noch unbekannt |
| `dp_name` | Quell-/Beschreibungsname (z. B. `new_fault_01_hi`) |
| `code_prefix` | `'E'` / `'F'` / `'D'` für generierte Codes; `0`, wenn `labels[]` benutzt wird |
| `first_code` | Codenummer von **Bit 0** dieses Registers |
| `bit_count` | Anzahl gültiger Bits (≤ 16) |
| `labels[16]` | explizite Labels Bit0..Bit15; `nullptr` → Code wird generiert |

**Code-Berechnung für ein gesetztes Bit `b`:**

```
labels[b] != nullptr   →  faults += labels[b]                  (z. B. "flow_fault")
sonst                  →  faults += sprintf("%c%02u",
                                     code_prefix, first_code + b)   (z. B. "E17")
```

Beispiel: `new_fault_01_hi` hat `code_prefix='E'`, `first_code=17`.
Ist Bit 0 gesetzt → `E` + `17+0` = **`E17`**. Bit 1 → `E18`, usw.

---

## 3. Register-Mapping (Stand: aus Dump-Diff)

`faultRegisters[]` in `src/modbus_faults.h`. Adressen sind 0-basiert (wie im Code/Dump).

| Tabelle (dp_name) | Modbus-Reg | Codes | Status |
|---|---|---|---|
| `fault_main` (semantisch) | `TODO` | siehe §6 | unbelegt — auf dieser Pumpe nicht nachgewiesen |
| `fault_main_hi` | `TODO` | `motor_fault` | unbelegt |
| **`new_fault_01_lo`** | **26** | **E01–E16** | ✅ per Dump-Diff bestätigt |
| **`new_fault_01_hi`** | **27** | **E17–E30** | ✅ bestätigt (E17 = Bit 0, s. §4) |
| `new_fault_02` | `TODO` | E31–E43 | Adresse unbekannt |
| `fault_2_lo` / `_hi` | `TODO` | E30–E45 / E46–E59 | Adresse unbekannt |
| `fault_3_lo` / `_hi` | `TODO` | E60–E75 / E76–E88 | Adresse unbekannt |
| `driver_fault_01_lo`/`_hi` | `TODO` | F01–F16 / F17–F30 | Adresse unbekannt |
| `driver_fault_02_lo`/`_hi` | `TODO` | F31–F46 / F47–F48 | Adresse unbekannt |
| `driver_fault_1_lo`/`_hi` | `TODO` | D17–D32 / D33–D46 | Adresse unbekannt |

> `TODO` = `FAULT_ADDR_TODO` (0xFFFF): wird beim Decode übersprungen, kein Bus-Zugriff.
> Adresse eintragen, sobald ein Diff (Sauber-Dump vs. provozierter Fehler) das Register zeigt.

---

## 4. Bestätigter Fall: E17 = „Flow Control"

Der einzige bisher **am Gerät verifizierte** Code.

- **Provokation:** Wasserpumpe ausschalten → Strömungswächter meldet fehlenden Durchfluss.
- **Display zeigt:** `E17`.
- **Dump-Diff** (clean vs. Fehlerzustand): **Reg 27** springt von `0` auf `1` (Bit 0).
  Mit `new_fault_01_hi.first_code = 17` ergibt Bit 0 = **E17** — deckungsgleich mit dem Display.

Damit ist die Abbildung DP103 (`new_fault_01`) → Reg 26 (lo) / Reg 27 (hi) **eindeutig**.

Bemerkenswert: Der Wasserdurchfluss-Fehler erscheint auf dieser Pumpe **als numerischer
E-Code** (`E17`), **nicht** als `flow_fault`-Bit des semantischen `fault_main`-Bitmaps (§6).
Das semantische Bitmap bleibt darum mangels Nachweis unbelegt.

| Reg | clean | E17-Zustand | Deutung |
|---|---|---|---|
| 26 | 0 | 0 | `new_fault_01` lo (E01–E16) — keiner aktiv |
| 27 | 0 | **1** | `new_fault_01` hi (E17–E30) — **Bit 0 = E17** |

---

## 5. Tuya-DP-Fehlertabellen (Code-Quelle)

Welcher DP welche Codenummern als Bitmap führt. Diese Struktur liefert die **Bit→Nummer**-
Zuordnung; die **textliche Bedeutung** der einzelnen Nummern ist gerätespezifisch und
außer für `E17` (§4) auf dieser Pumpe **nicht** verifiziert.

| DP | Code (tuya) | Bedeutung | Codes |
|---|---|---|---|
| 15 | `fault` | Störungsmeldung | `EE`, E01–E29 |
| 103 | `new_fault_01` | Fehlercodetabelle 1 | E01–E30 |
| 107 | `new_fault_02` | Fehlercodetabelle 2 | E31–E43 |
| 118 | `fault_2` | Störungsmeldung 2 | E30–E59 |
| 119 | `fault_3` | Störungsmeldung 3 | E60–E88 |
| 110 | `new_driver_fault_01` | Treiberfehler-Tabelle 1 | F01–F30 |
| 111 | `new_driver_fault_02` | Treiberfehler-Tabelle 2 | F31–F48 |
| 120 | `driver_fault_1` | Treiberfehler | D17–D46 |

> Hinweis: Die Tuya-Tabellen überlappen sich teils (z. B. `E30` in `new_fault_01` und
> `fault_2`). Welche Tabelle(n) diese Pumpe tatsächlich bedient, ist nur für
> `new_fault_01` (Reg 26/27) belegt. Im Code ist jede Tabelle als eigener
> `faultRegisters[]`-Eintrag vorbereitet; nur belegte Adressen werden gelesen.

---

## 6. Semantisches `fault_main`-Bitmap (vorbereitet, unbestätigt)

Alternatives Benennungsschema (benanntes Public-Bitmap). Diese Bits liefern direkt ein
sprechendes Label statt einer Nummer (`labels[]` statt `code_prefix`). Auf dieser Pumpe
**bisher nicht nachgewiesen** (Adresse `TODO`) — der Flow-Fehler kam als `E17`, nicht hier.

| Bit | Label | Bedeutung |
|---|---|---|
| 0 | `sys_high_fault` | Hochdruck |
| 1 | `sys_low_fault` | Niederdruck |
| 2 | `flow_fault` | Wasserdurchfluss / Strömungswächter |
| 3 | `power_fault` | Spannungs-/Phasenfehler |
| 4 | `cooling_fault` | Kühlfehler |
| 5 | `heating_fault` | Heizfehler |
| 6 | `temp_dif_fault` | zu große Temperaturdifferenz |
| 7 | `in_temp_fault` | Wasser-Eingangstemp-Sensor |
| 8 | `eff_temp_fault` | Wasser-Ausgangstemp-Sensor |
| 9 | `coil_temp_fault` | Verdampfer-/Spulen-Sensor |
| 10 | `ret_temp_fault` | Rückgastemp-Sensor |
| 11 | `news_fault` | (unklar) |
| 12 | `amb_temp_fault` | Umgebungstemp-Sensor |
| 13 | `lack_water` | Wassermangel |
| 14 | `serious_fault` | schwerwiegender Fehler |
| 15 | `sensor_fault` | allgemeiner Sensorfehler |
| 16 | `motor_fault` | Motorfehler (= Bit 0 des Folgeregisters `fault_main_hi`) |

---

## 7. Offene Punkte

- Adressen der übrigen Tabellen (`new_fault_02`, `fault_2/3`, F-/D-Treiberfehler,
  semantisches `fault_main`) per weiterer Dump-Diffs lokalisieren und in
  `src/modbus_faults.h` statt `FAULT_ADDR_TODO` eintragen.
- Textliche Bedeutung weiterer E-Codes (außer `E17`) am Gerät verifizieren.
