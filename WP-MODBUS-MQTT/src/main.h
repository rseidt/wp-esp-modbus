#ifndef SRC_MAIN_H_
#define SRC_MAIN_H_

// #define MODBUS_DISABLED  // comment this line to enable Modbus functions

// GPIO, das ueber den BC547-Low-Side-Switch den WBR3D-EN-Pin steuert.
// HIGH = Transistor leitet = WBR3D EN auf GND = WBR3D AUS = ESP32 alleiniger Master (MQTT).
// LOW  = Transistor sperrt = Pull-up zieht EN hoch = WBR3D AN = Steuerung via Hersteller-App.
// Kein Strapping-/Input-Only-Pin (siehe CLAUDE.md).
#define WBR3_EN_PIN 21

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
bool connectToWifi(void *pvParameters);
bool connectToMqtt(void *pvParameters);
bool reportMemoryStatus(void *pvParameters);

// Steuerungsmodus-Umschaltung (App vs. MQTT) — wird auch vom Webserver aufgerufen.
// appControl=true  -> WBR3D AN, Modbus-Poll pausiert (Hersteller-App steuert).
// appControl=false -> WBR3D AUS, Modbus-Poll aktiv (Steuerung via MQTT).
void setControlMode(bool appControl);
bool isAppControlMode();

#endif // SRC_MAIN_H_
