#include "setupReconfigureServer.h"
#include <ESP8266WiFi.h>
#include "setupWifiManager.h"

WiFiServer server(80);


void setupReconfigureServer()
{
    server.begin();
}

void loopReconfigureServer()
{
    WiFiClient client = server.available();   // listen for incoming clients

    if (client) {                             // if you get a client,
        log(LOG_LEVEL_INFO,"New Client.");          // print a message out the serial port
        String currentLine = "";                // make a String to hold incoming data from the client
        while (client.connected()) {            // loop while the client's connected
            if (client.available()) {             // if there's bytes to read from the client,
                String line = client.readStringUntil('\n');             // read a byte, then
                line.trim();
                log(LOG_LEVEL_INFO,line);                    // print it out the serial monitor
                if (line.indexOf("GET / ") > -1) {                    // if the byte is a newline character
                    while(line.length() > 1) {
                        line = client.readStringUntil('\n');
                        line.trim(); // http does newline with \r\n, so the \r remains
                        log(LOG_LEVEL_INFO,line);
                    }
                    // if the current line is blank, you got two newline characters in a row.
                    // that's the end of the client HTTP request, so send a response:
                        // HTTP headers always start with a response code (e.g. HTTP/1.1 200 OK)
                        // and a content-type so the client knows what's coming, then a blank line:
                        client.println("HTTP/1.1 200 OK");
                        client.println("Content-type:text/html");
                        client.println();

                        // the content of the HTTP response follows the header:
                        client.print("<html><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">");
                        client.print("<link rel=\"icon\" href=\"data:,\">");
                        client.print("<style>body { font-family: Arial; text-align: center;}</style>");
                        client.print("</head><body><h1>Reconfigure modbus bridge</h1>");
                        client.print("<p>Click <a href=\"/reconfigure\">here</a> to reconfigure modbus bridge.</p>");
                        client.print("</body></html>");

                        // The HTTP response ends with another blank line:
                        client.println();
                        
                client.stop(100);
                log(LOG_LEVEL_INFO,"Client disconnected");
                        // break out of the while loop:
                } else if (line.indexOf("GET /reconfigure ") > -1) {
                    while(line.length() > 0) {
                        line = client.readStringUntil('\n');
                        line.trim();
                        log(LOG_LEVEL_INFO,line);
                    }
                    log(LOG_LEVEL_INFO,"Reconfigure requested");
                    client.println("HTTP/1.1 200 Found");
                        client.println("Content-type:text/html");
                        client.println();

                        client.print("<html><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">");
                        client.print("<link rel=\"icon\" href=\"data:,\">");
                        client.print("<style>body { font-family: Arial; text-align: center;}</style>");
                        client.print("</head><body><h1>Captive portal started</h1>");
                        client.print("<p>Connect to AP <code>APAutoConnect</code> with password <code>password</code> to reconfigure</p>");
                        client.print("</body></html>");

                    client.stop(100);
                    setupWifiManager(true);
                } else {
                    while(line.length() > 0) {
                        line = client.readStringUntil('\n');
                        line.trim();
                        log(LOG_LEVEL_INFO,line);
                    }
                    log(LOG_LEVEL_INFO,"Reconfigure requested");
                    client.println("HTTP/1.1 404 NotFound");
                    client.println();
                    client.println();
                client.stop(100);
                log(3,"Client disconnected");
                }
                break;
            }
        }
    }
}
    