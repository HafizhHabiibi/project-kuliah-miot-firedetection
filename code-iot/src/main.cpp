#include <Arduino.h>
#include <WiFi.h>
#include <DHT.h>
#include <Firebase_ESP_Client.h>

#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"

#define WIFI_SSID      "Hendar"
#define WIFI_PASSWORD  "ririmimiciko"

#define API_KEY        "AIzaSyD3xW4gGitmCiJ-6_p3EYWHLZhLJcAYLVQ"
#define DATABASE_URL   "https://miot-miniproject-firedetection-default-rtdb.asia-southeast1.firebasedatabase.app"

#define MQ2_PIN      32
#define DHT_PIN      4
#define BUZZER_PIN   15

#define DHT_TYPE DHT22
DHT dht(DHT_PIN, DHT_TYPE);

const float TEMP_THRESHOLD  = 35.0;

// Threshold
const int GAS_NORMAL = 2500;
const int GAS_LOW    = 3200;
const int GAS_MEDIUM = 3500;

const unsigned long SENSOR_INTERVAL = 2000;
const unsigned long BUZZER_INTERVAL = 200;

// Firebase objects
FirebaseData   fbdo;
FirebaseAuth   auth;
FirebaseConfig config;

bool firebaseReady = false;

// Timing
unsigned long previousSensorMillis = 0;
unsigned long previousBuzzerMillis = 0;
bool buzzerState = false;

struct SensorData {
  int           gasValue;
  float         temperature;
  float         humidity;
  String        smokeLevel;
  String        status;
  unsigned long timestamp;
};

SensorData sensor;
SensorData previousSensor;
bool       firstRead = true;

void connectWiFi() {
  Serial.print("Menghubungkan ke WiFi");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int retry = 0;
  while (WiFi.status() != WL_CONNECTED && retry < 20) {
    Serial.print(".");
    delay(500);
    retry++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.println("WiFi Terhubung!");
    Serial.print("IP Address : ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println();
    Serial.println("WiFi GAGAL — sistem tetap berjalan offline.");
  }
}

void checkWiFi() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi terputus, mencoba reconnect...");
    connectWiFi();
  }
}

void syncTime() {
  Serial.print("Sinkronisasi waktu NTP");

  configTime(0, 0, "pool.ntp.org", "time.nist.gov", "id.pool.ntp.org");  

  struct tm timeinfo;                                                    
  int retry = 0;
  while (!getLocalTime(&timeinfo, 1000) && retry < 30) {                 
    Serial.print(".");
    retry++;                                                             
  }

  if (retry < 30) {
    Serial.println();
    Serial.print("Waktu tersinkron: ");
    Serial.println(&timeinfo, "%Y-%m-%d %H:%M:%S");                      
  } else {
    Serial.println();
    Serial.println("Sinkronisasi waktu GAGAL — SSL Firebase bisa gagal.");
  }
}

void initFirebase() {
  config.api_key      = API_KEY;
  config.database_url = DATABASE_URL;
 
  config.token_status_callback = tokenStatusCallback;
  if (Firebase.signUp(&config, &auth, "", "")) {
    Serial.println("Anonymous sign-up request berhasil dikirim.");
  } else {
    Serial.print("Sign-up error: ");
    Serial.println(config.signer.signupError.message.c_str());
  }
 
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
 
  Serial.print("Menunggu Firebase token");
  unsigned long tokenWait = millis();
  while (!Firebase.ready() && millis() - tokenWait < 10000) {
    Serial.print(".");
    delay(500);
  }
 
  if (Firebase.ready()) {
    firebaseReady = true;
    Serial.println();
    Serial.println("Firebase Terhubung!");
  } else {
    Serial.println();
    Serial.println("Firebase GAGAL — upload dinonaktifkan.");
  }
}

int readGasValue() {
  long total = 0;
  for (int i = 0; i < 10; i++) {
    total += analogRead(MQ2_PIN);
  }
  return total / 10;
}

String getSmokeLevel(int gasValue) {
  if (gasValue < GAS_NORMAL) return "Normal";
  if (gasValue < GAS_LOW)    return "Rendah";
  if (gasValue < GAS_MEDIUM) return "Sedang";
  return "Tinggi";
}

