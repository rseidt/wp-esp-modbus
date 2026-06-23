#ifndef SRC_LOG_H_
#define SRC_LOG_H_

#define LOG_LEVEL_ERROR 1
#define LOG_LEVEL_WARNING 2
#define LOG_LEVEL_INFO 3
#define LOG_LEVEL_DEBUG 4

#define MAX_LOG_LEVEL 2 // Change this to set the maximum log level (Serial-Ausgabe)

#include "Arduino.h"
#include "modbus_registers.h"

// Persistentes Logging ins LittleFS (zusaetzlich zum Serial). Zwei Dateien: das aktuelle Log
// des laufenden Boots und das des vorherigen Boots ("pre-restart"), das beim Boot durch
// Umbenennen erhalten wird. Beide ueber die Weboberflaeche anzeigbar.
#define FILE_LOG_PATH_CURRENT  "/log.txt"
#define FILE_LOG_PATH_PREVIOUS "/log_prev.txt"
#define FILE_LOG_PATH_TMP      "/log.tmp"
// Crash-Log: NICHT rotiert. Beim Boot nach einem Absturz (Panic/Watchdog/Brownout) wird die
// abgestuerzte Session hierher kopiert, damit der Crash-Kontext NICHT vom naechsten Reboot oder
// Power-Cycle aus current/previous geschoben wird (Befund 2026-06-21: doppelter Boot loeschte ihn).
#define FILE_LOG_PATH_CRASH    "/log_crash.txt"
#define FILE_LOG_MAX_BYTES     32768  // Kappung je Datei; bei Ueberlauf werden die aeltesten Zeilen verworfen

// Datei-Log-Level, UNABHAENGIG von MAX_LOG_LEVEL (Serial). Zur Laufzeit ueber /logs umschaltbar,
// damit man fuer eine Diagnose-Session kurz INFO/DEBUG aktivieren kann, ohne den Flash dauerhaft
// zu belasten. Default = WARNING.
extern volatile int16_t fileLogLevel;

void log(int16_t level, const String &message_s);

// FS mounten, rotieren (current -> previous) und neues current mit Boot-Banner anlegen.
// Frueh in setup() aufrufen, vor den ersten zu persistierenden Logs. firmwareVersion fliesst
// ins Banner (FIRMWARE_VERSION ist in main.cpp eine lokale const, kein Makro).
void initFileLog(const char *firmwareVersion);

// Mutex um den LittleFS-Log-Zugriff. log() wird aus mehreren Tasks (loop, AsyncTCP, WiFi-Event)
// aufgerufen; der Webserver liest die Dateien aus loop(). Leser (Webserver) nutzen diese Wrapper.
bool logFsLock(uint32_t timeout_ms);
void logFsUnlock();

#endif // SRC_LOG_H_
