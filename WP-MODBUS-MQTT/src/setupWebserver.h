#ifndef SETUPWEBSERVER_H
#define SETUPWEBSERVER_H

#if defined(ARDUINO_ARCH_ESP32)
#include "WebServer.h"
#elif defined(ARDUINO_ARCH_ESP8266)
#include <ESP8266WebServer.h>
#endif

#include "setupWifiManager.h"
#include "log.h"

void loopWebserver();
void setupWebserver();

#endif // SETUPWEBSERVER_H
