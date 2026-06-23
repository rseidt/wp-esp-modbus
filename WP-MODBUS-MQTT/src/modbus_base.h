#ifndef SRC_MODBUS_BASE_H_
#define SRC_MODBUS_BASE_H_



#include <ModbusMaster.h>
#include <ArduinoJson.h>
#include "modbus_registers.h"
#include "log.h"
#include "Arduino.h"


// TODO: add this to WifiManager
// RX/TX getauscht (2026-06-14): nach Wechsel HW-519 -> MAX3485-Modul kamen keine Daten;
// dessen RXD/TXD-Silkscreen ist aus Modul-Sicht beschriftet -> Pins gegenueber vorher vertauscht.
#define RXD 17 // aka D17
#define TXD 16 // aka D16
// Richtungssteuerung des MAX3485-Moduls ("RS485 V2.0", EN-Pin = DE+/RE zusammengelegt)
// an GPIO 22. preTransmission()=HIGH (senden), postTransmission()=LOW (empfangen);
// Init auf Empfang in initModbus(). Ersetzt das alte HW-519-Auto-Direction-Modul, dessen
// RC-Nachlaufzeit den Antwortanfang verschluckte (~50% "Response Timed Out").
// NICHT auf Strapping-Pins (0/2/12/15) oder 34-39 legen.
// HARDWARE: 10 kOhm Pulldown von EN nach GND vorsehen! Zwischen Reset und initModbus()
// floatet GPIO 22 — ohne Pulldown koennte das Modul beim Boot kurz in den Sendemodus
// gehen und den Bus treiben. Der Pulldown haelt EN bis zur Firmware-Init auf Empfang.
#define RTS 22

#define MODBUS_BAUDRATE 9600
#define MODBUS_TIMEOUT 1000
#define MODBUS_UNIT 1
#define MODBUS_RETRIES 2
// Inter-Transaktions-Abstand vor einem per MQTT injizierten Write (wie Poll-Tick): ein Write
// kann direkt nach einer Poll-Transaktion kommen, dieser Slave verschluckt dann die zu dicht
// folgende Transaktion -> der 1. Versuch lief sonst in den ~2 s-Timeout.
#define MODBUS_TX_SPACING_MS 500
// Bei Buskollisionen mit dem Tuya-Master (Invalid Slave ID / Invalid CRC / Timeout)
// deutlich mehr Versuche, bis ein Fenster ohne Fremdverkehr erwischt wird.
// Hoch lassen, auch bei stillgelegtem WBR3: das Display selbst pollt weiter gelegentlich
// über A/B und verursacht seltene Restkollisionen. Praxis zeigt: damit laeuft es stabil.
// Gilt NUR fuer den Read-Poller (fillRegisterValues): dort ist jeder Versuch ein eigener
// Poll-Tick (1 Transaktion/Aufruf, currentTryIndex ueber Ticks) -> nie eine lange
// CPU-Blockade, fuer den Watchdog harmlos.
#define MODBUS_RETRIES_BUS_COLLISION 30
// Eigenes, KLEINES Budget fuer den per MQTT injizierten Write (writeModbusRegister): dort
// laufen die Versuche als enge for-Schleife in EINEM Aufruf, jeder writeSingleRegister
// blockiert ~1 Timeout per Busy-Wait. Mit 30 Versuchen ergab das ~30-60 s CPU-Blockade ohne
// yield -> IDLE-Task verhungert -> Task-Watchdog-Reset (Crash 2026-06-16). 6 Versuche bremst
// das hart; zusaetzlich yieldet writeModbusRegister zwischen den Versuchen (siehe dort).
#define MODBUS_WRITE_RETRIES_BUS_COLLISION 6

// Block-Read fuer den Webserver-Registerdump.
#define MODBUS_DUMP_CHUNK 50   // Register pro Block-Transaktion (<= ku8MaxBufferSize = 64)
#define MODBUS_DUMP_RETRIES 2  // Wiederholungen pro Chunk nur bei transientem (Kollisions-)Fehler

// Bus-Timing des Worker-Tasks (frueher in main.cpp). Der Worker liest eine Range pro Iteration
// und legt zwischen den Transaktionen MODBUS_POLL_INTERVAL_MS Pause ein (Slave-Erholzeit, per
// Diagnose 2026-06-15 bestaetigt), zwischen vollen Poll-Zyklen MODBUS_SCANRATE_MS.
#define MODBUS_POLL_INTERVAL_MS 500
#define MODBUS_SCANRATE_MS 1000

