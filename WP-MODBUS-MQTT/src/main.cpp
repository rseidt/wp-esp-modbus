#include "main.h"

// Modbus-Bus-Timing (MODBUS_POLL_INTERVAL_MS / MODBUS_SCANRATE_MS) liegt jetzt in modbus_base.h:
// der Worker-Task taktet sich selbst, kein Poll-Timer mehr im Loop.

static char HOSTNAME[12] = "ESP-MM-FFFF";
static const char __attribute__((__unused__)) *TAG = "Main";

// static const char *FIRMWARE_URL = "https://domain.com/path/file.bin";
// Nicht static: der Webserver (setupWebserver.cpp) zeigt die Version auf der Startseite an (extern).
const char *FIRMWARE_VERSION = "000.000.026";

// instanciate AsyncMqttClient object
AsyncMqttClient mqtt_client;

// instanciate timers
Timer<1, millis> mqtt_reconnect_timer;
Timer<1, millis> wifi_reconnect_timer;
Timer<1, millis> memory_report_timer;
bool wifiConnected = false;
bool mqttConnected = false;
// false = MQTT-Steuerung (WBR3D aus, ESP pollt). true = Hersteller-App (WBR3D an, ESP-Poll pausiert).
// Default = MQTT, passend zum HIGH-Boot-Zustand von WBR3_EN_PIN. Keine Persistenz: jeder Boot startet im MQTT-Modus.
// volatile: aus dem Loop-Task (setControlMode) geschrieben, aus dem Worker-Task (Core 0) gelesen.
volatile bool appControlMode = false;

int freeHeap;

// Flap-Zaehler fuer die Ferndiagnose der Link-Stabilitaet (im /status-JSON exponiert). Steigen sie
// im Gleichschritt mit zunehmender Traegheit, ist der Link die Ursache. mqtt/wifiDisconnectCount aus
// den jeweiligen Callbacks; webserverRestartCount aus restartWebserver() (extern, setupWebserver.cpp).
volatile uint32_t mqttDisconnectCount = 0;
volatile uint32_t wifiDisconnectCount = 0;
extern volatile uint32_t webserverRestartCount;

// Schaltet zwischen App- und MQTT-Steuerung um und setzt entsprechend WBR3_EN_PIN.
// Sorgt dafuer, dass nie zwei Modbus-Master gleichzeitig aktiv sind (kein Buskonflikt):
//  - App-Modus: WBR3D AN; der Modbus-Worker sieht appControlMode und fasst den Bus nicht an.
//  - MQTT-Modus: WBR3D AUS; der Worker pollt wieder.
// Kein Timer-Handling mehr noetig — der Worker entscheidet selbst anhand von appControlMode.
void setControlMode(bool appControl)
{
	appControlMode = appControl;
	if (appControl)
	{
		log(LOG_LEVEL_INFO, "Control mode: Hersteller-App (WBR3D AN, Modbus-Poll pausiert)");
		digitalWrite(WBR3_EN_PIN, LOW); // BC547 sperrt -> Pull-up -> WBR3D EN HIGH -> WBR3D AN
	}
	else
	{
		log(LOG_LEVEL_INFO, "Control mode: MQTT (WBR3D AUS, Modbus-Poll aktiv)");
		digitalWrite(WBR3_EN_PIN, HIGH); // BC547 leitet -> WBR3D EN auf GND -> WBR3D AUS
	}
}

bool isAppControlMode()
{
	return appControlMode;
}

void startWifiConnectTimer()
{
	wifi_reconnect_timer.cancel();
	wifi_reconnect_timer.every(2000, connectToWifi);
}

void stopWifiConnectTimer()
{
	wifi_reconnect_timer.cancel();
}

void startMemoryReportTimer()
{
	memory_report_timer.cancel();
	memory_report_timer.every(20000, reportMemoryStatus);
}
void stopMemoryReportTimer()
{
	memory_report_timer.cancel();
}

