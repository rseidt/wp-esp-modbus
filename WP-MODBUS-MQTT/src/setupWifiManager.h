#ifndef SETUPWIFIMANAGER_H_
#define SETUPWIFIMANAGER_H_

#include <LittleFS.h>
#include <WiFiManager.h> //https://github.com/tzapu/WiFiManager

#include <ArduinoJson.h> //https://github.com/bblanchon/ArduinoJson

extern char param_mqtt_server[40];
extern char param_mqtt_port[6];
extern char param_mqtt_topic[50];

void setupWifiManager(bool forceConfigPortal);

#endif
