#ifndef SETUPWEBSERVER_H
#define SETUPWEBSERVER_H

#include <ESPAsyncWebServer.h>

// Bewusst NICHT setupWifiManager.h einbinden: das zieht WiFiManager -> (synchrone) WebServer.h ->
// http_parser.h herein, dessen HTTP_GET/HTTP_POST-Enum mit ESPAsyncWebServers AsyncWebRequestMethod
// kollidiert (mehrdeutig). setupWebserver.cpp forward-deklariert setupWifiManager() stattdessen.
#include "log.h"

// loopWebserver() bedient NICHT mehr den Webserver (der laeuft jetzt asynchron im AsyncTCP-Task) —
// es fuehrt nur noch aufgeschobene Aktionen aus (Reboot / Reconfigure), die nicht im AsyncTCP-
// Handler laufen duerfen. Wird weiter jede Loop-Iteration aufgerufen.
void loopWebserver();
void setupWebserver();
void restartWebserver();

#endif // SETUPWEBSERVER_H
