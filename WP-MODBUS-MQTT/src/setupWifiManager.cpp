
#include "setupWifiManager.h"
#include "log.h"

char param_mqtt_server[40];
char param_mqtt_port[6] = "8080";
char param_mqtt_topic[50] = "esp/modbus";

#define FORMAT_LITTLEFS_IF_FAILED true

// flag for saving data
bool shouldSaveConfig = false;

// callback notifying us of the need to save config
void saveConfigCallback()
{
	log(LOG_LEVEL_INFO, "Should save config");
	shouldSaveConfig = true;
}

void setupWifiManager(bool forceConfigPortal)
{
	// put your setup code here, to run once:

	// clean FS, for testing
	// LittleFS.format();

	// local instantiation: once it's job is done there's no need to keep it.
	WiFiManager wifiManager;

	if (forceConfigPortal)
	{
		wifiManager.resetSettings();
		log(LOG_LEVEL_INFO, "Erased WiFi settings, config fields kept. Rebooting to launch captive portal");
		delay(1000);
		ESP.restart();
	}

	// read configuration from FS json
	log(LOG_LEVEL_INFO, "mounting FS...");
	bool fsOk = false;
#if defined(ARDUINO_ARCH_ESP32)
	fsOk = LittleFS.begin(FORMAT_LITTLEFS_IF_FAILED);
#elif defined(ARDUINO_ARCH_ESP8266)
	fsOk = LittleFS.begin();
#endif
	if (!fsOk)
	{
		log(LOG_LEVEL_ERROR, "LittleFS Mount Failed");
		return;
	}

	log(LOG_LEVEL_INFO, "mounted file system");
	// file exists, reading and loading
	log(LOG_LEVEL_INFO, "reading config file");
	File configFile = LittleFS.open("/config.json", "r");
	if (configFile)
	{
		log(LOG_LEVEL_INFO, "opened config file");
		size_t size = configFile.size();
		log(LOG_LEVEL_INFO, "Size: " + String(size));
		// Allocate a buffer to store contents of the file.
		std::unique_ptr<char[]> buf(new char[size]);

		configFile.readBytes(buf.get(), size);
		JsonDocument json;
		auto deserializeError = deserializeJson(json, buf.get());
		serializeJson(json, Serial);
		if (!deserializeError)
		{

			log(LOG_LEVEL_INFO, "\nparsed json");
			strcpy(param_mqtt_server, json["mqtt_server"]);
			strcpy(param_mqtt_port, json["mqtt_port"]);
			strcpy(param_mqtt_topic, json["mqtt_topic"]);
		}
		else
		{
			log(LOG_LEVEL_ERROR, "failed to load json config");
		}
		configFile.close();
	}
	LittleFS.end();
	// end read

	// The extra parameters to be configured (can be either global or just in the setup)
	// After connecting, parameter.getValue() will get you the configured value
	// id/name placeholder/prompt default length
	WiFiManagerParameter custom_mqtt_server("server", "mqtt server", param_mqtt_server, 40);
	WiFiManagerParameter custom_mqtt_port("port", "mqtt port", param_mqtt_port, 6);
	WiFiManagerParameter custom_mqtt_topic("topic", "mqtt topic", param_mqtt_topic, 50);

	// WiFiManager
	// Local intialization. Once its business is done, there is no need to keep it around

	// set config save notify callback
	wifiManager.setSaveConfigCallback(saveConfigCallback);

	// set static ip
	//   wifiManager.setSTAStaticIPConfig(IPAddress(10, 0, 1, 99), IPAddress(10, 0, 1, 1), IPAddress(255, 255, 255, 0));

	// add all your parameters here
	wifiManager.addParameter(&custom_mqtt_server);
	wifiManager.addParameter(&custom_mqtt_port);
	wifiManager.addParameter(&custom_mqtt_topic);

	// reset settings - for testing
	// wifiManager.resetSettings();

	// set minimu quality of signal so it ignores AP's under that quality
	// defaults to 8%
	// wifiManager.setMinimumSignalQuality();

	// sets timeout until configuration portal gets turned off
	// useful to make it all retry or go to sleep
	// in seconds
	// wifiManager.setTimeout(120);

	// fetches ssid and pass and tries to connect
	// if it does not connect it starts an access point with the specified name
	// here  "AutoConnectAP"
	// and goes into a blocking loop awaiting configuration
	log(LOG_LEVEL_INFO, "Auto connect");
	if (!wifiManager.autoConnect("AutoConnectAP", "password"))
	{
		log(LOG_LEVEL_ERROR, "failed to connect and hit timeout");
		delay(3000);
		// reset and try again, or maybe put it to deep sleep
		ESP.restart();
		delay(5000);
	}
	// if you get here you have connected to the WiFi
	log(LOG_LEVEL_INFO, "connected...yeey :)");

	// read updated parameters
	strncpy(param_mqtt_server, custom_mqtt_server.getValue(), 40);
	strncpy(param_mqtt_port, custom_mqtt_port.getValue(), 6);
	strncpy(param_mqtt_topic, custom_mqtt_topic.getValue(), 50);

	log(LOG_LEVEL_INFO, "The values in the file are: ");
	log(LOG_LEVEL_INFO, "\tmqtt_server : " + String(param_mqtt_server));
	log(LOG_LEVEL_INFO, "\tmqtt_port : " + String(param_mqtt_port));
	log(LOG_LEVEL_INFO, "\tmqtt_topic : " + String(param_mqtt_topic));

	// save the custom parameters to FS
	if (shouldSaveConfig)
	{
		log(LOG_LEVEL_INFO, "saving config");
		JsonDocument json;
		json["mqtt_server"] = param_mqtt_server;
		json["mqtt_port"] = param_mqtt_port;
		json["mqtt_topic"] = param_mqtt_topic;
		json.shrinkToFit();
		if (LittleFS.begin())
		{
			File configFile = LittleFS.open("/config.json", "w");
			if (!configFile)
			{
				log(LOG_LEVEL_ERROR, "failed to open config file for writing");
			}

			serializeJson(json, Serial);
			serializeJson(json, configFile);
			configFile.close();
			LittleFS.end();
		}
		else
		{
			log(LOG_LEVEL_ERROR, "Save Config: failed to mount FS");
		}
		// end save
	}

	log(LOG_LEVEL_WARNING, "local ip");
	log(LOG_LEVEL_WARNING, WiFi.localIP().toString());

	configTime(0, 0, "pool.ntp.org"); // init UTC time
	struct tm now;
	if (getLocalTime(&now))
	{
		log(LOG_LEVEL_INFO, "Time: " + String(now.tm_year + 1900) + "-" + String(now.tm_mon + 1) + "-" + String(now.tm_mday) + " " + String(now.tm_hour) + ":" + String(now.tm_min) + ":" + String(now.tm_sec));
	}
	else
	{
		log(LOG_LEVEL_ERROR, "Failed to obtain time");
	}
}
