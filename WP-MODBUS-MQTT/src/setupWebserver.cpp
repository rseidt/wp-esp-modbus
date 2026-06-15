#include "setupWebserver.h"
#include "modbus_base.h"
#include <LittleFS.h>

// In main.cpp definiert — Umschaltung App-/MQTT-Steuerung inkl. WBR3_EN_PIN und Modbus-Poll.
void setControlMode(bool appControl);
bool isAppControlMode();

#if defined(ARDUINO_ARCH_ESP32)
WebServer server(80);
#elif defined(ARDUINO_ARCH_ESP8266)
ESP8266WebServer server(80);
#endif

// Registerdump 0..200 (= 201 Register). Der Bus wird einmal beim Seitenaufruf gescannt
// und direkt in die HTML-Tabelle gerendert. Der CSV-Download wird client-seitig aus genau
// dieser Tabelle erzeugt (siehe dlcsv() im HTML) → eine Datenquelle, kein server-seitiger State.
#define MODBUS_DUMP_START 0
#define MODBUS_DUMP_COUNT 201

// 4-stelliges Grossbuchstaben-Hex mit fuehrenden Nullen (z.B. 0x00FF → "00FF").
static String hex4(uint16_t value)
{
	String s = String(value, HEX);
	s.toUpperCase();
	while (s.length() < 4)
	{
		s = "0" + s;
	}
	return s;
}

// 16-stellige Binaerdarstellung mit fuehrenden Nullen.
static String bin16(uint16_t value)
{
	String s;
	for (int8_t b = 15; b >= 0; --b)
	{
		s += ((value >> b) & 1) ? '1' : '0';
	}
	return s;
}

void handleRoot()
{

	String content = "<html><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">";
	content += "<link rel=\"icon\" href=\"data:,\">";
	content += "<style>body { font-family: Arial; text-align: center;}</style>";
	content += "</head><body><h1>Reconfigure modbus bridge</h1>";
	content += "<p>Click <a href=\"/reconfigure\">here</a> to reconfigure modbus bridge.</p>";
	content += "<p>Click <a href=\"/update\">here</a> to update Firmware.</p>";
	content += "<p>Click <a href=\"/modbusdump\">here</a> to create a Modbus register dump (0..200).</p>";
	content += "<p>Click <a href=\"/control\">here</a> to switch control mode (Hersteller-App / MQTT).</p>";
	content += "<p>Click <a href=\"/logs\">here</a> to view logs.</p>";
	content += "</body></html>";

	server.send(200, "text/html", content);
}

// Klartext-Name eines Log-Levels (siehe log.h: 1=ERROR..4=DEBUG).
static String logLevelName(int16_t level)
{
	switch (level)
	{
	case LOG_LEVEL_ERROR:   return "ERROR";
	case LOG_LEVEL_WARNING: return "WARNING";
	case LOG_LEVEL_INFO:    return "INFO";
	case LOG_LEVEL_DEBUG:   return "DEBUG";
	default:                return "?";
	}
}

// Streamt eine Logdatei als text/plain. Nutzt das chunked-Transfer-Muster (wie handleModbusDump),
// damit auch 32 KB nicht als ein String im Heap landen. Liest unter logFsLock(), damit kein
// gleichzeitiger log()-Schreibzugriff die Datei waehrend des Lesens veraendert.
static void streamLogFile(const char *path)
{
	if (!logFsLock(2000))
	{
		server.send(503, "text/plain", "Log busy, bitte erneut versuchen.");
		return;
	}
	File f = LittleFS.open(path, "r");
	if (!f)
	{
		logFsUnlock();
		server.send(404, "text/plain", "Noch keine Logdatei vorhanden.");
		return;
	}
	server.setContentLength(CONTENT_LENGTH_UNKNOWN);
	server.send(200, "text/plain", "");
	char buf[1025];
	int n;
	while ((n = f.read((uint8_t *)buf, sizeof(buf) - 1)) > 0)
	{
		buf[n] = '\0';
		server.sendContent(buf);
	}
	f.close();
	server.sendContent(""); // schliesst den chunked Transfer ab
	logFsUnlock();
}