// Loop-Heartbeat-Waechter: der Loop-Task ruft feedLoopHeartbeat() jede Iteration. Der Worker-Task
// (laeuft unabhaengig auf Core 0 weiter, auch wenn der Loop haengt) rebootet den ESP, falls der
// Heartbeat laenger als LOOP_HEARTBEAT_TIMEOUT_MS ausbleibt. Faengt ein Einfrieren des Loop-Tasks
// ab (z.B. blockierendes server.handleClient() oder eine lwIP-Verklemmung beim Mischen von
// synchroner WebServer-Klasse und AsyncTCP), das KEIN Watchdog erkennt: der Loop-Task ist nicht
// beim TWDT registriert und die IDLE-Task laeuft bei einem blockierenden Hang weiter -> stiller
// Freeze, bisher nur per Stromstecken behebbar (Befund 2026-06-21: Heap/WiFi gesund, Status-Publish
// aus dem Loop-Task hoerte auf -> Loop eingefroren). Schwelle deutlich > laengster legitimer
// Loop-Block: /modbusdump blockiert den Loop bis ~20 s (Semaphore-Wait im HTTP-Handler).
#define LOOP_HEARTBEAT_TIMEOUT_MS 60000


void preTransmission();
void postTransmission();
void initModbus();
bool writeModbusRegister(const char *register_name, uint16_t value);
bool fillRegisterValues();
void writeRegisterValuesToJson(ArduinoJson::JsonVariant variant);
String getModbusState();
bool readHoldingRange(uint16_t start_id, uint16_t count, uint16_t *values, bool *valid);
void writeFaultStatusToJson(ArduinoJson::JsonVariant variant);

// --- Modbus-Worker-Task (alleiniger Bus-Owner) + Request-API ---------------------------
// Genau EIN FreeRTOS-Task besitzt den RS485-Bus. Poll, Write und Web-Dump werden zu Requests,
// die hier serialisiert ausgefuehrt werden. Das beseitigt die Cross-Task-Bus-Races (Loop-Poller
// vs. AsyncTCP-Write) und holt das blockierende Busy-Wait aus dem AsyncTCP-Callback.
void startModbusWorker();
// Vom Loop-Task jede Iteration aufgerufen: aktualisiert den Heartbeat-Zeitstempel, den der
// Worker-Task ueberwacht (Selbstheilung bei eingefrorenem Loop-Task, siehe LOOP_HEARTBEAT_TIMEOUT_MS).
void feedLoopHeartbeat();
// Reiht einen Schreibbefehl ein (non-blocking, aus jedem Task — z.B. dem MQTT-Callback).
bool enqueueModbusWrite(const char *register_name, uint16_t value);

// --- Non-blocking Register-Dump fuer den asynchronen Webserver -------------------------
// Frueher blockierte modbusDump() den Aufrufer bis zu 20 s auf eine Semaphore — im AsyncTCP-
// Handler (ESPAsyncWebServer) verboten. Jetzt: requestModbusDump() reiht einen Dump ein und
// kehrt sofort zurueck; der Worker fuellt interne Puffer und setzt den Status auf MB_DUMP_DONE.
// Der Webserver pollt modbusDumpState() (Auto-Refresh-Seite) und rendert bei DONE aus den
// Accessor-Puffern, dann modbusDumpReset() -> der naechste Aufruf startet einen frischen Dump.
#define MODBUS_DUMP_MAX 201 // max. Registeranzahl pro Dump (statischer Puffer im Worker)
enum ModbusDumpState
{
	MB_DUMP_IDLE,
	MB_DUMP_RUNNING,
	MB_DUMP_DONE
};
bool requestModbusDump(uint16_t start, uint16_t count); // false, wenn schon laufend oder Queue voll
ModbusDumpState modbusDumpState();
uint16_t modbusDumpStart();
uint16_t modbusDumpCount();
const uint16_t *modbusDumpValues();
const bool *modbusDumpValid();
void modbusDumpReset(); // nach dem Rendern: Status zurueck auf IDLE
// loop() pollt das: liefert einmal true, nachdem der Worker neue Daten bereitgestellt hat
// (voller Poll-Zyklus oder bestaetigter Write) -> publishModbusData() laeuft so im Loop-Task.
bool consumeModbusPublishRequest();
// Schuetzt register_values[]/Fault-Cache: der Worker schreibt darunter, Leser (publishModbusData)
// nehmen es kurz fuer einen konsistenten Snapshot.
bool lockRegisterCache(uint32_t timeout_ms);
void unlockRegisterCache();
#endif // SRC_MODBUS_BASE_H_
