/*
 * Simple HTTP get webclient test
 * For Adafruit's HUZZAH ESP8266 breakout:
 * https://learn.adafruit.com/adafruit-huzzah-esp8266-breakout/overview
 * To upload, hold GPIO, then tap Reset.
 */

#include <ESP8266WiFi.h>
// https://arduinojson.org/v6/example/parser/
#include <ArduinoJson.h>

// Defines ssid, password, host, and url.
#include "config.h"

#define PIN_BUILTIN_LED 0

#define MAX_JSON_SIZE 1024
StaticJsonDocument<MAX_JSON_SIZE> jsonDoc;
int jsonBufferPos;
char jsonBuffer[MAX_JSON_SIZE];

void setup() {
  pinMode(PIN_BUILTIN_LED, OUTPUT);
  digitalWrite(PIN_BUILTIN_LED, LOW); // The HUZZAH's builtin LED is inverted.
  delay(200);
  digitalWrite(PIN_BUILTIN_LED, HIGH);
  delay(100);

  Serial.begin(115200);
  delay(100);

  // We start by connecting to a WiFi network

  Serial.println();
  Serial.println();
  Serial.print(F("Connecting to "));
  Serial.println(ssid);

  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print('.');
  }

  Serial.print('\n');
  Serial.println(F("WiFi connected"));
  Serial.println(F("IP address: "));
  Serial.println(WiFi.localIP());
  Serial.print(F("Netmask: "));
  Serial.println(WiFi.subnetMask());
  Serial.print(F("Gateway: "));
  Serial.println(WiFi.gatewayIP());
}

void loop() {
  delay(5000);

  Serial.print(F("connecting to "));
  Serial.println(host);

  // Use WiFiClient class to create TCP connections
  WiFiClient client;
  const int httpPort = 80;
  if (!client.connect(host, httpPort)) {
    Serial.println(F("connection failed"));
    return;
  }

  // We now create a URI for the request
  Serial.print(F("Requesting URL: "));
  Serial.println(url);

  // This will send the request to the server
  client.print(String("GET ") + url + " HTTP/1.1\r\n" +
               "Host: " + host + "\r\n" +
               "Connection: close\r\n\r\n");
  Serial.println(F("Waiting for response."));
  while(!client.available()) {
    Serial.print(".");
    delay(100);
  }
  Serial.println(F("\nResponse arriving."));

  int inBraces = 0;
  jsonBufferPos = 0;
  while(client.available()){
    // Only parse things within at least some braces as JSON, to skip HTTP headers etc.
    char b = (char) client.read(); // OK to cast byte to char b/c available protects from EOF
    if (b == '{') {
      inBraces++;
    }
    Serial.print(b); // Print everything to Serial for debugging.
    if (inBraces) {
      jsonBuffer[jsonBufferPos++] = b;
      if (jsonBufferPos >= MAX_JSON_SIZE) {
        Serial.println(F("Ran out of room for JSON."));
        break;
      }
    }
    if (b == '}') {
      inBraces--;
    }
  }
  jsonBuffer[jsonBufferPos] = '\0';

  Serial.println();
  Serial.println(F("closing connection"));

  DeserializationError error = deserializeJson(jsonDoc, jsonBuffer);
  if (error) {
    Serial.print(F("deserializeJson() failed: "));
    Serial.println(error.c_str());
    return;
  }

  if (jsonDoc["players"] == NULL) {
    Serial.println(F("Nobody online."));
  } else {
    long numOnline = jsonDoc["players"]["online"];
    Serial.print(F("Online: "));
    Serial.println(numOnline);
    for (int i = 0; i < numOnline; i++) {
      digitalWrite(PIN_BUILTIN_LED, LOW); // The HUZZAH's builtin LED is inverted.
      delay(100);
      digitalWrite(PIN_BUILTIN_LED, HIGH);
      delay(100);
    }
    for (int i = 0; i < numOnline; i++) {
      const char* name = jsonDoc["players"]["sample"][i]["name"];
      Serial.println(name);
    }
  }
}
