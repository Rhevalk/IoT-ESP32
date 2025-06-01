#include <Arduino.h>

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <vector>

#include <NTPClient.h>

#include "DHT.h"
#define DHTPIN 4  
#define DHTTYPE DHT11  

DHT dht(DHTPIN, DHTTYPE);

#define PIN_SUHU_AIR    13
#define PIN_TDS         27
#define PIN_DEBIT       26
#define PIN_LEVEL_AIR   25

const char* ssid = "Banyak makan";
const char* password = "rhevan1119";
String serverUrl_get = ""; 
String serverUrl_post = ""; 

// waktu
WiFiUDP ntpUDP;
uint8_t jam, menit, detik;
NTPClient timeClient(ntpUDP, "time.google.com", 3600 * 7, 60000);  // 7 jam offset, update setiap 6 detik

struct Alarm {
  int hari;
  int pin;
  int jam_N, menit_N, detik_N;
  int jam_L, menit_L, detik_L;
};

std::vector<Alarm> alarms;

int8_t convertHariToInt(const String& hari) {
  if (hari == "Minggu") return 0;
  if (hari == "Senin") return 1;
  if (hari == "Selasa") return 2;
  if (hari == "Rabu") return 3;
  if (hari == "Kamis") return 4;
  if (hari == "Jumat") return 5;
  if (hari == "Sabtu") return 6;
  return -1;
}

void parseTimeFull(const String& timeStr, int& jam, int& menit, int& detik) {
  sscanf(timeStr.c_str(), "%d:%d:%d", &jam, &menit, &detik);
}

void loadAlarmsFromJson(JsonDocument& doc) {
  alarms.clear();
  JsonArray array = doc["jadwal"];

  for (JsonObject obj : array) {
    Alarm a;
    String hariStr = obj["hari"] | "";
    a.hari = convertHariToInt(hariStr);
    a.pin = obj["pin"].as<int>();

    String start = obj["start"] | "00:00:00";
    String end = obj["end"] | "00:00:00";
    parseTimeFull(start, a.jam_N, a.menit_N, a.detik_N);
    parseTimeFull(end, a.jam_L, a.menit_L, a.detik_L);

    pinMode(a.pin, OUTPUT);
    digitalWrite(a.pin, LOW);
    if (a.hari != -1) {
      alarms.push_back(a);
    }
  }
}

void checkAndRunAlarms() {
  timeClient.update();
  int jam = timeClient.getHours();
  int menit = timeClient.getMinutes();
  int detik = timeClient.getSeconds();
  unsigned long epochTime = timeClient.getEpochTime();
  struct tm *ptm = gmtime((time_t *)&epochTime);
  int currentDay = ptm->tm_wday;

  int nowSeconds = jam * 3600 + menit * 60 + detik;

  for (auto& a : alarms) {
    if (a.hari != currentDay) continue;

    int onTime = a.jam_N * 3600 + a.menit_N * 60 + a.detik_N;
    int offTime = a.jam_L * 3600 + a.menit_L * 60 + a.detik_L;

    Serial.printf("Pin %d: now=%d, on=%d, off=%d\n", a.pin, nowSeconds, onTime, offTime);

    if (nowSeconds >= onTime && nowSeconds < offTime) {
      digitalWrite(a.pin, HIGH);
      Serial.println(String(a.pin) + " HIGH");
    } else {
      digitalWrite(a.pin, LOW);
      Serial.println(String(a.pin) + " LOW");
    }
  }
}

