/*
 main.h - esp-modbus-mqtt
 Copyright (C) 2020 Germain Masse

 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#ifndef SRC_MAIN_H_
#define SRC_MAIN_H_

//#define MODBUS_DISABLED  // comment this line to enable Modbus functions

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
#include "esp_base.h"
#ifndef MODBUS_DISABLED
#include <modbus_base.h>
#endif
void runOtaUpdateTask();
bool runModbusPollerTask(void * pvParameters);
bool connectToWifi(void * pvParameters);
bool connectToMqtt(void * pvParameters);

#endif  // SRC_MAIN_H_