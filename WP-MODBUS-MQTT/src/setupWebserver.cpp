#include "setupWebserver.h"
#include "modbus_base.h"
#include <LittleFS.h>
#include <Update.h>

// In main.cpp definiert — Umschaltung App-/MQTT-Steuerung inkl. WBR3_EN_PIN und Modbus-Poll.
void setControlMode(bool appControl);
bool isAppControlMode();
// In setupWifiManager.cpp definiert. Forward-deklariert statt setupWifiManager.h einzubinden, um die
// WiFiManager-/WebServer.h-Includekette (HTTP_*-Kollision mit ESPAsyncWebServer) aus dieser TU zu halten.
void setupWifiManager(bool forceConfigPortal);
// In main.cpp definiert — fuer die Versionsanzeige auf der Startseite.
extern const char *FIRMWARE_VERSION;

// Asynchroner Webserver (ESPAsyncWebServer) auf demselben AsyncTCP-Stack wie AsyncMqttClient.
// Ersetzt die synchrone WebServer-Klasse: deren server.handleClient() lief im Loop-Task und konnte
// dort dauerhaft blockieren (haengende TCP-Verbindung / lwIP-Mischstack-Kontention) -> Loop-Freeze,
// nur per Power-Cycle behebbar. AsyncWebServer-Handler laufen im AsyncTCP-Task, der Loop bleibt frei.
AsyncWebServer server(80);

// Zaehlt frueher die Neu-Bindungen des Listen-Sockets (synchroner Server verlor ihn bei WLAN-Verlust).
// Mit AsyncTCP nicht mehr noetig; bleibt fuer das /status-JSON (main.cpp) erhalten, bleibt jetzt 0.
volatile uint32_t webserverRestartCount = 0;

// Aufgeschobene Aktionen: Reboot/Reconfigure duerfen NICHT im AsyncTCP-Handler laufen (delay()/
// ESP.restart()/blockierendes WiFiManager). Der Handler setzt nur ein Flag + Faelligkeit; loopWebserver()
// (Loop-Task) fuehrt es kurz darauf aus — so flusht die HTTP-Antwort noch raus, bevor neu gestartet wird.
static volatile uint8_t pendingAction = 0; // 0=keine, 1=Reboot, 2=Reconfigure (Captive Portal)
static volatile uint32_t pendingActionAtMs = 0;
static void scheduleAction(uint8_t action)
{
	pendingActionAtMs = millis() + 800; // ~0,8 s Vorlauf, damit die Antwort noch ausgeliefert wird
	pendingAction = action;
}

// Registerdump 0..200 (= 201 Register). Der Bus wird vom Worker gescannt (non-blocking angestossen),
// die HTML-Tabelle bei Fertigstellung gerendert. Der CSV-Download wird client-seitig aus genau
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

void handleRoot(AsyncWebServerRequest *request)
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
	content += "<p>Click <a href=\"/reboot\">here</a> to reboot the ESP.</p>";
	content += "<hr><p><small>Firmware version: " + String(FIRMWARE_VERSION) + "</small></p>";
	content += "</body></html>";

	request->send(200, "text/html", content);
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

// Sendet eine Logdatei als text/plain. Liest sie unter logFsLock() komplett in einen String (<= ~32 KB,
// FILE_LOG_MAX_BYTES) und gibt sie dann als eine Antwort aus. Der Lock wird NUR waehrend des kurzen
// Lesens gehalten (nicht waehrend der asynchronen Uebertragung) — verhindert, dass ein gleichzeitiger
// log()-Schreibzugriff bzw. eine Trim-/Rotate-Operation die Datei mitten im Lesen veraendert.
static void sendLogFile(AsyncWebServerRequest *request, const char *path)
{
	if (!logFsLock(2000))
	{
		request->send(503, "text/plain", "Log busy, bitte erneut versuchen.");
		return;
	}
	File f = LittleFS.open(path, "r");
	if (!f)
	{
		logFsUnlock();
		request->send(404, "text/plain", "Noch keine Logdatei vorhanden.");
		return;
	}
	String body = f.readString();
	f.close();
	logFsUnlock();
	request->send(200, "text/plain", body);
}