/*===============DATA-POST=============*/
bool dataPost() {
  if (WiFi.status() != WL_CONNECTED) return false;

  HTTPClient http;
  http.setTimeout(3000);  // 3 detik timeout
  http.begin(serverUrl_post);
  http.addHeader("Content-Type", "application/json");

  float suhu_udara = dht.readHumidity();
  float kelembapan_udara = dht.readTemperature();

  uint8_t status = 1;
  uint8_t suhu_air = analogRead(PIN_SUHU_AIR);
  uint16_t tds = analogRead(PIN_TDS);
  float debit = analogRead(PIN_DEBIT);
  uint8_t lever_air = analogRead(PIN_LEVEL_AIR);

  String jsonData = "{";
  jsonData += "\"status\":" + String(status) + ",";
  jsonData += "\"suhu_air\":" + String(suhu_air) + ",";
  jsonData += "\"tds\":" + String(tds) + ",";
  jsonData += "\"debit\":" + String(debit, 2) + ",";
  jsonData += "\"suhu_udara\":" + String(suhu_udara) + ",";
  jsonData += "\"kelembapan_udara\":" + String(kelembapan_udara) + ",";
  jsonData += "\"lever_air\":" + String(lever_air);
  jsonData += "}";

  int httpResponseCode = http.POST(jsonData);

  if (httpResponseCode > 0) {
    String response = http.getString();
    Serial.println("(POST)-> " + response);
    http.end();
    return true;
  } else {
    Serial.println("(ERROR_POST)-> " + String(httpResponseCode));
    http.end();
    return false;
  }
}
/*===============DATA-GET=============*/
bool dataGet() {
  if (WiFi.status() != WL_CONNECTED) return false;

  HTTPClient http;
  http.setTimeout(3000);  // 3 detik timeout
  http.begin(serverUrl_get);

  int httpCode = http.GET();

  if (httpCode == 200) {
    String payload = http.getString();

    DynamicJsonDocument doc(2048);
    DeserializationError error = deserializeJson(doc, payload);

    if (error) {
      Serial.println("JSON parsing failed!");
      http.end();
      return false;
    }

    loadAlarmsFromJson(doc);

    http.end();
    return true;
  } else {
    Serial.println("(ERROR_GET)-> " + String(httpCode));
    http.end();
    return false; 
  }
}

void inputGet() {
  Serial.println("URL GET>>");
  while (Serial.available() == 0) {
    delay(100);
  }

  String input_get = Serial.readStringUntil('\n');
  input_get.trim();

  serverUrl_get = input_get;
  delay(500);
  Serial.println("server URL_GET telah disimmpan sementara -> ");
  Serial.print(serverUrl_get + "\n");
}

void inputPost() {
  Serial.println("URL POST>>");
  while (Serial.available() == 0) {
    delay(100);
  }

  String input_post = Serial.readStringUntil('\n');
  input_post.trim();

  serverUrl_post = input_post;
  delay(500);
  Serial.println("server URL_GET telah disimmpan sementara -> ");
  Serial.print(serverUrl_post + "\n");
}


void setup() {
  Serial.begin(115200);

  pinMode(PIN_SUHU_AIR, INPUT);
  pinMode(PIN_TDS, INPUT);
  pinMode(PIN_DEBIT, INPUT);
  pinMode(PIN_LEVEL_AIR, INPUT);

  dht.begin();

  WiFi.begin(ssid, password);
  Serial.print("Menghubungkan ke WiFi...");

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nWiFi terhubung.");
  delay(1000);
  
  timeClient.begin(); // Memulai NTPClient

  Serial.println(WiFi.localIP());  // harusnya 172.25.x.x

  // Ambil konfigurasi awal dari server
  inputGet();
  delay(500);
  inputPost();

  dataGet();
  dataPost();
}

void loop() {
  static uint32_t lastTime_get = 0;
  static uint32_t lastTime_post = 0;
  static uint32_t lastTime_timeUpdate = 0;
  static uint32_t lastTime_handleAlarm = 0;

  uint32_t now = millis();

  if (now - lastTime_get > 30000) {
    lastTime_get = now;
    if (!dataGet()) {
      Serial.println("(ERROR_GET)-> -1");
    }
  }

  if (now - lastTime_post > 5000) {
    lastTime_post = now;
    if (!dataPost()) {
      Serial.println("(ERROR_POST)-> -1");
    }
  }

  if (now - lastTime_handleAlarm > 1000) {
    lastTime_handleAlarm = now;
    checkAndRunAlarms();
  }

  // Hapus delay atau gunakan delay kecil yang tidak blocking
  delay(1);
}
