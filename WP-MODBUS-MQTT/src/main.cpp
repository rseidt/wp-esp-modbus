#include "main.h"

const int MODBUS_SCANRATE = 1; // in seconds (Pause zwischen Poll-Zyklen; dominiert die Update-Rate)

// Abstand zwischen zwei Modbus-Transaktionen (= Poll-Tick; der Poller liest einen Range je Tick).
// Per Diagnose 2026-06-15 bestaetigt: dieser Slave verschluckt Anfragen, die zu kurz (~100 ms) auf
// die vorige folgen -> jede Range ausser der ersten lief sonst im 1. Versuch in einen Timeout.
// 500 ms gibt dem Slave Erholzeit. Bei verbleibenden Erstversuch-Timeouts weiter erhoehen (z.B. 1000).
const int MODBUS_POLL_INTERVAL_MS = 500;

static char HOSTNAME[12] = "ESP-MM-FFFF";
static const char __attribute__((__unused__)) *TAG = "Main";

// static const char *FIRMWARE_URL = "https://domain.com/path/file.bin";
static const char *FIRMWARE_VERSION = "000.000.024";

// instanciate AsyncMqttClient object
AsyncMqttClient mqtt_client;

// instanciate timers
Timer<1, millis> mqtt_reconnect_timer;
Timer<1, millis> wifi_reconnect_timer;
Timer<1, millis> modbus_poller_timer;
Timer<1, millis> memory_report_timer;
bool modbus_poller_inprogress = false;
bool modbus_poller_paused = false;
ulong modbus_poller_paused_millis = 0;
bool wifiConnected = false;
bool mqttConnected = false;
// false = MQTT-Steuerung (WBR3D aus, ESP pollt). true = Hersteller-App (WBR3D an, ESP-Poll pausiert).
// Default = MQTT, passend zum HIGH-Boot-Zustand von WBR3_EN_PIN. Keine Persistenz: jeder Boot startet im MQTT-Modus.
bool appControlMode = false;

int freeHeap;

// Timer<1, millis> hat nur 1 Slot — defensives cancel() vor every(), damit ein bereits laufender
// Task nicht den neuen .every()-Aufruf still verschluckt.
void startModbusPoller()
{
	modbus_poller_timer.cancel();
	modbus_poller_timer.every(MODBUS_POLL_INTERVAL_MS, runModbusPollerTask);
}

void stopModbusPoller()
{
	modbus_poller_timer.cancel();
	modbus_poller_inprogress = false;
}

