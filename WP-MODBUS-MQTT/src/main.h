#ifndef SRC_MAIN_H_
#define SRC_MAIN_H_

// #define MODBUS_DISABLED  // comment this line to enable Modbus functions

#include <math.h>
#if defined(ARDUINO_ARCH_ESP32)
#include <WiFi.h>
#include <ESPmDNS.h>
// #include "esp32-hal-log.h"
#elif defined(ARDUINO_ARCH_ESP8266)
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#endif

#include <ArduinoJson.h>
#include <AsyncMqttClient.h>
#include <WiFiManager.h>
#include <arduino-timer.h>
#include <Url.h>
#include "log.h"
#include "setupWebserver.h"
#include "setupWifiManager.h"

#ifndef MODBUS_DISABLED
#include <modbus_base.h>
#endif
void runOtaUpdateTask();
bool runModbusPollerTask(void *pvParameters);
bool connectToWifi(void *pvParameters);
bool connectToMqtt(void *pvParameters);
bool reportMemoryStatus(void *pvParameters);

#endif // SRC_MAIN_H_
