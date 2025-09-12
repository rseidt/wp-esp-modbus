/*
 esp_base.cpp - ESP core functions
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

#include "esp_base.h"

#include "Arduino.h"

#include "log.h"

#include <WiFiClientSecure.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266httpUpdate.h>

#include <Url.h>

static const char *VERSION_HEADER = "X-Object-Meta-Version";

static const char __attribute__((__unused__)) *TAG = "ESP_base";


bool _httpRequest(HTTPClient *http_client, const String& url_s) {
  
  WiFiClient client;
  
  bool ret = false;
  Url url(url_s);
  if (url.Protocol == "http") {
    const uint16_t port = (url.Port.isEmpty() ? 80 : url.Port.toInt());
    ret = http_client->begin(client, url.Host, port, url.Path);
  } else {
    log(LOG_LEVEL_ERROR, "Unsupported protocol: " + String(url.Protocol));
    return false;
  }

  if (ret) {
    int httpCode = http_client->GET();

    // httpCode will be negative on error
    if (httpCode > 0) {
      // HTTP header has been send and Server response header has been handled
      log(LOG_LEVEL_INFO, "HTTP GET... code:" + String(httpCode));

      // file found at server
      if (httpCode == HTTP_CODE_OK) {
        return true;
      } else if (httpCode == HTTP_CODE_MOVED_PERMANENTLY || httpCode == HTTP_CODE_FOUND
              || httpCode == HTTP_CODE_TEMPORARY_REDIRECT || httpCode == HTTP_CODE_PERMANENT_REDIRECT ) {
        log(LOG_LEVEL_INFO, "HTTP redirection not supported (yet): "+String(httpCode));
      } else {
        log(LOG_LEVEL_ERROR, "Unsupported HTTP response code: "+String(httpCode));
      }
    } else {
      log(LOG_LEVEL_ERROR, "HTTP GET... failed, error: " + String(http_client->errorToString(httpCode)));
    }
  }
  return false;
}

String _requestFirmwareVersion(const String &url_s) {
  log(LOG_LEVEL_INFO, "Requesting firmware from url=" + String(url_s));
  String firmware_version = "";
  HTTPClient client;
  const char* headers_list[] = { VERSION_HEADER, "Content-Type" };
  client.collectHeaders(headers_list, sizeof(headers_list)/sizeof(headers_list[0]));
  if (_httpRequest(&client, url_s)) {
    log(LOG_LEVEL_INFO, "Header "+String(VERSION_HEADER)+"="+String(client.header(VERSION_HEADER)));
    log(LOG_LEVEL_INFO, "Header Content-Type=" + String(client.header("Content-Type")));
    if (client.header("Content-Type") == "application/octet-stream") {
      firmware_version = client.header(VERSION_HEADER);
    } else {
      log(LOG_LEVEL_ERROR, "Invalid Content-Type: " + String(client.header("Content-Type")));
    }
  }
  client.end();
  return firmware_version;
}

bool checkFirmwareUpdate(const String& url_s, const String& current_version) {
  log(LOG_LEVEL_INFO, "Checking firmware version="+String(current_version)+" from url=" + String(url_s));

  const String remote_version = _requestFirmwareVersion(url_s);
  if (remote_version.isEmpty()) {
    log(LOG_LEVEL_WARNING, "Remote firmware not found");
  } else if (remote_version > current_version) {
    log(LOG_LEVEL_WARNING, "New firmware version detected: " + String(remote_version));
    return true;
  } else {
    log(LOG_LEVEL_INFO, "Firmware remote version: " + String(remote_version));
    log(LOG_LEVEL_INFO, "Firmware is already up to date (version " + String(current_version) + ")");
  }
  return false;
}

bool updateOTA(const String& url_s) {
  HTTPClient client;
  if (_httpRequest(&client, url_s)) {
    int len = client.getSize();
    WiFiClient *tcp = client.getStreamPtr();

    // check whether we have everything for OTA update
    if (len) {
      if (Update.begin(len)) {
        log(LOG_LEVEL_WARNING, "Starting Over-The-Air update. This may take some time to complete ...");
        size_t written = Update.writeStream(*tcp);

        if (written == len) {
          log(LOG_LEVEL_WARNING, "Written: " + String(written) + " successfully");
        } else {
          log(LOG_LEVEL_ERROR, "Written only: " + String(written) + "/" + String(len) + ". Retry?");
        }

        if (Update.end()) {
          if (Update.isFinished()) {
            log(LOG_LEVEL_WARNING, "OTA update has successfully completed. Reboot needed...");
            client.end();
            return true;
          } else {
            log(LOG_LEVEL_ERROR, "Something went wrong! OTA update hasn't been finished properly.");
          }
        } else {
          log(LOG_LEVEL_ERROR, "An error Occurred. Error #: " + String(Update.getError()));
        }
      } else {
        log(LOG_LEVEL_ERROR, "There isn't enough space to start OTA update");
      }
    } else {
      log(LOG_LEVEL_ERROR, "Invalid content-length received from server");
    }
  } else {
    log(LOG_LEVEL_ERROR, "Unable to connect to server");
  }
  client.end();
  return false;
}