void startMqttConnectTimer()
{
	mqtt_reconnect_timer.cancel();
	mqtt_reconnect_timer.every(2000, connectToMqtt);
}

void stopMqttConnectTimer()
{
	mqtt_reconnect_timer.cancel();
}

bool reportMemoryStatus(void *pvParameters)
{
	int freeHeap = ESP.getFreeHeap();
	log(LOG_LEVEL_INFO, "Free heap: " + String(freeHeap) + " bytes");
	struct tm now;
	if (getLocalTime(&now))
	{
		log(LOG_LEVEL_INFO, "Time: " + String(now.tm_year + 1900) + "-" + String(now.tm_mon + 1) + "-" + String(now.tm_mday) + " " + String(now.tm_hour) + ":" + String(now.tm_min) + ":" + String(now.tm_sec));
	}
	else
	{
		log(LOG_LEVEL_ERROR, "Failed to obtain time");
	}
	String json = "{";
	json += "\"freeHeap\":" + String(freeHeap) + ",";
	json += "\"minFreeHeap\":" + String(ESP.getMinFreeHeap()) + ",";
	// rssi: WLAN-Signalstaerke in dBm (näher an 0 = besser; < -75 dBm = schwach). Diagnostiziert,
	// ob Rest-Latenz/Drops trotz WiFi.setSleep(false) an einem schwachen Link liegen.
	json += "\"rssi\":" + String(WiFi.isConnected() ? WiFi.RSSI() : 0) + ",";
	// Interner Die-Temperatursensor des ESP32 (temperatureRead() -> Grad C auf Core 3.x). Misst die
	// Chip-/Die-Temperatur, NICHT die Umgebung (liest auf dem Classic-ESP32 erfahrungsgemaess hoch);
	// dient der Ueberwachung von thermischem Stress, nicht als Raumtemperatur.
	json += "\"internalTemp\":" + String(temperatureRead(), 1) + ",";
	// Flap-Zaehler seit Boot: steigen sie synchron mit zunehmender Traegheit -> Link instabil.
	json += "\"mqttDisconnects\":" + String(mqttDisconnectCount) + ",";
	json += "\"wifiDisconnects\":" + String(wifiDisconnectCount) + ",";
	json += "\"webserverRestarts\":" + String(webserverRestartCount) + ",";
	json += "\"uptime\":" + String(millis() / 1000) + ",";
	json += "\"time\":\"" + String(now.tm_year + 1900) + "-" + String(now.tm_mon + 1) + "-" + String(now.tm_mday) + " " + String(now.tm_hour) + ":" + String(now.tm_min) + ":" + String(now.tm_sec) + "\"";
	json += "}";
	if (mqtt_client.connected())
	{
		String mqtt_complete_topic = param_mqtt_topic;
		mqtt_complete_topic += "/" + String(HOSTNAME) + "/status";
		log(LOG_LEVEL_INFO, "MQTT Publishing data to topic " + mqtt_complete_topic + ": " + json);
		mqtt_client.publish(mqtt_complete_topic.c_str(), 0, true, json.c_str(), json.length());
	}
	return true;
}

void resetWiFi()
{
	log(LOG_LEVEL_INFO, "Resetting Wifi");
	WiFi.mode(WIFI_STA);
	WiFi.disconnect();
	wifiConnected = false;
}

bool connectToWifi(void *pvParameters)
{
	if (!WiFi.isConnected())
	{
		log(LOG_LEVEL_INFO, "Connecting to WiFi");
		wl_status_t state = WiFi.begin();
	}
	return true;
}

bool connectToMqtt(void *pvParameters)
{
	if (!mqtt_client.connected())
	{
		log(LOG_LEVEL_INFO, "Connecting to MQTT");
		mqtt_client.connect();
	}
	return true;
}