// /logs: GET zeigt aktuellen Datei-Log-Level + Umschalt-Buttons + Links zu den Logdateien.
// POST setzt fileLogLevel (validiert 1..4). Der Datei-Log-Level ist unabhaengig von MAX_LOG_LEVEL
// (Serial) und nur zur Laufzeit gedacht (kein Persistieren): nach Reboot wieder Default WARNING.
void handleLogs()
{
	if (server.method() == HTTP_POST)
	{
		int lvl = server.arg("level").toInt();
		if (lvl >= LOG_LEVEL_ERROR && lvl <= LOG_LEVEL_DEBUG)
		{
			fileLogLevel = (int16_t)lvl;
			log(LOG_LEVEL_WARNING, "File log level set to " + logLevelName(fileLogLevel) + " via web");
		}
		String content = "<html><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">";
		content += "<link rel=\"icon\" href=\"data:,\"><style>body{font-family:Arial;text-align:center;}</style>";
		content += "</head><body><h1>Log-Level geaendert</h1>";
		content += "<p>Datei-Log-Level: <b>" + logLevelName(fileLogLevel) + "</b></p>";
		content += "<p><a href=\"/logs\">Zurueck</a> | <a href=\"/\">Home</a></p></body></html>";
		server.send(200, "text/html", content);
		return;
	}

	String content = "<html><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">";
	content += "<link rel=\"icon\" href=\"data:,\"><style>body{font-family:Arial;text-align:center;}label{margin:0 .5em;}</style>";
	content += "</head><body><h1>Logs</h1>";
	content += "<p>Aktueller Datei-Log-Level: <b>" + logLevelName(fileLogLevel) + "</b></p>";
	content += "<form method='POST' action='/logs'>";
	for (int lvl = LOG_LEVEL_ERROR; lvl <= LOG_LEVEL_DEBUG; ++lvl)
	{
		content += "<label><input type='radio' name='level' value='" + String(lvl) + "'";
		content += (lvl == fileLogLevel) ? " checked" : "";
		content += "> " + logLevelName(lvl) + "</label>";
	}
	content += "<br><br><input type='submit' value='Level uebernehmen'></form>";
	content += "<p>INFO/DEBUG nur kurz fuer Diagnose nutzen (Flash-Verschleiss).</p>";
	content += "<hr><p><a href=\"/log/current\">Aktuelles Log anzeigen</a></p>";
	content += "<p><a href=\"/log/previous\">Log vor letztem Reboot (pre-restart) anzeigen</a></p>";
	content += "<p><a href=\"/\">Home</a></p></body></html>";
	server.send(200, "text/html", content);
}

// Umschalter zwischen Hersteller-App-Steuerung (WBR3D an, ESP-Poll pausiert) und
// MQTT-Steuerung (WBR3D aus, ESP pollt). GET zeigt das Formular mit dem aktuell aktiven
// Modus vorausgewaehlt; POST uebernimmt die Auswahl via setControlMode().
void handleControl()
{
	if (server.method() == HTTP_POST)
	{
		bool appControl = (server.arg("mode") == "app");
		setControlMode(appControl);
		String content = "<html><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">";
		content += "<link rel=\"icon\" href=\"data:,\">";
		content += "<style>body { font-family: Arial; text-align: center;}</style>";
		content += "</head><body><h1>Steuerungsmodus geaendert</h1>";
		content += appControl
					   ? "<p>Aktiv: <b>Hersteller-App</b> (WBR3D AN, Modbus-Poll pausiert).</p>"
					   : "<p>Aktiv: <b>MQTT</b> (WBR3D AUS, Modbus-Poll aktiv).</p>";
		content += "<p><a href=\"/control\">Zurueck</a> | <a href=\"/\">Home</a></p>";
		content += "</body></html>";
		server.send(200, "text/html", content);
		return;
	}

	bool app = isAppControlMode();
	String content = "<html><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">";
	content += "<link rel=\"icon\" href=\"data:,\">";
	content += "<style>body { font-family: Arial; text-align: center;} label{display:block;margin:1em;}</style>";
	content += "</head><body><h1>Steuerungsmodus</h1>";
	content += "<form method='POST' action='/control'>";
	content += "<label><input type='radio' name='mode' value='mqtt'";
	content += app ? "" : " checked";
	content += "> Steuerung via MQTT (WBR3D aus, ESP pollt)</label>";
	content += "<label><input type='radio' name='mode' value='app'";
	content += app ? " checked" : "";
	content += "> Steuerung via Hersteller-App (WBR3D an, ESP-Poll pausiert)</label>";
	content += "<input type='submit' value='Uebernehmen'>";
	content += "</form>";
	content += "<p><a href=\"/\">Home</a></p>";
	content += "</body></html>";
	server.send(200, "text/html", content);
}

void handleReconfigure()
{
	String content = "<html><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">";
	content += "<link rel=\"icon\" href=\"data:,\">";
	content += "<style>body { font-family: Arial; text-align: center;}</style>";
	content += "</head><body><h1>Captive portal started</h1>";
	content += "<p>Connect to AP <code>APAutoConnect</code> with password <code>password</code> to reconfigure.</p>";
	content += "<p>if the captive portal doesn't open, navigate your browser to <a href=\"http://192.168.4.1\" target=\"_blank\">http://192.168.4.1</a></p>";
	content += "</body></html>";

	server.send(200, "text/html", content);
	setupWifiManager(true);
}

void handleUploadForm()
{
	String content = "<html><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">";
	content += "<link rel=\"icon\" href=\"data:,\">";
	content += "<style>body { font-family: Arial; text-align: center;}</style>";
	content += "</head><body><h1>Firmware Update</h1>";
	content += "<form method='POST' action='/uploadFirmware' enctype='multipart/form-data'>";
	content += "<input type='file' name='update'>";
	content += "<input type='submit' value='Update Firmware'>";
	content += "</form></body></html>";

	server.send(200, "text/html", content);
}

