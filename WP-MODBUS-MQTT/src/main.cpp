/*
 main.cpp - esp-modbus-mqtt
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

#include "main.h"
#include "setupWifiManager.h"
#include "setupReconfigureServer.h"


const int MODBUS_SCANRATE = 2;  // in seconds


static char HOSTNAME[24] = "ESP-MM-FFFFFFFFFFFFFFFF";
static const char __attribute__((__unused__)) *TAG = "Main";

// static const char *FIRMWARE_URL = "https://domain.com/path/file.bin";
static const char *FIRMWARE_VERSION = "000.000.024";

// instanciate AsyncMqttClient object
AsyncMqttClient mqtt_client;

// instanciate timers
Timer<1,millis> mqtt_reconnect_timer;
Timer<1,millis> wifi_reconnect_timer;
Timer<1,millis> modbus_poller_timer;
Timer<1,millis> memory_report_timer;
bool modbus_poller_inprogress = false;
bool wifiConnected = false;
bool mqttConnected = false;

int freeHeap;

void startModbusPoller()
{
  modbus_poller_timer.every(2000, runModbusPollerTask);
}

void stopModbusPoller(){
  modbus_poller_timer.cancel();
  modbus_poller_inprogress = false;
}

void startWifiConnectTimer()
{
  wifi_reconnect_timer.every(2000, connectToWifi);
}

void stopWifiConnectTimer(){
  wifi_reconnect_timer.cancel();
}

void startMemoryReportTimer()
{
  memory_report_timer.every(20000, [](void *) {
    int freeHeap = ESP.getFreeHeap();
    log(LOG_LEVEL_INFO, "Free heap: " + String(freeHeap) + " bytes");
    struct tm now;
    if (getLocalTime(&now)) {
      log(LOG_LEVEL_INFO, "Time: "+String(now.tm_year+1900)+"-"+ String(now.tm_mon+1) +"-" +String(now.tm_mday)+ " " + String(now.tm_hour) + ":"+ String(now.tm_min) +":" + String(now.tm_sec));
    } else {
      log(LOG_LEVEL_ERROR, "Failed to obtain time");
    }
    String json = "{";
    json += "\"freeHeap\":"+String(freeHeap) + ",";
    json += "\"uptime\":"+String(millis()/1000)+ "," ;
    json += "\"time\":\""+String(now.tm_year+1900)+"-"+ String(now.tm_mon+1) +"-" +String(now.tm_mday)+ " " + String(now.tm_hour) + ":"+ String(now.tm_min) +":" + String(now.tm_sec)+"\"";
    json += "}";
    if (mqtt_client.connected()) {
      String mqtt_complete_topic = param_mqtt_topic;
      mqtt_complete_topic += "/" + String(HOSTNAME) + "/status";
      log(LOG_LEVEL_WARNING, "MQTT Publishing data to topic " + mqtt_complete_topic + ": " + json);
      mqtt_client.publish(mqtt_complete_topic.c_str(), 0, true, json.c_str(), json.length());
    }
    return true; // repeat the task
  });
}
void stopMemoryReportTimer(){
  memory_report_timer.cancel();
}

void startMqttConnectTimer()
{
  mqtt_reconnect_timer.every(2000, connectToMqtt);
}

void stopMqttConnectTimer(){
  mqtt_reconnect_timer.cancel();
}


void resetWiFi() {
  log(LOG_LEVEL_INFO, "Resetting Wifi");
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  wifiConnected=false;
}

bool connectToWifi(void * pvParameters) {
  if (!WiFi.isConnected())
  {
    log(LOG_LEVEL_INFO, "Connecting to WiFi");
    wl_status_t state = WiFi.begin();
  }
  return true;
}

bool connectToMqtt(void * pvParameters) {
  if (!mqtt_client.connected()) {
    log(LOG_LEVEL_INFO, "Connecting to MQTT");
    mqtt_client.connect();
  }
  return true;
}

void wiFiEvent(WiFiEvent_t event) {
  log(LOG_LEVEL_INFO, "WiFi event: " + event);
  switch (event) {
    case WIFI_EVENT_STAMODE_GOT_IP:
      log(LOG_LEVEL_INFO, "WiFi connected with IP address: " + WiFi.localIP().toString());
      if (!MDNS.begin(HOSTNAME)) {  // init mdns
        log(LOG_LEVEL_WARNING, "Error setting up MDNS responder");
      }
      configTime(0, 0, "pool.ntp.org");  // init UTC time
      struct tm now;
      if (getLocalTime(&now)) {
        log(LOG_LEVEL_INFO, "Time: "+String(now.tm_year+1900)+"-"+ String(now.tm_mon+1) +"-" +String(now.tm_mday)+ " " + String(now.tm_hour) + ":"+ String(now.tm_min) +":" + String(now.tm_sec));
      } else {
        log(LOG_LEVEL_ERROR, "Failed to obtain time");
      }
      startMqttConnectTimer();
      break;
    case WIFI_EVENT_STAMODE_DISCONNECTED:
      log(LOG_LEVEL_INFO, "WiFi lost connection");
      stopMqttConnectTimer();  // ensure we don't reconnect to MQTT while reconnecting to Wi-Fi
      startWifiConnectTimer();
      break;
    default:
      break;
  }
}

void onMqttConnect(bool sessionPresent) {
  log(LOG_LEVEL_INFO, "Connected to MQTT");
  log(LOG_LEVEL_INFO, "Session present: " + sessionPresent ? "true" : "false");
  stopMqttConnectTimer();

  String mqtt_complete_topic = param_mqtt_topic;
  mqtt_complete_topic += "/" + String(HOSTNAME);
  log(LOG_LEVEL_INFO, "Subscribing to " + mqtt_complete_topic + "/action/#");
  mqtt_client.subscribe(String(mqtt_complete_topic + "/action/#").c_str(), 1);
  mqtt_client.publish(String(mqtt_complete_topic + "/status").c_str(), 1, true, "mqtt_connected");
  log(LOG_LEVEL_INFO, "Published online status to " + mqtt_complete_topic + "/status");
}

void onMqttDisconnect(AsyncMqttClientDisconnectReason reason) {
  log(LOG_LEVEL_WARNING, "Disconnected from MQTT: " + String((int)reason));

  if (WiFi.isConnected()) {
    startMqttConnectTimer();
  }
}

void onMqttSubscribe(uint16_t packetId, uint8_t qos) {
  log(LOG_LEVEL_INFO, "Subscribe acknowledged for packetId: "+String(packetId)+" qos: " +String(qos));
}

void onMqttUnsubscribe(uint16_t packetId) {
  log(LOG_LEVEL_INFO, "Unsubscribe acknowledged for packetId: " + String(packetId));
}

void onMqttMessage(char *topic, char *payload,
  AsyncMqttClientMessageProperties properties, size_t len, size_t index, size_t total) {
  log(LOG_LEVEL_INFO, "Message received (topic="+String(topic)+", qos="+String(properties.qos)+", dup="+String(properties.dup)+", retain="+String(properties.retain)+", len="+String(len)+", index="+String(index)+", total="+String(total)+"): " + String(payload));

  String suffix = String(topic).substring(strlen(param_mqtt_topic) + strlen(HOSTNAME) + 9);
          // substring(1 + strlen(MQTT_TOPIC) + strlen("/") + strlen(HOSTNAME) + strlen("/") + strlen("action"))
  log(LOG_LEVEL_INFO, "MQTT topic suffix=" + String(suffix.c_str()));

  if (suffix == "upgrade") {
    log(LOG_LEVEL_INFO, "MQTT OTA update requested");
    runOtaUpdateTask();
    return;
  } else if (suffix == "write_register") {
    log(LOG_LEVEL_INFO, "MQTT Write modbus register requested");
    stopModbusPoller();
    String payload_s = String(payload);
    String register_name = payload_s.substring(0, payload_s.indexOf('='));
    String register_value = payload_s.substring(payload_s.indexOf('=') + 1);
    log(LOG_LEVEL_INFO, "Writing register name="+String(register_name)+" with value="+String(register_value));
    
    if (writeModbusRegister(register_name.c_str(), register_value.toInt())) {
      log(LOG_LEVEL_INFO, "Register written successfully");
    } else {
      log(LOG_LEVEL_ERROR, "Failed to write register " + String(register_name) + " with value \"" + String(register_value) + "\"");
    }
    startModbusPoller();
    return;
  } else  {
    log(LOG_LEVEL_INFO, "Unknown MQTT topic received: "+String(topic));
  }
}

void onMqttPublish(uint16_t packetId) {
  log(LOG_LEVEL_INFO, "Publish acknowledged for packetId: " + String(packetId));
}

void runOtaUpdateTask() {
  log(LOG_LEVEL_INFO, "Entering OTA task.");

    log(LOG_LEVEL_INFO, "Checking if new firmware is available");
    if (checkFirmwareUpdate(param_firmware_url, FIRMWARE_VERSION)) {
      log(LOG_LEVEL_WARNING, "New firmware found");
      log(LOG_LEVEL_INFO, "Suspending modbus poller");
      modbus_poller_timer.cancel();
      modbus_poller_inprogress = false;

      if (updateOTA(param_firmware_url)) {
        // Update is done. Rebooting...
        log(LOG_LEVEL_INFO, "Rebooting.");
        log(LOG_LEVEL_INFO, "************************ REBOOT IN PROGRESS *************************");
        ESP.restart();
      } else {
        log(LOG_LEVEL_ERROR, "OTA update failed. Restarting Modbus Poller");
// TODO(gmasse): retry?
        startModbusPoller();
      }
    }
  }

bool runModbusPollerTask(void * pvParameters) {
#ifndef MODBUS_DISABLED
  log(LOG_LEVEL_INFO, "Entering Modbus Poller task. ");

    if (modbus_poller_inprogress) {  // not sure if needed
      log(LOG_LEVEL_WARNING, "Modbus polling in already progress. Waiting for next cycle.");
      return true;
    }
    modbus_poller_inprogress = true;

    JsonDocument json_doc;  // instanciate JSON storage
    parseModbusToJson(json_doc);
    size_t json_size = measureJson(json_doc) +1;
    char *buffer = (char*)malloc(json_size * sizeof(char));
    size_t n = serializeJson(json_doc, buffer, json_size);
    log(LOG_LEVEL_INFO, "JSON serialized: " + String(buffer));
    if (mqtt_client.connected()) {
      String mqtt_complete_topic = param_mqtt_topic;
      mqtt_complete_topic += "/" + String(HOSTNAME) + "/data";
      log(LOG_LEVEL_INFO, "MQTT Publishing data to topic: " + String(mqtt_complete_topic.c_str()));
      mqtt_client.publish(mqtt_complete_topic.c_str(), 0, true, buffer, n);
    }
    free(buffer);
    modbus_poller_inprogress = false;
#endif  // MODBUS_DISABLED
return true;
}


void setup() {
  // debug comm
  Serial.begin(74880);
  //while (!Serial) continue;
  delay(500);
  //Serial.setDebugOutput(true);
  log(LOG_LEVEL_INFO, "Serial started at 74880 baud");

  setupWifiManager(false);
  setupReconfigureServer();



  snprintf(HOSTNAME, sizeof(HOSTNAME), "ESP-MM-%i", ESP.getChipId());  // setting hostname

  log(LOG_LEVEL_INFO, "*********************************************************************");
  log(LOG_LEVEL_INFO, "Firmware version "+String(FIRMWARE_VERSION)+" (compiled at "+__DATE__+" "+__TIME__+")");
  log(LOG_LEVEL_INFO, "Hostname: " +String(HOSTNAME));

  WiFi.onEvent(wiFiEvent);

  mqtt_client.onConnect(onMqttConnect);
  mqtt_client.onDisconnect(onMqttDisconnect);
  mqtt_client.onSubscribe(onMqttSubscribe);
  mqtt_client.onUnsubscribe(onMqttUnsubscribe);
  mqtt_client.onMessage(onMqttMessage);
  mqtt_client.onPublish(onMqttPublish);

  mqtt_client.setServer(param_mqtt_server, std::stoi(param_mqtt_port));

  startMqttConnectTimer();
  startWifiConnectTimer();
  startMemoryReportTimer();

#ifndef MODBUS_DISABLED
  initModbus();
  startModbusPoller();
#endif  // MODBUS_DISABLED


}
int i = 0;
void loop() {
  loopReconfigureServer();
  //log(4, "Entering main loop : "+String(i++));
  wifi_reconnect_timer.tick();
  mqtt_reconnect_timer.tick();
  modbus_poller_timer.tick();
  memory_report_timer.tick();
}
