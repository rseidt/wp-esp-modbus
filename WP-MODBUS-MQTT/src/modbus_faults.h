#ifndef SRC_MODBUS_FAULTS_H_
#define SRC_MODBUS_FAULTS_H_

#include "Arduino.h"

// ============================================================================
// Geraete-Fehlercodes der Pumpe (Tuya-Fault-DPs), vorbereitet zum Dekodieren.
//
// Stand: Die BIT-STRUKTUR ist bekannt (sequenziell, Bit N = N-ter Code). Die
// MODBUS-ADRESSEN der numerischen E-Code-Tabelle new_fault_01 sind per Dump-Diff
// BESTAETIGT (Reg 26 = lo, Reg 27 = hi, siehe unten). Alle uebrigen Tabellen
// (new_fault_02, fault_2/3, Treiberfehler, semantisches fault_main) sind mangels
// provozierbarer Fehler NOCH NICHT lokalisiert und stehen auf FAULT_ADDR_TODO —
// Eintraege werden beim Decode uebersprungen, bis ein weiterer Diff die Adresse zeigt.
//
// Quellen: tuya-local Issue #1712 (product_id k5vqutj2llzox1gg, "海外泳池机公版")
// und das benannte Public-Fault-Bitmap (sys_high/sys_low/flow_fault ...).
//
// Praktischer Befund: Der "Flow Control"-Fehler (Wasserpumpe aus) erscheint auf
// DIESER Pumpe NICHT als flow_fault-Bit des semantischen fault_main-Bitmaps,
// sondern als numerischer Code E17 in der Tabelle new_fault_01 (Reg 27, Bit 0).
// Das semantische fault_main-Bitmap bleibt darum unbelegt (kein Adress-Nachweis).
// ============================================================================

// Sentinel: Adresse noch nicht identifiziert -> Eintrag wird beim Decode uebersprungen.
// 0xFFFF liegt ausserhalb des genutzten Registerbereichs (0..200).
#define FAULT_ADDR_TODO 0xFFFF

typedef struct
{
	uint16_t modbus_addr;	 // echte Holding-Register-Adresse oder FAULT_ADDR_TODO
	const char *dp_name;	 // Quelle/Beschreibung (auch Schluessel im MQTT-"raw"-Objekt)
	char code_prefix;		 // 'E'/'F'/'D' fuer generierte Codes; 0 wenn explizite labels[]
	uint8_t first_code;		 // Codenummer von Bit 0 (nur bei generierten Codes)
	uint8_t bit_count;		 // wie viele Bits dieses Registers gueltig sind (<= 16)
	const char *labels[16];	 // explizite Labels Bit0..Bit15; nullptr -> Code generieren
} fault_register_t;

// Hinweis zu mehr-als-16-Bit-DPs: pro 16-Bit-Modbus-Register ein Eintrag (lo/hi).
// Beim Eintragen der Adressen sind lo- und hi-Wort i.d.R. aufeinanderfolgende Register.
const fault_register_t faultRegisters[] = {
	// --- Haupt-Stoerung (benanntes Public-Bitmap) -- enthaelt flow_fault (Bit 2) ---
	{FAULT_ADDR_TODO, "fault_main", 0, 0, 16, {
		"sys_high_fault",  // Bit 0: Hochdruck
		"sys_low_fault",   // Bit 1: Niederdruck
		"flow_fault",      // Bit 2: Wasserdurchfluss / Stroemungswaechter  <-- "Flow Control"
		"power_fault",     // Bit 3: Spannungs-/Phasenfehler
		"cooling_fault",   // Bit 4
		"heating_fault",   // Bit 5
		"temp_dif_fault",  // Bit 6: zu grosse Temperaturdifferenz
		"in_temp_fault",   // Bit 7: Wasser-Eingangstemp-Sensor
		"eff_temp_fault",  // Bit 8: Wasser-Ausgangstemp-Sensor
		"coil_temp_fault", // Bit 9: Verdampfer-/Spulen-Sensor
		"ret_temp_fault",  // Bit 10: Rueckgastemp-Sensor
		"news_fault",      // Bit 11
		"amb_temp_fault",  // Bit 12: Umgebungstemp-Sensor
		"lack_water",      // Bit 13: Wassermangel
		"serious_fault",   // Bit 14: schwerwiegender Fehler
		"sensor_fault"     // Bit 15: allgemeiner Sensorfehler
	}},
	{FAULT_ADDR_TODO, "fault_main_hi", 0, 0, 1, {
		"motor_fault" // Bit 16 (= Bit 0 des Folgeregisters)
	}},

	// --- Numerische Fehlertabellen (E-Codes), Bit N = E(first_code+N) ---
	// BESTAETIGT per Dump-Diff (clean vs. provozierter Flow-Fehler): Reg 26 = lo, Reg 27 = hi.
	// Im Fehlerfall ("Flow Control", Wasserpumpe aus) zeigt das Display E17 -> Reg 27 == 1 (Bit 0,
	// first_code 17 -> E17). Damit ist die DP103->Register-Abbildung dieser Pumpe eindeutig.
	{26, "new_fault_01_lo", 'E', 1, 16, {nullptr}},  // DP103: E01..E16
	{27, "new_fault_01_hi", 'E', 17, 14, {nullptr}}, // DP103: E17..E30
	{FAULT_ADDR_TODO, "new_fault_02", 'E', 31, 13, {nullptr}},    // DP107: E31..E43
	{FAULT_ADDR_TODO, "fault_2_lo", 'E', 30, 16, {nullptr}},      // DP118: E30..E45
	{FAULT_ADDR_TODO, "fault_2_hi", 'E', 46, 14, {nullptr}},      // DP118: E46..E59
	{FAULT_ADDR_TODO, "fault_3_lo", 'E', 60, 16, {nullptr}},      // DP119: E60..E75
	{FAULT_ADDR_TODO, "fault_3_hi", 'E', 76, 13, {nullptr}},      // DP119: E76..E88

	// --- Treiber-/Inverter-Fehler (F-Codes) ---
	{FAULT_ADDR_TODO, "driver_fault_01_lo", 'F', 1, 16, {nullptr}},  // DP110: F01..F16
	{FAULT_ADDR_TODO, "driver_fault_01_hi", 'F', 17, 14, {nullptr}}, // DP110: F17..F30
	{FAULT_ADDR_TODO, "driver_fault_02_lo", 'F', 31, 16, {nullptr}}, // DP111: F31..F46
	{FAULT_ADDR_TODO, "driver_fault_02_hi", 'F', 47, 2, {nullptr}},  // DP111: F47..F48

	// --- Treiberfehler (D-Codes) ---
	{FAULT_ADDR_TODO, "driver_fault_1_lo", 'D', 17, 16, {nullptr}}, // DP120: D17..D32
	{FAULT_ADDR_TODO, "driver_fault_1_hi", 'D', 33, 14, {nullptr}}, // DP120: D33..D46
};

#endif // SRC_MODBUS_FAULTS_H_
