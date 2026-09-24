#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include <addons/TokenHelper.h>
#include <addons/RTDBHelper.h>
#include <DHT.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// ---------- WiFi & Firebase Configuration ----------
#define WIFI_SSID "Jithmi's s21"
#define WIFI_PASSWORD "11223344"

#define FIREBASE_API_KEY "AIzaSyDPOL2Jg4yDcLeY0pwAlj_MXk_fnzbL7Nc"
#define FIREBASE_DATABASE_URL "https://green-house-by-greenlink-default-rtdb.asia-southeast1.firebasedatabase.app"
// ----------------------------------------------------

// Pin Definitions
#define DHTPIN 4
#define DHTTYPE DHT11

#define SOIL_PIN 34      // Analog pin for soil moisture sensor
#define TRIG_PIN 5       // Ultrasonic sensor Trig pin
#define ECHO_PIN 18      // Ultrasonic sensor Echo pin

#define PUMP_PIN 26      // Water Mist (Water Pump) Relay pin
#define FAN_PIN 27       // Cooling Fan Relay pin
#define LED_PIN 2        // Status LED

// ---------- Relay Logic Level ----------
// This relay module is ACTIVE-HIGH: the relay energizes (load ON)
// when the IN pin is HIGH, and de-energizes (OFF) when LOW.
#define RELAY_ON  HIGH
#define RELAY_OFF LOW

// I2C LCD Configuration
#define LCD_ADDRESS 0x27
#define LCD_COLS 16
#define LCD_ROWS 2

// Water Tank Constants (in cm)
#define TANK_FULL_CM 5.0
#define TANK_EMPTY_CM 30.0

// Threshold Constants
#define SOIL_LOW_THRESHOLD 60.0
#define SOIL_HIGH_THRESHOLD 85.0
#define TEMP_FAN_ON_THRESHOLD 30.0
#define TEMP_FAN_OFF_THRESHOLD 28.0

DHT dht(DHTPIN, DHTTYPE);
LiquidCrystal_I2C lcd(LCD_ADDRESS, LCD_COLS, LCD_ROWS);

FirebaseData fbdo;
FirebaseData fbdoHistory;   // separate FirebaseData object so a history push
                             // never collides with a live-value push
FirebaseData fbdoStream;
FirebaseAuth auth;
FirebaseConfig config;

unsigned long lastSensorPush = 0;
const unsigned long SENSOR_INTERVAL_MS = 2000;      // live values: every 2 s

unsigned long lastHistoryPush = 0;
const unsigned long HISTORY_INTERVAL_MS = 60000;    // history log: every 60 s
// Change HISTORY_INTERVAL_MS to log more/less often, e.g. 300000 for 5 min.

// System States
bool waterMistState = false;
bool fanState = false;
bool autoMode = true; // Auto control enabled by default — toggle from the dashboard to switch to manual

void streamCallback(FirebaseStream data) {
  String path = data.dataPath();
  if (path == "/water_mist" && data.dataType() == "boolean") {
    waterMistState = data.boolData();
    digitalWrite(PUMP_PIN, waterMistState ? RELAY_ON : RELAY_OFF);
    Serial.printf("[Stream] Water Mist state updated: %s\n", waterMistState ? "ON" : "OFF");
  } else if (path == "/fan" && data.dataType() == "boolean") {
    fanState = data.boolData();
    digitalWrite(FAN_PIN, fanState ? RELAY_OFF : RELAY_ON);
    Serial.printf("[Stream] Fan state updated: %s\n", fanState ? "ON" : "OFF");
  } else if (path == "/auto_mode" && data.dataType() == "boolean") {
    autoMode = data.boolData();
    Serial.printf("[Stream] Auto Mode: %s\n", autoMode ? "ENABLED" : "DISABLED");
  }
}

void streamTimeoutCallback(bool timeout) {
  if (timeout) Serial.println("Stream timeout, resuming...");
}

float readSoilMoisture() {
  int rawAnalog = analogRead(SOIL_PIN);
  int dryVal = 3500;
  int wetVal = 1400;
  float moisture = map(rawAnalog, dryVal, wetVal, 0, 100);
  return constrain(moisture, 0.0, 100.0);
}

float readWaterTankLevel() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  long duration = pulseIn(ECHO_PIN, HIGH, 30000);
  if (duration == 0) return 0.0;

  float distanceCm = (duration * 0.0343) / 2.0;
  float levelPct = map(distanceCm, TANK_EMPTY_CM, TANK_FULL_CM, 0, 100);
  return constrain(levelPct, 0.0, 100.0);
}

void updateLcd(float temp, float hum, float soilMoisture, float waterLevel) {
  lcd.clear();

  lcd.setCursor(0, 0);
  lcd.print("T:" + String(temp, 1) + "C H:" + String(hum, 0) + "%");

  lcd.setCursor(0, 1);
  lcd.print("Soil:" + String(soilMoisture, 0) + "% Tk:" + String(waterLevel, 0) + "%");
}

