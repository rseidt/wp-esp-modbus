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
#define MODBUS_UNIT 1
#define MODBUS_RETRIES 2


void preTransmission();
void postTransmission();
void initModbus();
bool writeModbusRegister(const char *register_name, uint16_t value);
bool fillRegisterValues();
void writeRegisterValuesToJson(ArduinoJson::JsonVariant variant);
#endif // SRC_MODBUS_BASE_H_