void updateSensors() {
  int   gas  = readGasValue();
  float temp = dht.readTemperature();
  float hum  = dht.readHumidity();

  if (isnan(temp) || isnan(hum)) {
    Serial.println("[ERROR] Gagal membaca DHT22! Cek wiring.");
    return;
  }

  sensor.gasValue    = gas;
  sensor.temperature = temp;
  sensor.humidity    = hum;
  sensor.smokeLevel  = getSmokeLevel(gas);
  sensor.timestamp   = millis();
}

void determineStatus() {
  if (sensor.smokeLevel == "Tinggi" && sensor.temperature > TEMP_THRESHOLD) {
    sensor.status = "Potensi Kebakaran";
  } else if (sensor.smokeLevel == "Sedang" || sensor.smokeLevel == "Tinggi") {
    sensor.status = "Asap Terdeteksi";
  } else {
    sensor.status = "Aman";
  }
}

bool dataChanged() {
  if (firstRead)                                          return true;
  if (sensor.gasValue    != previousSensor.gasValue)     return true;
  if (sensor.status      != previousSensor.status)       return true;
  if (abs(sensor.temperature - previousSensor.temperature) > 0.3) return true;
  if (abs(sensor.humidity    - previousSensor.humidity)    > 1.0) return true;
  return false;
}

void uploadFirebase() {
  if (!firebaseReady) {
    Serial.println("[Firebase] Tidak terhubung, skip upload.");
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[Firebase] WiFi offline, skip upload.");
    return;
  }

  // --- Buat JSON object ---
  FirebaseJson json;
  json.set("gasValue",    sensor.gasValue);
  json.set("temperature", sensor.temperature);
  json.set("humidity",    sensor.humidity);
  json.set("smokeLevel",  sensor.smokeLevel);
  json.set("status",      sensor.status);
  json.set("timestamp",   (int)sensor.timestamp);

  // --- Upload ke /sensor/latest ---
  if (Firebase.RTDB.setJSON(&fbdo, "/sensor/latest", &json)) {
    Serial.println("[Firebase] /sensor/latest updated.");
  } else {
    Serial.print("[Firebase] Gagal update latest: ");
    Serial.println(fbdo.errorReason());
  }

  // --- Push ke /sensor/history ---
  // pushJSON otomatis buat key unik berdasarkan timestamp Firebase
  if (Firebase.RTDB.pushJSON(&fbdo, "/sensor/history", &json)) {
    Serial.print("[Firebase] History pushed, key: ");
    Serial.println(fbdo.pushName());
  } else {
    Serial.print("[Firebase] Gagal push history: ");
    Serial.println(fbdo.errorReason());
  }
}

void printSensorData() {
  Serial.println();
  Serial.println("========== DATA SENSOR ==========");
  Serial.print("Gas Value    : ");
  Serial.println(sensor.gasValue);
  Serial.print("Smoke Level  : ");
  Serial.println(sensor.smokeLevel);
  Serial.print("Temperature  : ");
  Serial.print(sensor.temperature);
  Serial.println(" C");
  Serial.print("Humidity     : ");
  Serial.print(sensor.humidity);
  Serial.println(" %");
  Serial.print("Status       : ");
  Serial.println(sensor.status);
  Serial.println("=================================");
}

void updateBuzzer() {
  if (sensor.status == "Aman") {
    digitalWrite(BUZZER_PIN, LOW);
    buzzerState = false;
    return;
  }

  unsigned long currentMillis = millis();
  if (currentMillis - previousBuzzerMillis >= BUZZER_INTERVAL) {
    previousBuzzerMillis = currentMillis;
    buzzerState = !buzzerState;
    digitalWrite(BUZZER_PIN, buzzerState);
  }
}

void setup() {
  Serial.begin(115200);

  pinMode(MQ2_PIN,    INPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  dht.begin();

  Serial.println();
  Serial.println("============================================");
  Serial.println("  SMART FIRE DETECTION SYSTEM AND MONITORING");
  Serial.println("============================================");

  connectWiFi();
  syncTime();
  initFirebase();

  updateSensors();
  determineStatus();
  previousSensor = sensor;
  firstRead = true; 
}

void loop() {
  checkWiFi();

  unsigned long currentMillis = millis();

  if (currentMillis - previousSensorMillis >= SENSOR_INTERVAL) {
    previousSensorMillis = currentMillis;

    updateSensors();
    determineStatus();
    printSensorData();

    if (dataChanged()) {
      uploadFirebase();
      previousSensor = sensor;
      firstRead = false;
    }
  }

  updateBuzzer();
}