// Schaltet zwischen App- und MQTT-Steuerung um und setzt entsprechend WBR3_EN_PIN.
// Sorgt dafuer, dass nie zwei Modbus-Master gleichzeitig aktiv sind (kein Buskonflikt):
//  - App-Modus: WBR3D AN, ESP-Poll gestoppt und Auto-Resume in loop() unterbunden.
//  - MQTT-Modus: WBR3D AUS, ESP-Poll laeuft.
void setControlMode(bool appControl)
{
	appControlMode = appControl;
	if (appControl)
	{
		log(LOG_LEVEL_INFO, "Control mode: Hersteller-App (WBR3D AN, Modbus-Poll pausiert)");
		digitalWrite(WBR3_EN_PIN, LOW); // BC547 sperrt -> Pull-up -> WBR3D EN HIGH -> WBR3D AN
		stopModbusPoller();
		modbus_poller_paused = false; // verhindert Auto-Resume in loop()
	}
	else
	{
		log(LOG_LEVEL_INFO, "Control mode: MQTT (WBR3D AUS, Modbus-Poll aktiv)");
		digitalWrite(WBR3_EN_PIN, HIGH); // BC547 leitet -> WBR3D EN auf GND -> WBR3D AUS
		modbus_poller_paused = false;
		startModbusPoller();
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
	json += "\"uptime\":" + String(millis() / 1000) + ",";
	json += "\"time\":\"" + String(now.tm_year + 1900) + "-" + String(now.tm_mon + 1) + "-" + String(now.tm_mday) + " " + String(now.tm_hour) + ":" + String(now.tm_min) + ":" + String(now.tm_sec) + "\"";
	json += "}";
	if (mqtt_client.connected())
	{
		String mqtt_complete_topic = param_mqtt_topic;
		mqtt_complete_topic += "/" + String(HOSTNAME) + "/status";
		log(LOG_LEVEL_WARNING, "MQTT Publishing data to topic " + mqtt_complete_topic + ": " + json);
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
	writeRegisterValuesToJson(json_doc);
	writeFaultStatusToJson(json_doc); // Geraetefehler als faults[]/fault_active in dieselbe Struktur
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
		stopModbusPoller();
		String register_name = payload_s.substring(0, eq);
		String register_value = payload_s.substring(eq + 1);
		log(LOG_LEVEL_INFO, "Writing register name=" + String(register_name) + " with value=" + String(register_value));

		if (writeModbusRegister(register_name.c_str(), register_value.toInt()))
		{
			log(LOG_LEVEL_INFO, "Register written successfully");
			// Sofort das komplette Datenmodell aus dem Cache zurueckmelden (writeModbusRegister hat
			// register_values[] bereits aktualisiert) — kein Bus-Read, keine Poll-Latenz. Verhindert
			// das Feedback-Loop/Flackern beim Umschalten (HA sieht den neuen Zustand unmittelbar).
			publishModbusData();
		}
		else
		{
			log(LOG_LEVEL_ERROR, "Failed to write register " + String(register_name) + " with value \"" + String(register_value) + "\"");
		}
		startModbusPoller();
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

bool runModbusPollerTask(void *pvParameters)
{
#ifndef MODBUS_DISABLED
	// Im App-Modus darf der ESP nicht pollen (sonst zwei Master). Defensiver Abbruch,
	// falls beim Umschalten noch ein Task in der Timer-Queue steckt.
	if (appControlMode)
	{
		return true;
	}
	log(LOG_LEVEL_INFO, "Entering Modbus Poller task. ");

	if (modbus_poller_inprogress)
	{ // not sure if needed
		log(LOG_LEVEL_WARNING, "Modbus polling in already progress. Waiting for next cycle.");
		return true;
	}
	modbus_poller_inprogress = true;
	if (fillRegisterValues())
	{ // returns true if completed
		publishModbusData();
		modbus_poller_paused = true;
		modbus_poller_paused_millis = millis();
		log(LOG_LEVEL_INFO, "Pausing Modbus poller for " + String(MODBUS_SCANRATE) + " seconds to allow other Modbus masters to communicate.");
		stopModbusPoller();
	}
	String modbus_state = getModbusState();
		if (modbus_state != "" && mqtt_client.connected())
		{
			String mqtt_complete_topic = param_mqtt_topic;
			mqtt_complete_topic += "/" + String(HOSTNAME) + "/modbus_status";
			mqtt_client.publish(mqtt_complete_topic.c_str(), 0, true, modbus_state.c_str(), modbus_state.length());
		}
	modbus_poller_inprogress = false;

#endif // MODBUS_DISABLED
	return true;
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
#endif // MODBUS_DISABLED
}
int i = 0;
void loop()
{
	loopWebserver();
	// log(4, "Entering main loop : "+String(i++));
	wifi_reconnect_timer.tick();
	mqtt_reconnect_timer.tick();
	modbus_poller_timer.tick();
	// if Modbus poller is paused, check if we can restart it
	int actualMillis = millis() - modbus_poller_paused_millis;
	int pauseMillis = MODBUS_SCANRATE * 1000;
	if (!appControlMode && modbus_poller_paused && (actualMillis > pauseMillis))
	{
		log(LOG_LEVEL_INFO, "Modbus poller pause time elapsed (" + String(actualMillis) + " milliseconds since paused). Pause millis wanted: " + String(pauseMillis));
		log(LOG_LEVEL_INFO, "Resuming Modbus poller after pause of " + String(MODBUS_SCANRATE) + " seconds.");
		modbus_poller_paused = false;
		startModbusPoller();
	}
	memory_report_timer.tick();
}
