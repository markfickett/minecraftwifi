/*
 * Simple HTTP get webclient test
 * For Adafruit's HUZZAH ESP8266 breakout (which suports 2.4GHz only):
 *     https://learn.adafruit.com/adafruit-huzzah-esp8266-breakout/overview
 * To prepare for upload, hold GPIO, then tap Reset. After upload, tap reset again.
 *
 * A logic level shifter is not strictly required. A Sparkfun level shifter such as
 *     https://www.sparkfun.com/products/retired/8745
 * can be used to supply a 5v logic signal to the LED from the ESP's 3.3v logic output,
 * however the WS2812 controllers seem to accept a 3.3v signal fine too.
 *
 * Program using a 5V USB FTDI to supply 5V to LEDs. Using a 3.3v FTDI works too, the
 * LEDs are just dimmer and substantially redder.
 *
 * Tested with ESP board def v2.5.2 and NeoPixel library v1.2.3.
 */

#include <ESP8266WiFi.h>
// https://arduinojson.org/v6/example/parser/
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>

// Defines ssid, password, host, and url.
#include "config.h"

#define PIN_BUILTIN_LED 0

#define FETCH_PERIOD_MS 5000
#define MAX_JSON_SIZE 1024
StaticJsonDocument<MAX_JSON_SIZE> jsonDoc;
// default value for the ServerStatus JsonArray ref
JsonArray emptyArray = jsonDoc.to<JsonArray>();
int jsonBufferPos;
char jsonBuffer[MAX_JSON_SIZE];

#define NEO_PIXEL_PIN 14
#define NUM_LEDS 9
Adafruit_NeoPixel strip = Adafruit_NeoPixel(
    NUM_LEDS, NEO_PIXEL_PIN, NEO_GRB + NEO_KHZ800);
const uint32_t COLOR_OFF = strip.Color(0, 0, 0);
const uint32_t COLOR_CONNECTED = strip.Color(0, 50, 100);
const uint32_t COLOR_ERROR = strip.Color(255, 0, 0);

void blink(int repeats) {
  for(int i = 0; i < repeats; i++) {
    digitalWrite(PIN_BUILTIN_LED, LOW); // The HUZZAH's builtin LED is inverted.
    delay(50);
    digitalWrite(PIN_BUILTIN_LED, HIGH);
    delay(200);
  }
}

void connectWifiAndPrintConnectionInfo() {
  Serial.print(F("\n\nConnecting to "));
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

/** Run a spot of this color along the LED strip. */
void strobeColor(uint32_t color) {
  for (int i = 0; i <= NUM_LEDS; i++) {
    strip.setPixelColor(i, color);
    strip.show();
    delay(200);
    strip.setPixelColor(i, COLOR_OFF);
  }
  strip.show();
}

struct ServerStatus {
  boolean error;
  int numOnline;
  // The array is owned by the JsonDocument.
  JsonArray& playersSample;

  ServerStatus(boolean err, int num):
    error(err), numOnline(num), playersSample(emptyArray) {}

  static struct ServerStatus ofError() {
    return ServerStatus(true, 0);
  }
};

struct ServerStatus fetchServerStatus() {
  Serial.print(F("connecting to "));
  Serial.println(host);

  // Use WiFiClient class to create TCP connections
  WiFiClient client;
  const int httpPort = 80;
  if (!client.connect(host, httpPort)) {
    Serial.println(F("connection failed"));
    return ServerStatus::ofError();
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
    char b = (char) client.read(); // OK to cast byte to char b/c available() protects from EOF
    if (b == '{') {
      inBraces++;
    }
    Serial.print(b); // Print everything to Serial for debugging.
    if (inBraces) {
      jsonBuffer[jsonBufferPos++] = b;
      if (jsonBufferPos >= MAX_JSON_SIZE) {
        Serial.println(F("Ran out of room for JSON."));
        return ServerStatus::ofError();
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
    return ServerStatus::ofError();
  }

  long numOnline = 0;
  if (jsonDoc[F("players")] == NULL) {
    Serial.println(F("Nobody online."));
    return ServerStatus::ofError();
  } else {
    numOnline = jsonDoc[F("players")][F("online")];
    Serial.print(F("Online: "));
    Serial.println(numOnline);
    struct ServerStatus status(false, numOnline);
    status.playersSample = jsonDoc[F("players")][F("sample")];
    return status;
  }
}

void setup() {
  pinMode(PIN_BUILTIN_LED, OUTPUT);
  blink(5);

  strip.begin(); // includes setting NEO_PIXEL_PIN as OUTPUT

  Serial.begin(115200);
  delay(100);

  connectWifiAndPrintConnectionInfo();

  strobeColor(COLOR_CONNECTED);
}

void loop() {
  struct ServerStatus status = fetchServerStatus();

  if (status.error) {
    strobeColor(COLOR_ERROR);
  } else {
    blink(status.numOnline);

    for (int i = 0; i < status.playersSample.size(); i++) {
      const char* name = status.playersSample[i][F("name")];
      Serial.println(name);
    }

    for (int i = 0; i < NUM_LEDS; i++) {
      strip.setPixelColor(i, status.numOnline > i ? strip.Color(255, 255, 50) : COLOR_OFF);
    }
    strip.show();
  }

  delay(FETCH_PERIOD_MS);
}
