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
#define RXD 16 // aka D16
#define TXD 17 // aka D17
#elif defined(ARDUINO_ARCH_ESP8266)
#define RXD 14 // aka D5
#define TXD 12 // aka D6
#endif
#define RTS NOT_A_PIN

#define MODBUS_BAUDRATE 9600
#define MODBUS_TIMEOUT 1000
#define MODBUS_UNIT 1
#define MODBUS_RETRIES 2
// Bei Buskollisionen mit dem Tuya-Master (Invalid Slave ID / Invalid CRC / Timeout)
// deutlich mehr Versuche, bis ein Fenster ohne Fremdverkehr erwischt wird.
// Hoch lassen, auch bei stillgelegtem WBR3: das Display selbst pollt weiter gelegentlich
// über A/B und verursacht seltene Restkollisionen. Praxis zeigt: damit laeuft es stabil.
#define MODBUS_RETRIES_BUS_COLLISION 30

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
