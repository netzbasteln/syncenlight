// SYNCENLIGHT BY NETZBASTELN
// ZF18 Workshop version
#define VERSION "0.6-nb"

#include <FS.h>                   // File system, this needs to be first.
#include <ESP8266WiFi.h>          // ESP8266 Core WiFi Library
#include <DNSServer.h>            // Local DNS Server used for redirecting all requests to the configuration portal
#include <ESP8266WebServer.h>     // Local WebServer used to serve the configuration portal
#include <WiFiManager.h>          // https://github.com/tzapu/WiFiManager WiFi Configuration Magic
#include <Adafruit_NeoPixel.h>    // LED
#include <PubSubClient.h>         // MQTT client
#include <ArduinoJson.h>          // https://github.com/bblanchon/ArduinoJson


#include <ESP8266HTTPClient.h>
#include <ESP8266httpUpdate.h>

#include <Ticker.h>


//---------------------------------------------------------
// Pins
#define BUTTON_PIN 0 // D3
#define PIXEL_PIN 4 // D2

// Defaults
char mqtt_server[40] = "netzbasteln.de";
char publish_topic[40] = "syncenlight";

unsigned int brightness = 255; // 0-255


//---------------------------------------------------------
bool shouldSaveConfig = false;
void saveConfigCallback () {
  Serial.println("Should save config");
  shouldSaveConfig = true;
}


WiFiManager wifiManager;
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

Adafruit_NeoPixel leds = Adafruit_NeoPixel(2, PIXEL_PIN, NEO_RGBW + NEO_KHZ800); // NEO_RGBW for Wemos Mini LED Modules, NEO_GRB for most Stripes 

uint16_t hue = 0; // 0-359
extern const uint8_t gamma8[];

bool buttonState;
bool lastButtonState;

Ticker blinkTicker;
bool blinkTickerOn = false;
uint32_t blinkColor;


String chipId = String(ESP.getChipId(), HEX);
char chip_id_char_arr[7];



void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println(".");
  Serial.println(".");
  
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  leds.begin();
  leds.setBrightness(brightness);
  blinkColor = leds.Color(10, 10, 10);
  blinkTicker.attach(1, blinkLed);
  
  chipId.toUpperCase();
  chipId.toCharArray(chip_id_char_arr, 7);
  String ssid = "SYNCENLIGHT-" + chipId;
  int ssid_char_arr_size = ssid.length() + 1;
  char ssid_char_arr[ssid_char_arr_size];
  ssid.toCharArray(ssid_char_arr, ssid_char_arr_size);

  Serial.println("Hi!");
  Serial.print("Version: Syncenlight ");
  Serial.println(VERSION);
  Serial.print("Chip ID: ");
  Serial.println(chipId);


  // Read configuration from FS json.
  Serial.println("Mounting FS ...");
  // Clean FS, for testing
  //SPIFFS.format();
  if (SPIFFS.begin()) {
    Serial.println("Mounted file system.");
    if (SPIFFS.exists("/config.json")) {
      // File exists, reading and loading.
      Serial.println("Reading config file.");
      File configFile = SPIFFS.open("/config.json", "r");
      if (configFile) {
        Serial.println("Opened config file.");
        size_t size = configFile.size();
        // Allocate a buffer to store contents of the file.
        std::unique_ptr<char[]> buf(new char[size]);
        configFile.readBytes(buf.get(), size);
        DynamicJsonBuffer jsonBuffer;
        JsonObject& json = jsonBuffer.parseObject(buf.get());
        json.printTo(Serial);
        if (json.success()) {
          Serial.println("\nParsed json.");
          strcpy(mqtt_server, json["mqtt_server"]);
          strcpy(publish_topic, json["publish_topic"]);
        } else {
          Serial.println("Failed to load json config.");
        }
        configFile.close();
      }
    }
  } else {
    Serial.println("Failed to mount FS.");
  }
  //end read

  // The extra parameters to be configured.
  WiFiManagerParameter custom_mqtt_server("Server", "mqtt server", mqtt_server, 40);
  WiFiManagerParameter custom_publish_topic("Channel", "channel", publish_topic, 40);
  
  wifiManager.setSaveConfigCallback(saveConfigCallback);
  // Add all parameters.
  wifiManager.addParameter(&custom_mqtt_server);
  wifiManager.addParameter(&custom_publish_topic);


  // When button is pressed on start, go into config portal.
  if (digitalRead(BUTTON_PIN) == LOW) {
    blinkTicker.attach(0.1, blinkLed);
    wifiManager.startConfigPortal(ssid_char_arr);
    blinkTicker.attach(1, blinkLed);
  }
  else {
    wifiManager.autoConnect(ssid_char_arr);    
  }

  // We are connected.
  Serial.println("WiFi Connected.");


  // Read updated parameters.
  strcpy(mqtt_server, custom_mqtt_server.getValue());
  strcpy(publish_topic, custom_publish_topic.getValue());

  // Save the custom parameters to FS.
  if (shouldSaveConfig) {
    Serial.println("Saving config.");
    DynamicJsonBuffer jsonBuffer;
    JsonObject& json = jsonBuffer.createObject();
    json["mqtt_server"] = mqtt_server;
    json["publish_topic"] = publish_topic;
    File configFile = SPIFFS.open("/config.json", "w");
    if (!configFile) {
      Serial.println("Failed to open config file for writing.");
    }
    json.printTo(Serial);
    json.printTo(configFile);
    configFile.close();
  }
  // End save.
 

  // Start MQTT client.
  mqttClient.setServer(mqtt_server, 1883);
  mqttClient.setCallback(mqtt_callback);
  Serial.println("MQTT client started.");
}