void onMqttConnect(bool sessionPresent)
{
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

void onMqttDisconnect(AsyncMqttClientDisconnectReason reason)
{
	mqttDisconnectCount++;
	log(LOG_LEVEL_WARNING, "Disconnected from MQTT: " + String((int)reason));
	// Timer immer starten — connect() failt solange WLAN weg ist, retried aber alle 2 s automatisch.
	// Zusätzlich triggert der WiFi-Event-Handler den Connect bei STA_GOT_IP.
	startMqttConnectTimer();
}

// Reagiert auf WLAN-Statusänderungen. Nötig weil:
//  - Webserver (synchrone WebServer-Klasse) verliert seinen TCP-Listen-Socket beim WLAN-Teardown
//    und hat keinen eigenen Reconnect-Hook → muss bei GOT_IP neu gebunden werden.
//  - MQTT-Reconnect ist über den Timer abgedeckt, hier wird er zur Sicherheit angestoßen.
void wiFiEvent(WiFiEvent_t event)
{
	switch (event)
	{
	case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
		wifiDisconnectCount++;
		log(LOG_LEVEL_WARNING, "WiFi disconnected");
		break;
	case ARDUINO_EVENT_WIFI_STA_GOT_IP:
		log(LOG_LEVEL_INFO, "WiFi got IP: " + WiFi.localIP().toString());
		restartWebserver();
		startMqttConnectTimer();
		break;
	default:
		break;
	}
}

void onMqttSubscribe(uint16_t packetId, uint8_t qos)
{
	log(LOG_LEVEL_INFO, "Subscribe acknowledged for packetId: " + String(packetId) + " qos: " + String(qos));
}

void onMqttUnsubscribe(uint16_t packetId)
{
	log(LOG_LEVEL_INFO, "Unsubscribe acknowledged for packetId: " + String(packetId));
}

// Baut das komplette Datenmodell (Register + Fehlerstatus) aus dem aktuellen Cache
// (register_values[]/Fault-Cache) und publisht es retained auf .../data. Liest NICHT den Bus —
// arbeitet rein aus dem Cache. Wird vom Poller nach einem vollen Zyklus aufgerufen UND direkt nach
// einem erfolgreichen MQTT-Write, damit der gesetzte Wert sofort (ohne Poll-Latenz) zurueckgemeldet
// wird und kein Feedback-Loop/Flackern beim Umschalten in Home Assistant entsteht.
void publishModbusData()
{
	JsonDocument json_doc;
	// Cache konsistent lesen: der Worker (Core 0) schreibt register_values[]/Fault-Cache, hier
	// (Loop-Task) wird daraus das JSON gebaut. Kurzer Lock verhindert torn/inkonsistente Reads.
	if (lockRegisterCache(200))
	{
		writeRegisterValuesToJson(json_doc);
		writeFaultStatusToJson(json_doc); // Geraetefehler als faults[]/fault_active in dieselbe Struktur
		unlockRegisterCache();
	}
	else
	{
		log(LOG_LEVEL_WARNING, "publishModbusData: Cache-Lock-Timeout, Publish uebersprungen");
		return;
	}
	size_t json_size = measureJson(json_doc) + 1;
	log(LOG_LEVEL_INFO, "JSON size: " + String(json_size) + " bytes");
	char *buffer = (char *)malloc(json_size * sizeof(char));
	size_t n = serializeJson(json_doc, buffer, json_size);
	log(LOG_LEVEL_INFO, "JSON serialized: " + String(buffer));
	if (mqtt_client.connected())
	{
		String mqtt_complete_topic = param_mqtt_topic;
		mqtt_complete_topic += "/" + String(HOSTNAME) + "/data";
		log(LOG_LEVEL_INFO, "MQTT Publishing data to topic: " + String(mqtt_complete_topic.c_str()));
		mqtt_client.publish(mqtt_complete_topic.c_str(), 0, true, buffer, n);
	}
	free(buffer);
}

void onMqttMessage(char *topic, char *payload, AsyncMqttClientMessageProperties properties, size_t len, size_t index, size_t total)
{
	// Payload defensiv lesen: AsyncMqttClient liefert bei einer leeren Nachricht (z.B. einer
	// GELOESCHTEN retained Message — MQTT loescht via Zero-Length-Retain) payload==nullptr und/oder
	// len==0. Das fruehere payload[total]=0 schrieb dann nach Adresse 0 -> StoreProhibited-Crash
	// (und lag ohnehin eine Stelle hinter dem nur len Bytes grossen, nicht terminierten Lib-Puffer).
	// Daher NICHT in den Puffer schreiben, sondern nur die tatsaechlich gelieferten len Bytes kopieren.
	String payload_s;
	if (payload != nullptr && len > 0)
	{
		payload_s.reserve(len);
		for (size_t i = 0; i < len; ++i)
		{
			payload_s += payload[i];
		}
	}
	log(LOG_LEVEL_INFO, "Message received (topic=" + String(topic) + ", qos=" + String(properties.qos) + ", dup=" + String(properties.dup) + ", retain=" + String(properties.retain) + ", len=" + String(len) + ", index=" + String(index) + ", total=" + String(total) + "): " + payload_s);

	String suffix = String(topic).substring(strlen(param_mqtt_topic) + strlen(HOSTNAME) + 9);
	// substring(1 + strlen(MQTT_TOPIC) + strlen("/") + strlen(HOSTNAME) + strlen("/") + strlen("action"))
	log(LOG_LEVEL_INFO, "MQTT topic suffix=" + String(suffix.c_str()));

	if (suffix == "write_register")
	{
		log(LOG_LEVEL_INFO, "MQTT Write modbus register requested");
		int eq = payload_s.indexOf('=');
		if (eq < 0)
		{
			// Leere/ungueltige Nachricht -> ignorieren, Poller NICHT anhalten. ERWARTETER Normalfall:
			// ioBroker setzt das write_register-Topic nach erfolgreichem Write auf "" zurueck, damit
			// der Broker keine retained Nachricht nachliefert, die spaeter erneut einen Write ausloest.
			// Loest hier nichts auf dem Modbus aus -> nur INFO, kein WARNING (siehe Crash-Doku CLAUDE.md).
			log(LOG_LEVEL_INFO, "write_register ohne gueltiges 'name=value' (Payload='" + payload_s + "') - ignoriert");
			return;
		}
		String register_name = payload_s.substring(0, eq);
		String register_value = payload_s.substring(eq + 1);
		log(LOG_LEVEL_INFO, "Writing register name=" + String(register_name) + " with value=" + String(register_value));

		// NUR einreihen, NICHT hier ausfuehren: writeModbusRegister blockiert per Busy-Wait und liefe
		// sonst im AsyncTCP-Callback -> TCP/MQTT haengt, Task-Watchdog (Crash 2026-06-16). Der
		// Worker-Task fuehrt den Write aus, aktualisiert den Cache und stoesst den /data-Publish an
		// (Sofort-Feedback ohne Bus-Read) — siehe serviceRequest()/consumeModbusPublishRequest().
		if (!enqueueModbusWrite(register_name.c_str(), (uint16_t)register_value.toInt()))
		{
			log(LOG_LEVEL_ERROR, "Failed to enqueue write " + String(register_name) + "=" + String(register_value));
		}
		return;
	}
	else
	{
		log(LOG_LEVEL_INFO, "Unknown MQTT topic received: " + String(topic));
	}
}

void onMqttPublish(uint16_t packetId)
{
	log(LOG_LEVEL_INFO, "Publish acknowledged for packetId: " + String(packetId));
}

// Wird im Loop-Task aufgerufen, sobald der Worker neue Daten gemeldet hat (consumeModbusPublishRequest).
// Baut /data aus dem Cache und meldet zusaetzlich den letzten Modbus-Status. Bewusst im Loop-Task,
// damit AsyncMqttClient aus genau einem Task bedient wird (kein Cross-Task-Publish).
void publishModbusUpdate()
{
#ifndef MODBUS_DISABLED
	publishModbusData();
	String modbus_state = getModbusState();
	if (modbus_state != "" && mqtt_client.connected())
	{
		String mqtt_complete_topic = param_mqtt_topic;
		mqtt_complete_topic += "/" + String(HOSTNAME) + "/modbus_status";
		mqtt_client.publish(mqtt_complete_topic.c_str(), 0, true, modbus_state.c_str(), modbus_state.length());
	}
#endif // MODBUS_DISABLED
}

void setup()
{
	// So frueh wie moeglich: WBR3D stilllegen, damit der ESP32 von Anfang an alleiniger
	// Modbus-Master ist (kein Buskonflikt mit dem Tuya-Modul). HIGH = WBR3D AUS.
	pinMode(WBR3_EN_PIN, OUTPUT);
	digitalWrite(WBR3_EN_PIN, HIGH);

	// debug comm
	Serial.begin(74880);
	// while (!Serial) continue;
	delay(500);
	// Serial.setDebugOutput(true);
	log(LOG_LEVEL_INFO, "Serial started at 74880 baud");

	// Persistentes Datei-Logging frueh starten: mountet LittleFS, rotiert das Log des
	// vorherigen Boots nach /log_prev.txt und beginnt /log.txt neu. Ab hier landet alles
	// (Datei-Log-Level) auch im Flash und ist nach einem Reboot/Crash ueber /logs auslesbar.
	initFileLog(FIRMWARE_VERSION);

	setupWifiManager(false);
	setupWebserver();

#if defined(ARDUINO_ARCH_ESP32)
	uint64_t fullMAC = ESP.getEfuseMac();
    uint32_t chipID = fullMAC & 0xFFFFFFFF; // Extract the lower 32 bits
	snprintf(HOSTNAME, sizeof(HOSTNAME), "ESP-MM-%X", chipID);
#elif defined(ARDUINO_ARCH_ESP8266)
	snprintf(HOSTNAME, sizeof(HOSTNAME), "ESP-MM-%X", ESP.getChipId());
#endif

	log(LOG_LEVEL_INFO, "*********************************************************************");
	log(LOG_LEVEL_INFO, "Firmware version " + String(FIRMWARE_VERSION) + " (compiled at " + __DATE__ + " " + __TIME__ + ")");
	log(LOG_LEVEL_INFO, "Hostname: " + String(HOSTNAME));

	WiFi.onEvent(wiFiEvent);

	// WiFi-Modem-Sleep abschalten. Default ist WIFI_PS_MIN_MODEM: der Funk schlaeft zwischen
	// DTIM-Beacons und weckt nur verzoegert auf -> eingehende Pakete stauen sich, RTT springt auf
	// Sekunden (per Ping bestaetigt: 0 % Loss, aber 200 ms..4 s steigend). Folge: traeger
	// synchroner Webserver und periodische MQTT-Keepalive-Abbrueche (reason 0). Der ESP wird per
	// Buck-Wandler aus dem Bus versorgt -> der Mehrverbrauch (~20-30 mA) ist irrelevant.
	WiFi.setSleep(false);

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
	startModbusWorker(); // dedizierter Bus-Owner-Task (ersetzt den Poll-Timer im Loop)
#endif // MODBUS_DISABLED
}

void loop()
{
	loopWebserver();
	wifi_reconnect_timer.tick();
	mqtt_reconnect_timer.tick();
	memory_report_timer.tick();
#ifndef MODBUS_DISABLED
	// Heartbeat fuer den Worker-Watchdog: solange der Loop-Task laeuft, wird der Zeitstempel
	// aktualisiert. Bleibt er aus (eingefrorener Loop), rebootet der Worker den ESP (Selbstheilung).
	feedLoopHeartbeat();
	// Der Worker-Task signalisiert hierueber neue Daten; der Publish laeuft bewusst im Loop-Task.
	if (consumeModbusPublishRequest())
	{
		publishModbusUpdate();
	}
#endif // MODBUS_DISABLED
}
