#ifndef SETUPWIFIMANAGER_H_
#define SETUPWIFIMANAGER_H_

#include <LittleFS.h>                   //this needs to be first, or it all crashes and burns...
#include <WiFiManager.h>          //https://github.com/tzapu/WiFiManager


#include <ArduinoJson.h>          //https://github.com/bblanchon/ArduinoJson

extern char param_mqtt_server[40];
extern char param_mqtt_port[6];
extern char param_firmware_url[255];
extern char param_mqtt_topic[50];

void setupWifiManager(bool forceConfigPortal);

#endif