void loop() {
  // Debounce button
  buttonState = digitalRead(BUTTON_PIN);
  if (buttonState == LOW) {
    hue = (hue + 1) % 360;
    updateLed();
    delay(15);
  }
  if (lastButtonState == LOW && buttonState == HIGH) {
    Serial.print("New Color: ");
    Serial.print(hue);
    Serial.println();

    // Send.
    char payload[1];
    itoa(hue, payload, 10);
    mqttClient.publish(publish_topic, payload, true);
  }
  lastButtonState = buttonState;

  if (!mqttClient.connected()) {
    blinkTicker.attach(1, blinkLed);
    mqtt_reconnect();
  }
  mqttClient.loop();
}


void mqtt_callback(char* topic, byte* payload, unsigned int length) {
  Serial.print("Received [");
  Serial.print(topic);
  Serial.print("]: ");
  for (int i = 0; i < length; i++) {
    Serial.print((char)payload[i]);
  }
  Serial.print(" (length ");
  Serial.print(length);
  Serial.print(")");
  Serial.println();

  // Update
  if (length <= 3) {
    payload[length] = '\0';
    String s = String((char*)payload);
    unsigned int newHue = s.toInt();
    if (newHue >= 0 && newHue < 360) {
      hue = newHue;
      updateLed();
    }
  }
}


void mqtt_reconnect() {
  while (!mqttClient.connected()) {
    Serial.println("Connecting MQTT...");
    if (mqttClient.connect(chip_id_char_arr)) {
      blinkTicker.detach();
      Serial.println("MQTT connected.");
      mqttClient.subscribe(publish_topic, 1); // QoS level 1
    }
    else {
      Serial.print("Error, rc=");
      Serial.print(mqttClient.state());
      delay(5000);
    }
  }
}


void updateLed() {
  uint32_t color = HSVtoRGB(hue, 255, 255);
  for (uint16_t i=0; i<leds.numPixels(); i++) {
    leds.setPixelColor(i, color);
  }
  leds.show();
}



void blinkLed() {
  if (blinkTickerOn) {
    leds.clear(); 
  }
  else {
    for (uint16_t i=0; i<leds.numPixels(); i++) {
      leds.setPixelColor(i, blinkColor);
    }
  }
  leds.show();
  blinkTickerOn = !blinkTickerOn;
}


// hue: 0-359, sat: 0-255, val (lightness): 0-255
uint32_t HSVtoRGB(unsigned int hue, unsigned int sat, unsigned int val) {
  int r, g, b, base;
  if (sat == 0) {
    r = g = b = val;
  }
  else {
    base = ((255 - sat) * val) >> 8;
    switch (hue / 60) {
      case 0:
        r = val;
        g = (((val - base) * hue) / 60) + base;
        b = base;
        break;
      case 1:
        r = (((val - base) * (60 - (hue % 60))) / 60) + base;
        g = val;
        b = base;
        break;
      case 2:
        r = base;
        g = val;
        b = (((val - base) * (hue % 60)) / 60) + base;
        break;
      case 3:
        r = base;
        g = (((val - base) * (60 - (hue % 60))) / 60) + base;
        b = val;
        break;
      case 4:
        r = (((val - base) * (hue % 60)) / 60) + base;
        g = base;
        b = val;
        break;
      case 5:
        r = val;
        g = base;
        b = (((val - base) * (60 - (hue % 60))) / 60) + base;
        break;
    }
  }
  
  return leds.Color(
    pgm_read_byte(&gamma8[r]),
    pgm_read_byte(&gamma8[g]),
    pgm_read_byte(&gamma8[b]));
}


// Gamma correction curve
// https://learn.adafruit.com/led-tricks-gamma-correction/the-quick-fix
const uint8_t PROGMEM gamma8[] = {
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  2,  2,  2,  2,  2,  2,  2,
    2,  3,  3,  3,  3,  3,  3,  3,  4,  4,  4,  4,  4,  5,  5,  5,
    5,  6,  6,  6,  6,  7,  7,  7,  7,  8,  8,  8,  9,  9,  9, 10,
   10, 10, 11, 11, 11, 12, 12, 13, 13, 13, 14, 14, 15, 15, 16, 16,
   17, 17, 18, 18, 19, 19, 20, 20, 21, 21, 22, 22, 23, 24, 24, 25,
   25, 26, 27, 27, 28, 29, 29, 30, 31, 32, 32, 33, 34, 35, 35, 36,
   37, 38, 39, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 50,
   51, 52, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 66, 67, 68,
   69, 70, 72, 73, 74, 75, 77, 78, 79, 81, 82, 83, 85, 86, 87, 89,
   90, 92, 93, 95, 96, 98, 99,101,102,104,105,107,109,110,112,114,
  115,117,119,120,122,124,126,127,129,131,133,135,137,138,140,142,
  144,146,148,150,152,154,156,158,160,162,164,167,169,171,173,175,
  177,180,182,184,186,189,191,193,196,198,200,203,205,208,210,213,
  215,218,220,223,225,228,231,233,236,239,241,244,247,249,252,255
};
