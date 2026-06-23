#include "log.h"

#include <LittleFS.h>
#include <time.h>
#include <esp_system.h>
#include <esp_core_dump.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

volatile int16_t fileLogLevel = LOG_LEVEL_WARNING;

static SemaphoreHandle_t logMutex = nullptr;
static bool fileLogReady = false;

bool logFsLock(uint32_t timeout_ms)
{
	if (logMutex == nullptr)
	{
		return false;
	}
	return xSemaphoreTake(logMutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void logFsUnlock()
{
	if (logMutex != nullptr)
	{
		xSemaphoreGive(logMutex);
	}
}

// Wanduhr-Zeitstempel via NTP (configTime() laeuft beim WLAN-Setup). Solange NTP noch nicht
// synchron ist (fruehe Bootphase), Fallback auf Uptime "+<millis>ms".
static String logTimestamp()
{
	struct tm now;
	if (getLocalTime(&now, 0))
	{
		char buf[20];
		strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &now);
		return String(buf);
	}
	return "+" + String(millis()) + "ms";
}

static const char *resetReasonStr()
{
	switch (esp_reset_reason())
	{
	case ESP_RST_POWERON:   return "Power-on";
	case ESP_RST_EXT:       return "External pin";
	case ESP_RST_SW:        return "Software reset (esp_restart)";
	case ESP_RST_PANIC:     return "Panic/Exception (CRASH)";
	case ESP_RST_INT_WDT:   return "Interrupt watchdog";
	case ESP_RST_TASK_WDT:  return "Task watchdog";
	case ESP_RST_WDT:       return "Other watchdog";
	case ESP_RST_DEEPSLEEP: return "Deep sleep wake";
	case ESP_RST_BROWNOUT:  return "Brownout";
	case ESP_RST_SDIO:      return "SDIO";
	default:                return "Unknown";
	}
}

// Kappt /log.txt auf die letzte Haelfte (Tail-Erhalt), ohne grosse RAM-Allokation: kopiert ab
// dem Schnittpunkt chunkweise in /log.tmp und benennt um. Wird aus log() unter gehaltenem Mutex
// gerufen, sobald die Datei FILE_LOG_MAX_BYTES ueberschreitet.
static void trimCurrentLog()
{
	File in = LittleFS.open(FILE_LOG_PATH_CURRENT, "r");
	if (!in)
	{
		return;
	}
	size_t size = in.size();
	size_t keep = FILE_LOG_MAX_BYTES / 2;
	size_t startAt = (size > keep) ? (size - keep) : 0;
	in.seek(startAt, SeekSet);
	// Bis zum naechsten Zeilenumbruch vorruecken, damit die erste Zeile nicht zerschnitten ist.
	while (in.available())
	{
		if (in.read() == '\n')
		{
			break;
		}
	}

	File out = LittleFS.open(FILE_LOG_PATH_TMP, "w");
	if (!out)
	{
		in.close();
		return;
	}
	uint8_t buf[512];
	size_t n;
	while ((n = in.read(buf, sizeof(buf))) > 0)
	{
		out.write(buf, n);
	}
	in.close();
	out.close();

	LittleFS.remove(FILE_LOG_PATH_CURRENT);
	LittleFS.rename(FILE_LOG_PATH_TMP, FILE_LOG_PATH_CURRENT);
}

// Kopiert eine Datei chunkweise (ohne grosse RAM-Allokation). Genutzt fuer die Crash-Log-Sicherung.
static void copyFile(const char *src, const char *dst)
{
	File in = LittleFS.open(src, "r");
	if (!in)
	{
		return;
	}
	File out = LittleFS.open(dst, "w");
	if (!out)
	{
		in.close();
		return;
	}
	uint8_t buf[512];
	size_t n;
	while ((n = in.read(buf, sizeof(buf))) > 0)
	{
		out.write(buf, n);
	}
	in.close();
	out.close();
}

void initFileLog(const char *firmwareVersion)
{
	if (logMutex == nullptr)
	{
		logMutex = xSemaphoreCreateMutex();
	}

	if (!LittleFS.begin(true)) // format-if-failed
	{
		Serial.println("[1]: initFileLog: LittleFS mount failed, file logging disabled");
		return;
	}

	// Crash-Log sichern, BEVOR rotiert wird: war der letzte Reset ein Absturz (Panic/Watchdog/
	// Brownout), die Breadcrumbs der abgestuerzten Session (= aktuelles /log.txt) in die nicht
	// rotierte /log_crash.txt kopieren. Sonst schiebt schon der naechste (Auto-)Reboot oder ein
	// Power-Cycle den Crash-Kontext aus current/previous heraus (Befund 2026-06-21). /log/crash
	// haelt damit IMMER die letzte abgestuerzte Session, ueberlebt beliebig viele Reboots.
	esp_reset_reason_t rr = esp_reset_reason();
	bool wasCrash = (rr == ESP_RST_PANIC || rr == ESP_RST_TASK_WDT || rr == ESP_RST_INT_WDT ||
					 rr == ESP_RST_WDT || rr == ESP_RST_BROWNOUT);
	if (wasCrash && LittleFS.exists(FILE_LOG_PATH_CURRENT))
	{
		copyFile(FILE_LOG_PATH_CURRENT, FILE_LOG_PATH_CRASH);
	}

	// Rotation: Log des vorherigen Boots als "pre-restart" sichern, dann frisch beginnen.
	if (LittleFS.exists(FILE_LOG_PATH_CURRENT))
	{
		if (LittleFS.exists(FILE_LOG_PATH_PREVIOUS))
		{
			LittleFS.remove(FILE_LOG_PATH_PREVIOUS);
		}
		LittleFS.rename(FILE_LOG_PATH_CURRENT, FILE_LOG_PATH_PREVIOUS);
	}
	// evtl. Reste einer abgebrochenen Trim-Operation
	if (LittleFS.exists(FILE_LOG_PATH_TMP))
	{
		LittleFS.remove(FILE_LOG_PATH_TMP);
	}

	File f = LittleFS.open(FILE_LOG_PATH_CURRENT, "w");
	if (f)
	{
		f.println("===== BOOT " + logTimestamp() + " =====");
		f.println("Firmware " + String(firmwareVersion) + " (compiled " + __DATE__ + " " + __TIME__ + ")");
		f.println("Reset reason: " + String(resetReasonStr()));
		// Coredump-Hinweis ins Boot-Banner: liegt nach einem Panic ein Flash-Coredump vor, hier
		// vermerken, damit der Crash-Workflow (Datei-Log -> /coredump) direkt sichtbar verknuepft ist.
		size_t cdAddr = 0, cdSize = 0;
		if (esp_core_dump_image_get(&cdAddr, &cdSize) == ESP_OK && cdSize > 0)
		{
			f.println("Coredump: vorhanden (" + String((unsigned)cdSize) + " Bytes) -> Backtrace unter /coredump");
		}
		f.close();
	}
	fileLogReady = true;
}

void log(int16_t level, const String &message_s)
{
	if (level <= MAX_LOG_LEVEL)
	{
		Serial.println("[" + String(level) + "]: " + message_s);
	}

	if (fileLogReady && level <= fileLogLevel && logFsLock(100))
	{
		File f = LittleFS.open(FILE_LOG_PATH_CURRENT, "a");
		if (f)
		{
			f.println(logTimestamp() + " [" + String(level) + "] " + message_s);
			size_t sz = f.size();
			f.close();
			if (sz > FILE_LOG_MAX_BYTES)
			{
				trimCurrentLog();
			}
		}
		logFsUnlock();
	}
}
