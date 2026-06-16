#ifndef SRC_MODBUS_BASE_H_
#define SRC_MODBUS_BASE_H_



#include <ModbusMaster.h>
#include <ArduinoJson.h>
#ifndef ARDUINO_ARCH_ESP32
#include <SoftwareSerial.h>
#endif
#include "modbus_registers.h"
#include "log.h"
#include "Arduino.h"


// TODO: add this to WifiManager
#if defined(ARDUINO_ARCH_ESP32)
// RX/TX getauscht (2026-06-14): nach Wechsel HW-519 -> MAX3485-Modul kamen keine Daten;
// dessen RXD/TXD-Silkscreen ist aus Modul-Sicht beschriftet -> Pins gegenueber vorher vertauscht.
#define RXD 17 // aka D17
#define TXD 16 // aka D16
#elif defined(ARDUINO_ARCH_ESP8266)
#define RXD 14 // aka D5
#define TXD 12 // aka D6
#endif
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


void preTransmission();
void postTransmission();
void initModbus();
bool writeModbusRegister(const char *register_name, uint16_t value);
bool fillRegisterValues();
void writeRegisterValuesToJson(ArduinoJson::JsonVariant variant);
String getModbusState();
bool readHoldingRange(uint16_t start_id, uint16_t count, uint16_t *values, bool *valid);
void writeFaultStatusToJson(ArduinoJson::JsonVariant variant);
#endif // SRC_MODBUS_BASE_H_