// /logs: GET zeigt aktuellen Datei-Log-Level + Umschalt-Buttons + Links zu den Logdateien.
// POST setzt fileLogLevel (validiert 1..4). Der Datei-Log-Level ist unabhaengig von MAX_LOG_LEVEL
// (Serial) und nur zur Laufzeit gedacht (kein Persistieren): nach Reboot wieder Default WARNING.
void handleLogs(AsyncWebServerRequest *request)
{
	if (request->method() == HTTP_POST)
	{
		if (request->hasParam("level", true))
		{
			int lvl = request->getParam("level", true)->value().toInt();
			if (lvl >= LOG_LEVEL_ERROR && lvl <= LOG_LEVEL_DEBUG)
			{
				fileLogLevel = (int16_t)lvl;
				log(LOG_LEVEL_WARNING, "File log level set to " + logLevelName(fileLogLevel) + " via web");
			}
		}
		String content = "<html><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">";
		content += "<link rel=\"icon\" href=\"data:,\"><style>body{font-family:Arial;text-align:center;}</style>";
		content += "</head><body><h1>Log-Level geaendert</h1>";
		content += "<p>Datei-Log-Level: <b>" + logLevelName(fileLogLevel) + "</b></p>";
		content += "<p><a href=\"/logs\">Zurueck</a> | <a href=\"/\">Home</a></p></body></html>";
		request->send(200, "text/html", content);
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
	content += "<p><a href=\"/log/crash\">Letztes Crash-Log (Panic/Watchdog) anzeigen</a></p>";
	content += "<p><a href=\"/\">Home</a></p></body></html>";
	request->send(200, "text/html", content);
}

// Umschalter zwischen Hersteller-App-Steuerung (WBR3D an, ESP-Poll pausiert) und
// MQTT-Steuerung (WBR3D aus, ESP pollt). GET zeigt das Formular mit dem aktuell aktiven
// Modus vorausgewaehlt; POST uebernimmt die Auswahl via setControlMode().
void handleControl(AsyncWebServerRequest *request)
{
	if (request->method() == HTTP_POST)
	{
		bool appControl = request->hasParam("mode", true) && request->getParam("mode", true)->value() == "app";
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
		request->send(200, "text/html", content);
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
	request->send(200, "text/html", content);
}

void handleReconfigure(AsyncWebServerRequest *request)
{
	String content = "<html><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">";
	content += "<link rel=\"icon\" href=\"data:,\">";
	content += "<style>body { font-family: Arial; text-align: center;}</style>";
	content += "</head><body><h1>Captive portal started</h1>";
	content += "<p>Connect to AP <code>APAutoConnect</code> with password <code>password</code> to reconfigure.</p>";
	content += "<p>if the captive portal doesn't open, navigate your browser to <a href=\"http://192.168.4.1\" target=\"_blank\">http://192.168.4.1</a></p>";
	content += "</body></html>";
	request->send(200, "text/html", content);
	// setupWifiManager(true) (resetSettings + Reboot ins Portal) NICHT im AsyncTCP-Handler ausfuehren —
	// aufschieben in loopWebserver(), damit die Antwort noch ausgeliefert wird.
	scheduleAction(2);
}

void handleUploadForm(AsyncWebServerRequest *request)
{
	String content = "<html><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">";
	content += "<link rel=\"icon\" href=\"data:,\">";
	content += "<style>body { font-family: Arial; text-align: center;}</style>";
	content += "</head><body><h1>Firmware Update</h1>";
	content += "<form method='POST' action='/uploadFirmware' enctype='multipart/form-data'>";
	content += "<input type='file' name='update'>";
	content += "<input type='submit' value='Update Firmware'>";
	content += "</form></body></html>";
	request->send(200, "text/html", content);
}

// OTA-Upload-Handler: wird von ESPAsyncWebServer pro Datei-Chunk aufgerufen (index/len/final).
// Schreibt das Firmware-Image stueckweise via Update. Laeuft im AsyncTCP-Task -> kein blockierender
// Code, nur Update.write(). Der Reboot wird im Abschluss-Handler (siehe setupWebserver) aufgeschoben.
static void handleUpload(AsyncWebServerRequest *request, const String &filename, size_t index, uint8_t *data, size_t len, bool final)
{
	if (index == 0)
	{
		Serial.printf("Update: %s\n", filename.c_str());
		if (!Update.begin(UPDATE_SIZE_UNKNOWN)) // start with max available size
		{
			Update.printError(Serial);
		}
	}
	if (len)
	{
		if (Update.write(data, len) != len)
		{
			Update.printError(Serial);
		}
	}
	if (final)
	{
		if (Update.end(true)) // true to set the size to the current progress
		{
			Serial.printf("Update Success: %u\nRebooting...\n", index + len);
		}
		else
		{
			Update.printError(Serial);
		}
	}
}

// HTML-Ansicht des Registerdumps — non-blocking ueber den Worker. Zustandsmaschine je Aufruf:
//   IDLE   -> Dump anstossen (requestModbusDump), Auto-Refresh-Warteseite ausliefern
//   RUNNING-> Auto-Refresh-Warteseite (Worker scannt gerade den Bus, ~Sekunden)
//   DONE   -> Tabelle aus den Worker-Puffern rendern, danach Reset -> naechster Aufruf scannt neu
// Frueher blockierte der Handler bis zu 20 s auf den Bus-Scan — im AsyncTCP-Task verboten.
// Gerendert wird in einen AsyncResponseStream (waechst im Heap, ~16 KB), den AsyncWebServer
// segmentiert ueber die Verbindung schickt. Der CSV-Download entsteht client-seitig via dlcsv().
void handleModbusDump(AsyncWebServerRequest *request)
{
	ModbusDumpState st = modbusDumpState();
	if (st == MB_DUMP_DONE)
	{
		const uint16_t *values = modbusDumpValues();
		const bool *valid = modbusDumpValid();
		uint16_t start = modbusDumpStart();
		uint16_t count = modbusDumpCount();

		AsyncResponseStream *resp = request->beginResponseStream("text/html");
		resp->print("<html><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">");
		resp->print("<link rel=\"icon\" href=\"data:,\">");
		resp->print("<script>function dlcsv(){var r=document.querySelectorAll('#dump tr'),o=[];");
		resp->print("for(var i=0;i<r.length;i++){var c=r[i].querySelectorAll('th,td'),l=[];");
		resp->print("for(var j=0;j<c.length;j++){l.push(c[j].textContent);}o.push(l.join(','));}");
		resp->print("var b=new Blob([o.join('\\r\\n')],{type:'text/csv'}),a=document.createElement('a');");
		resp->print("a.href=URL.createObjectURL(b);a.download='modbus_dump.csv';a.click();}</script>");
		resp->print("</head><body><h1>Modbus Dump (Register 0..200)</h1>");
		resp->print("<p><button onclick=\"dlcsv()\">Download CSV</button> | <a href=\"/\">Home</a></p>");
		resp->print("<table border=\"1\" id=\"dump\"><tr><th>Reg</th><th>Dez</th><th>Hex</th><th>Bin</th></tr>");
		for (uint16_t i = 0; i < count; ++i)
		{
			String row = "<tr><td>" + String(start + i) + "</td>";
			if (valid[i])
			{
				row += "<td>" + String(values[i]) + "</td><td>0x" + hex4(values[i]) + "</td><td>" + bin16(values[i]) + "</td>";
			}
			else
			{
				row += "<td>ERR</td><td>ERR</td><td>ERR</td>";
			}
			row += "</tr>";
			resp->print(row);
		}
		resp->print("</table></body></html>");
		request->send(resp);
		modbusDumpReset(); // naechster Aufruf startet einen frischen Scan
		return;
	}

	if (st == MB_DUMP_IDLE)
	{
		requestModbusDump(MODBUS_DUMP_START, MODBUS_DUMP_COUNT); // non-blocking; bei Queue-voll einfach erneut laden
	}

	// RUNNING (oder gerade angestossen): Auto-Refresh-Warteseite.
	String wait = "<html><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">";
	wait += "<meta http-equiv=\"refresh\" content=\"2\"><link rel=\"icon\" href=\"data:,\">";
	wait += "<style>body{font-family:Arial;text-align:center;}</style></head><body>";
	wait += "<h1>Modbus Dump</h1><p>Dump laeuft... die Seite laedt automatisch neu.</p>";
	wait += "<p><a href=\"/\">Home</a></p></body></html>";
	request->send(200, "text/html", wait);
}

// /reboot: GET zeigt einen Bestaetigungs-Button, POST startet den ESP neu. Bewusst nur per POST
// (kein Reboot durch versehentlichen GET/Browser-Prefetch). Der eigentliche ESP.restart() wird
// aufgeschoben (loopWebserver), damit die Antwort noch ausgeliefert wird.
void handleReboot(AsyncWebServerRequest *request)
{
	if (request->method() == HTTP_POST)
	{
		log(LOG_LEVEL_WARNING, "Reboot via web requested");
		String content = "<html><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">";
		content += "<link rel=\"icon\" href=\"data:,\"><style>body{font-family:Arial;text-align:center;}</style>";
		content += "</head><body><h1>Neustart...</h1>";
		content += "<p>Der ESP startet neu. <a href=\"/\">Home</a> (nach ein paar Sekunden erneut laden).</p>";
		content += "</body></html>";
		request->send(200, "text/html", content);
		scheduleAction(1);
		return;
	}

	String content = "<html><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">";
	content += "<link rel=\"icon\" href=\"data:,\"><style>body{font-family:Arial;text-align:center;}</style>";
	content += "</head><body><h1>Neustart</h1>";
	content += "<p>ESP wirklich neu starten?</p>";
	content += "<form method='POST' action='/reboot'><input type='submit' value='Jetzt neu starten'></form>";
	content += "<p><a href=\"/\">Abbrechen</a></p></body></html>";
	request->send(200, "text/html", content);
}

// Fuehrt aufgeschobene Aktionen aus dem Loop-Task aus (nicht aus dem AsyncTCP-Handler): Reboot bzw.
// Reconfigure. Bedient KEINEN Webserver mehr (der laeuft asynchron). Wird jede Loop-Iteration gerufen.
void loopWebserver()
{
	if (pendingAction != 0 && (int32_t)(millis() - pendingActionAtMs) >= 0)
	{
		uint8_t action = pendingAction;
		pendingAction = 0;
		if (action == 1)
		{
			log(LOG_LEVEL_WARNING, "Aufgeschobener Reboot -> ESP.restart()");
			ESP.restart();
		}
		else if (action == 2)
		{
			log(LOG_LEVEL_WARNING, "Aufgeschobenes Reconfigure -> setupWifiManager(true) (Captive Portal)");
			setupWifiManager(true); // resetSettings + Reboot ins Portal
		}
	}
}

void setupWebserver()
{
	server.on("/", HTTP_GET, handleRoot);
	server.on("/reconfigure", HTTP_GET, handleReconfigure);
	server.on("/update", HTTP_GET, handleUploadForm);
	server.on("/modbusdump", HTTP_GET, handleModbusDump);
	server.on("/control", HTTP_ANY, handleControl); // GET-Formular + POST-Submit
	server.on("/reboot", HTTP_ANY, handleReboot);
	server.on("/logs", HTTP_ANY, handleLogs);
	server.on("/log/current", HTTP_GET, [](AsyncWebServerRequest *request)
			  { sendLogFile(request, FILE_LOG_PATH_CURRENT); });
	server.on("/log/previous", HTTP_GET, [](AsyncWebServerRequest *request)
			  { sendLogFile(request, FILE_LOG_PATH_PREVIOUS); });
	server.on("/log/crash", HTTP_GET, [](AsyncWebServerRequest *request)
			  { sendLogFile(request, FILE_LOG_PATH_CRASH); });
	server.on(
		"/uploadFirmware", HTTP_POST,
		[](AsyncWebServerRequest *request)
		{
			AsyncWebServerResponse *resp = request->beginResponse(200, "text/plain", Update.hasError() ? "FAIL" : "OK");
			resp->addHeader("Connection", "close");
			request->send(resp);
			scheduleAction(1); // Reboot aufschieben (Antwort erst ausliefern)
		},
		handleUpload);
	server.begin();
	log(LOG_LEVEL_INFO, "Async webserver started on port 80");
}

// Mit ESPAsyncWebServer nicht mehr noetig: der AsyncTCP-Listener bleibt ueber WLAN-Reconnects bestehen
// (kein Socket-Teardown wie bei der synchronen WebServer-Klasse). Bewusst No-op; das Symbol bleibt fuer
// den WiFi-Event-Handler in main.cpp erhalten.
void restartWebserver()
{
}
