/**
 * Fetch Minecraft server status over WiFi, and display which players are online
 * using NeoPixel addressable RGB LEDs.
 *
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
 *
 * Minecraft servers don't directly respond to HTTP, they have their own
 * protocol. This depends on a proxy which responds over HTTP with simple JSON
 * such as github.com/markfickett/minecraftstatus/blob/master/mcstatusproxy.cgi
 * A direct UDP implementation of the Minecraft server status protocol is
 * available in https://github.com/copethomas/Arduino-Minecraft-Monitor
 * which might be adapted to use ESP8266's WiFiUdp:
 * https://arduino-esp8266.readthedocs.io/en/latest/esp8266wifi/udp-examples.html
 */

#include <ESP8266WiFi.h>
// https://arduinojson.org/v6/example/parser/
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>

// Defines:
// const char* ssid = "my WiFi SSID";
// const char* password = "my WiFi password";
// const char* host = "minecraft.server.host.com";
// const char* url = "/path/for/status"; // which returns JSON
#include "network.h"

#define PIN_BUILTIN_LED 0

#define FETCH_PERIOD_MS 5000
#define MAX_JSON_SIZE 1024
#define CONNECTION_RETRIES 3
#define MAX_RESPONSE_WAIT_TIME_MS 30000
#define RESPONSE_WAIT_INTERVAL_MS 100
StaticJsonDocument<MAX_JSON_SIZE> jsonDoc;
// default value for the ServerStatus JsonArray ref
JsonArray emptyArray = jsonDoc.to<JsonArray>();
int jsonBufferPos;
char jsonBuffer[MAX_JSON_SIZE];

#define NEO_PIXEL_PIN 14
const uint32_t COLOR_OFF = Adafruit_NeoPixel::Color(0, 0, 0);
const uint32_t COLOR_CONNECTING = Adafruit_NeoPixel::Color(0, 50, 100);
const uint32_t COLOR_CONNECTED = Adafruit_NeoPixel::Color(0, 100, 0);
const uint32_t COLOR_ERROR = Adafruit_NeoPixel::Color(255, 0, 0);
uint32_t COLOR_ONLINE = Adafruit_NeoPixel::Color(200, 200, 200);
uint32_t COLOR_JOINED = Adafruit_NeoPixel::Color(10, 100, 200);
uint32_t COLOR_LEFT = Adafruit_NeoPixel::Color(255, 10, 10);

/**
 * Controls the color of an LED associated with a player's online state.
 * Tracks the historical online state so transitions can have separate colors.
 */
class PlayerLed {
  private:
    // The empty string as a name is treated as a wildcard.
    char name[32];
    // was online last cycle
    boolean wasOnline;
    // is known to be online this cycle
    boolean isOnline;

    void printName() {
      if (strlen(name) > 0) {
        Serial.print(name);
      } else {
        Serial.print(F("<unknown player>"));
      }
    }
  public:
    PlayerLed(const char* iName) {
      strcpy(name, iName);
      wasOnline = false;
      isOnline = false;
    }

    boolean matches(const char* str) {
      return strlen(name) == 0 || strcmp(str, name) == 0;
    }

    void clearOnline() {
      isOnline = false;
    }

    void setOnline() {
      isOnline = true;
    }

    uint32_t getColorAndUpdate() {
      uint32_t color;
      if (wasOnline && isOnline) {
        color = COLOR_ONLINE;
      } else if (wasOnline && !isOnline) {
        color = COLOR_LEFT;
        printName();
        Serial.println(F(" left."));
      } else if (!wasOnline && isOnline) {
        color = COLOR_JOINED;
        printName();
        Serial.println(F(" joined."));
      } else {
        color = COLOR_OFF;
      }
      wasOnline = isOnline;
      return color;
    }
};

// There must be a players.h with
//     #define NUM_PLAYERS ??
//     #define NUM_LEDS ??
// and
//     PlayerLed players[NUM_PLAYERS] = {PlayerLed("name1"), ..., PlayerLed("" /* wildcard */)};
// If the number of PlayerLed initializations does not match NUM_PLAYERS, the compiler
// will attempt to use the default constructor for any extras required and thus fail.
#include "players.h"
static_assert(NUM_PLAYERS <= NUM_LEDS, "Not enough LEDs for all players.");

Adafruit_NeoPixel strip = Adafruit_NeoPixel(
    NUM_LEDS, NEO_PIXEL_PIN, NEO_GRB + NEO_KHZ800);

void blink(int repeats) {
  for(int i = 0; i < repeats; i++) {
    // The HUZZAH's builtin LED is inverted. (The other pins are normal.)
    digitalWrite(PIN_BUILTIN_LED, LOW);
    delay(10);
    digitalWrite(PIN_BUILTIN_LED, HIGH);
    delay(100);
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

/** Run a spot of this color along the LED strip (for startup/error display). */
void strobeColor(uint32_t color) {
  for (int i = 0; i <= NUM_LEDS; i++) {
    strip.setPixelColor(i, color);
    strip.show();
    delay(200);
    strip.setPixelColor(i, COLOR_OFF);
  }
  strip.show();
}

/**
 * Minecraft server status container.
 */
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

/**
 * Fetch status and parse JSON.
 */
struct ServerStatus fetchServerStatus() {
  // Use WiFiClient class to create TCP connections
  WiFiClient client;
  const int httpPort = 80;
  int retries = CONNECTION_RETRIES;
  while (true) {
    Serial.print(F("connecting to "));
    Serial.println(host);
    if (client.connect(host, httpPort)) {
      break;
    } else {
      Serial.println(F("connection failed"));
      if (--retries <= 0) {
        return ServerStatus::ofError();
      }
    }
  }

  // We now create a URI for the request
  Serial.print(F("Requesting URL: "));
  Serial.println(url);

  // This will send the request to the server
  client.print(String("GET ") + url + " HTTP/1.1\r\n" +
               "Host: " + host + "\r\n" +
               "Connection: close\r\n\r\n");
  Serial.println(F("Waiting for response."));
  int waitMs = MAX_RESPONSE_WAIT_TIME_MS;
  while(!client.available()) {
    Serial.print(".");
    delay(RESPONSE_WAIT_INTERVAL_MS);
    waitMs -= RESPONSE_WAIT_INTERVAL_MS;
    if (waitMs <= 0) {
      Serial.println(F("\nNo response."));
      return ServerStatus::ofError();
    }
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

  strobeColor(COLOR_CONNECTING);
  connectWifiAndPrintConnectionInfo();
  strobeColor(COLOR_CONNECTED);
}

void loop() {
  struct ServerStatus status = fetchServerStatus();

  if (status.error) {
    strobeColor(COLOR_ERROR);
  } else {
    blink(status.numOnline);

    // Set the "online" state for (only) players online now.
    for (int p = 0; p < NUM_PLAYERS; p++) {
      players[p].clearOnline();
    }
    for (int i = 0; i < status.playersSample.size(); i++) {
      const char* name = status.playersSample[i][F("name")];
      Serial.println(name);
      // NUM_PLAYERS * playersSample.size() should be small, so just do linear search.
      for (int p = 0; p < NUM_PLAYERS; p++) {
        if (players[p].matches(name)) {
          players[p].setOnline();
          break;
        }
      }
    }
    // Update LED colors based on player state.
    for (int i = 0; i < NUM_LEDS; i++) {
      if (i < NUM_PLAYERS) {
        strip.setPixelColor(i, players[i].getColorAndUpdate());
      } else {
        strip.setPixelColor(i, COLOR_OFF);
      }
    }
    strip.show();
  }

  delay(FETCH_PERIOD_MS);
}
