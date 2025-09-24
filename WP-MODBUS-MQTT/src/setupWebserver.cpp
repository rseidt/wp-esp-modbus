#include "setupWebserver.h"

#if defined(ARDUINO_ARCH_ESP32)
WebServer server(80);
#elif defined(ARDUINO_ARCH_ESP8266)
ESP8266WebServer server(80);
#endif

void handleRoot()
{

	String content = "<html><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">";
	content += "<link rel=\"icon\" href=\"data:,\">";
	content += "<style>body { font-family: Arial; text-align: center;}</style>";
	content += "</head><body><h1>Reconfigure modbus bridge</h1>";
	content += "<p>Click <a href=\"/reconfigure\">here</a> to reconfigure modbus bridge.</p>";
	content += "<p>Click <a href=\"/update\">here</a> to update Firmware.</p>";
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

void loopWebserver()
{
	server.handleClient();
}

void setupWebserver()
{
	server.on("/", handleRoot);
	server.on("/reconfigure", handleReconfigure);
	server.on("/update", handleUploadForm);
	server.on("/uploadFirmware", HTTP_POST, []()
	{
        server.send(200, "text/plain", (Update.hasError()) ? "FAIL" : "OK");
        ESP.restart();
	}, handleUpload);
	server.begin();
	log(LOG_LEVEL_INFO, "Webserver started on port 80");
}