// Pushes one snapshot to /sensor_history/{auto-id}. Uses Firebase's server
// clock for the timestamp (.sv) since the ESP32 has no real-time clock.
void logHistory(float temp, float hum, float soilMoisture, float waterLevel) {
  FirebaseJson historyJson;
  historyJson.set("temperature", temp);
  historyJson.set("humidity", hum);
  historyJson.set("soil_moisture", soilMoisture);
  historyJson.set("water_level", waterLevel);
  historyJson.set("water_mist", waterMistState);
  historyJson.set("fan", fanState);
  historyJson.set("timestamp/.sv", "timestamp");

  if (Firebase.RTDB.pushJSON(&fbdoHistory, "/sensor_history", &historyJson)) {
    Serial.println("[History] Logged snapshot to /sensor_history");
  } else {
    Serial.println("[History] Push failed: " + fbdoHistory.errorReason());
  }
}

void setup() {
  Serial.begin(115200);

  pinMode(PUMP_PIN, OUTPUT);
  pinMode(FAN_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

  digitalWrite(PUMP_PIN, RELAY_OFF);
  digitalWrite(FAN_PIN, RELAY_OFF);
  digitalWrite(LED_PIN, LOW);

  dht.begin();

  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("GreenLink");
  lcd.setCursor(0, 1);
  lcd.print("Connecting...");

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
  }
  Serial.println("\nWiFi Connected! IP: " + WiFi.localIP().toString());

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("WiFi Connected");
  lcd.setCursor(0, 1);
  lcd.print(WiFi.localIP().toString());
  delay(1500);

  config.api_key = FIREBASE_API_KEY;
  config.database_url = FIREBASE_DATABASE_URL;

  auth.user.email = "";
  auth.user.password = "";
  Firebase.signUp(&config, &auth, "", "");

  config.token_status_callback = tokenStatusCallback;
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  if (!Firebase.RTDB.beginStream(&fbdoStream, "/control")) {
    Serial.println("Stream begin failed: " + fbdoStream.errorReason());
  }
  Firebase.RTDB.setStreamCallback(&fbdoStream, streamCallback, streamTimeoutCallback);

  digitalWrite(LED_PIN, HIGH);
}

void loop() {
  if (Firebase.ready() && millis() - lastSensorPush > SENSOR_INTERVAL_MS) {
    lastSensorPush = millis();

    float temp = dht.readTemperature();
    float hum = dht.readHumidity();
    float soilMoisture = readSoilMoisture();
    float waterLevel = readWaterTankLevel();

    if (isnan(temp) || isnan(hum)) {
      Serial.println("DHT11 sensor reading failed!");
      temp = 25.0;
      hum = 50.0;
    }

    if (autoMode) {
      if (soilMoisture < SOIL_LOW_THRESHOLD && !waterMistState) {
        waterMistState = true;
        digitalWrite(PUMP_PIN, RELAY_ON);
        Firebase.RTDB.setBool(&fbdo, "/control/water_mist", true);
        Serial.println("[AUTO] Soil Moisture < 60%. Water Mist ACTIVATED!");
      } else if (soilMoisture >= SOIL_HIGH_THRESHOLD && waterMistState) {
        waterMistState = false;
        digitalWrite(PUMP_PIN, RELAY_OFF);
        Firebase.RTDB.setBool(&fbdo, "/control/water_mist", false);
        Serial.println("[AUTO] Soil Moisture >= 85%. Water Mist DEACTIVATED!");
      }

      if (temp >= TEMP_FAN_ON_THRESHOLD && !fanState) {
        fanState = true;
        digitalWrite(FAN_PIN, RELAY_ON);
        Firebase.RTDB.setBool(&fbdo, "/control/fan", true);
        Serial.println("[AUTO] Temperature >= 30 C. Fan ACTIVATED!");
      } else if (temp <= TEMP_FAN_OFF_THRESHOLD && fanState) {
        fanState = false;
        digitalWrite(FAN_PIN, RELAY_OFF);
        Firebase.RTDB.setBool(&fbdo, "/control/fan", false);
        Serial.println("[AUTO] Temperature <= 28 C. Fan DEACTIVATED!");
      }
    }

    Firebase.RTDB.setFloat(&fbdo, "/sensor/temperature", temp);
    Firebase.RTDB.setFloat(&fbdo, "/sensor/humidity", hum);
    Firebase.RTDB.setFloat(&fbdo, "/sensor/soil_moisture", soilMoisture);
    Firebase.RTDB.setFloat(&fbdo, "/sensor/water_level", waterLevel);

    updateLcd(temp, hum, soilMoisture, waterLevel);

    Serial.printf("Temp: %.1f C | Hum: %.1f %% | Soil: %.1f %% | Tank: %.1f %% | Pump: %s | Fan: %s\n",
                  temp, hum, soilMoisture, waterLevel,
                  waterMistState ? "ON" : "OFF",
                  fanState ? "ON" : "OFF");

    // Log to history on its own, slower interval so the database doesn't
    // fill up from the fast 2-second live-value loop.
    if (millis() - lastHistoryPush > HISTORY_INTERVAL_MS) {
      lastHistoryPush = millis();
      logHistory(temp, hum, soilMoisture, waterLevel);
    }
  }
}