void handleUpload()
{
	HTTPUpload &upload = server.upload();
	if (upload.status == UPLOAD_FILE_START)
	{
		Serial.printf("Update: %s\n", upload.filename.c_str());

#if defined(ARDUINO_ARCH_ESP8266)
#define UPDATE_SIZE_UNKNOWN 0XFFFFFFFF
#endif

		if (!Update.begin(UPDATE_SIZE_UNKNOWN))
		{ // start with max available size
			Update.printError(Serial);
		}
	}
	else if (upload.status == UPLOAD_FILE_WRITE)
	{
		/* flashing firmware to ESP*/
		if (Update.write(upload.buf, upload.currentSize) != upload.currentSize)
		{
			Update.printError(Serial);
		}
	}
	else if (upload.status == UPLOAD_FILE_END)
	{
		if (Update.end(true))
		{ // true to set the size to the current progress
			Serial.printf("Update Success: %u\nRebooting...\n", upload.totalSize);
		}
		else
		{
			Update.printError(Serial);
		}
	}
}

// HTML-Ansicht des Registerdumps. Scannt den Bus einmal und rendert die Tabelle
// (Register / Dezimal / Hex / Binaer); ungueltige Register erscheinen als "ERR".
// Der CSV-Download wird per JavaScript (dlcsv) direkt aus dieser Tabelle gebaut, damit
// CSV und Ansicht garantiert dieselben Daten enthalten.
// Die ~16 KB Seite wird per chunked Transfer (CONTENT_LENGTH_UNKNOWN) gestreamt — ein
// einzelnes server.send() der kompletten String-Antwort sprengt den TCP-Sendepuffer und
// fuehrt sonst zu ERR_CONTENT_LENGTH_MISMATCH (gesendete Bytes < angekuendigte Laenge).
void handleModbusDump()
{
	static uint16_t values[MODBUS_DUMP_COUNT];
	static bool valid[MODBUS_DUMP_COUNT];
	readHoldingRange(MODBUS_DUMP_START, MODBUS_DUMP_COUNT, values, valid);

	server.setContentLength(CONTENT_LENGTH_UNKNOWN);
	String head;
	head.reserve(700);
	head += "<html><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">";
	head += "<link rel=\"icon\" href=\"data:,\">";
	head += "<script>function dlcsv(){var r=document.querySelectorAll('#dump tr'),o=[];";
	head += "for(var i=0;i<r.length;i++){var c=r[i].querySelectorAll('th,td'),l=[];";
	head += "for(var j=0;j<c.length;j++){l.push(c[j].textContent);}o.push(l.join(','));}";
	head += "var b=new Blob([o.join('\\r\\n')],{type:'text/csv'}),a=document.createElement('a');";
	head += "a.href=URL.createObjectURL(b);a.download='modbus_dump.csv';a.click();}</script>";
	head += "</head><body><h1>Modbus Dump (Register 0..200)</h1>";
	head += "<p><button onclick=\"dlcsv()\">Download CSV</button> | <a href=\"/\">Home</a></p>";
	head += "<table border=\"1\" id=\"dump\"><tr><th>Reg</th><th>Dez</th><th>Hex</th><th>Bin</th></tr>";
	server.send(200, "text/html", head);

	// Zeilen in Buendeln streamen, damit kein grosser String im Heap entsteht.
	String chunk;
	chunk.reserve(1600);
	for (uint16_t i = 0; i < MODBUS_DUMP_COUNT; ++i)
	{
		chunk += "<tr><td>" + String(MODBUS_DUMP_START + i) + "</td>";
		if (valid[i])
		{
			chunk += "<td>" + String(values[i]) + "</td><td>0x" + hex4(values[i]) + "</td><td>" + bin16(values[i]) + "</td>";
		}
		else
		{
			chunk += "<td>ERR</td><td>ERR</td><td>ERR</td>";
		}
		chunk += "</tr>";
		if (chunk.length() > 1200)
		{
			server.sendContent(chunk);
			chunk = "";
		}
	}
	chunk += "</table></body></html>";
	server.sendContent(chunk);
	server.sendContent(""); // schliesst den chunked Transfer ab
}

void loopWebserver()
{
	server.handleClient();
}

void setupWebserver()
{
	server.on("/", handleRoot);
	server.on("/reconfigure", handleReconfigure);
	server.on("/update", handleUploadForm);
	server.on("/modbusdump", handleModbusDump);
	server.on("/control", handleControl);
	server.on("/logs", handleLogs);
	server.on("/log/current", []()
			  { streamLogFile(FILE_LOG_PATH_CURRENT); });
	server.on("/log/previous", []()
			  { streamLogFile(FILE_LOG_PATH_PREVIOUS); });
	server.on("/uploadFirmware", HTTP_POST, []()
	{
        server.send(200, "text/plain", (Update.hasError()) ? "FAIL" : "OK");
        ESP.restart();
	}, handleUpload);
	server.begin();
	log(LOG_LEVEL_INFO, "Webserver started on port 80");
}

// Bindet den TCP-Listen-Socket neu. Nötig nach WLAN-Verlust: lwIP reißt das Interface ab,
// der vorhandene Socket wird ungültig, server.handleClient() läuft ins Leere.
void restartWebserver()
{
	server.close();
	server.begin();
	log(LOG_LEVEL_INFO, "Webserver re-bound on port 80");
}
