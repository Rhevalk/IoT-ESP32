#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <vector>
#include <NTPClient.h>
#include "DHT.h"
#include <set> 

#define DHTPIN 4  
#define DHTTYPE DHT11  

DHT dht(DHTPIN, DHTTYPE);

#define PIN_SUHU_AIR    13
#define PIN_TDS         27
#define PIN_DEBIT       26
#define PIN_LEVEL_AIR   25

const char* ssid = "Banyak makan";
const char* password = "rhevan1119";
const char* serverUrl_get = "http://orangepi.local:3000/api/data-get?file=hidroponik"; 
const char* serverUrl_post = "http://orangepi.local:3000/api/data-post?file=hidroponik"; 

// waktu
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "time.google.com", 3600 * 7, 60000);  // 7 jam offset, update setiap 60 detik

struct Alarm {
  int8_t hari; // 0-6 hari, -1 invalid, -2 setiap hari
  std::vector<int8_t> pengecualianHari; // hari yg dikecualikan jika setiap hari
  int pin;
  int jam_N, menit_N, detik_N;
  int jam_L, menit_L, detik_L;
};

std::vector<Alarm> alarms;

std::set<int> initializedPins;

SemaphoreHandle_t xAlarmMutex = NULL;

int8_t convertHariToInt(const String& hari) {
  if (hari == "Minggu") return 0;
  if (hari == "Senin") return 1;
  if (hari == "Selasa") return 2;
  if (hari == "Rabu") return 3;
  if (hari == "Kamis") return 4;
  if (hari == "Jumat") return 5;
  if (hari == "Sabtu") return 6;
  if (hari == "Setiap Hari") return -2;
  return -1;
}

void parseTimeFull(const String& timeStr, int& jam, int& menit, int& detik) {
  sscanf(timeStr.c_str(), "%d:%d:%d", &jam, &menit, &detik);
}

void safeLoadAlarms(JsonDocument& doc) {
  if (xSemaphoreTake(xAlarmMutex, portMAX_DELAY)) {
    alarms.clear();

    JsonArray array = doc["plantInfo"]["jadwal"].as<JsonArray>();

    for (JsonObject obj : array) {
      Alarm a;
      String hariStr = obj["hari"] | "";
      a.hari = convertHariToInt(hariStr);
      a.pin = obj["pin"].as<int>();

      String start = obj["start"] | "00:00:00";
      String end = obj["end"] | "00:00:00";
      parseTimeFull(start, a.jam_N, a.menit_N, a.detik_N);
      parseTimeFull(end, a.jam_L, a.menit_L, a.detik_L);

      a.pengecualianHari.clear();
      if (a.hari == -2 && obj.containsKey("pengecualian")) {
        JsonArray pArray = obj["pengecualian"].as<JsonArray>();
        for (const auto& hariEx : pArray) {
          int8_t hariExInt = convertHariToInt(String((const char*)hariEx));
          if (hariExInt != -1) {
            a.pengecualianHari.push_back(hariExInt);
          }
        }
      }

      // Hanya set pinMode jika belum pernah di-set
      if (!initializedPins.count(a.pin)) {
        pinMode(a.pin, OUTPUT);
        initializedPins.insert(a.pin);
      }

      if (a.hari != -1) {
        alarms.push_back(a);
      }
    }

    xSemaphoreGive(xAlarmMutex);
  }
}

bool isHariInPengecualian(int8_t hari, const std::vector<int8_t>& pengecualian) {
  for (auto& p : pengecualian) {
    if (p == hari) return true;
  }
  return false;
}


void checkAndRunAlarms() {
  timeClient.update();
  uint8_t jam = timeClient.getHours();
  uint8_t menit = timeClient.getMinutes();
  uint8_t detik = timeClient.getSeconds();

  unsigned long localEpoch = timeClient.getEpochTime();
  time_t rawTime = (time_t)localEpoch;
  struct tm *ptm = localtime(&rawTime);
  uint8_t currentDay = ptm->tm_wday; // 0 = Minggu ... 6 = Sabtu

  int nowSeconds = jam * 3600 + menit * 60 + detik;

  //Serial.printf("⏰ Sekarang: %02d:%02d:%02d | Hari: %d\n", jam, menit, detik, currentDay);

  for (auto& a : alarms) {
    bool aktifHari = false;
    //String alasan = "";

    if (a.hari == currentDay) {
      aktifHari = true;
      //alasan = "Cocok hari";
    } else if (a.hari == -2) {
      if (!isHariInPengecualian(currentDay, a.pengecualianHari)) {
        aktifHari = true;
        //alasan = "Setiap Hari, bukan pengecualian";
      } else {
        //alasan = "Setiap Hari, tapi dikecualikan";
      }
    } else {
      //alasan = "Bukan hari yang cocok";
    }

    if (!aktifHari) {
      //Serial.printf("❌ Alarm pin %d TIDAK aktif hari ini (%s)\n", a.pin, alasan.c_str());
      digitalWrite(a.pin, HIGH);
      continue;
    }

    int onTime = a.jam_N * 3600 + a.menit_N * 60 + a.detik_N;
    int offTime = a.jam_L * 3600 + a.menit_L * 60 + a.detik_L;

    bool isActive = false;
    if (onTime <= offTime) {
      isActive = nowSeconds >= onTime && nowSeconds < offTime;
    } else {
      isActive = nowSeconds >= onTime || nowSeconds < offTime;
    }

    if (isActive) {
      digitalWrite(a.pin, LOW);
      //Serial.printf("✅ Alarm pin %d AKTIF sekarang (%02d:%02d:%02d - %02d:%02d:%02d)\n", a.pin, a.jam_N, a.menit_N, a.detik_N, a.jam_L, a.menit_L, a.detik_L);
    } else {
      digitalWrite(a.pin, HIGH);
      //Serial.printf("🛑 Alarm pin %d TIDAK AKTIF sekarang (di luar waktu %02d:%02d:%02d - %02d:%02d:%02d)\n", a.pin, a.jam_N, a.menit_N, a.detik_N, a.jam_L, a.menit_L, a.detik_L);
    }
  }
}


bool dataPost() {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClient client;
  HTTPClient http;
  http.setTimeout(3000);
  http.begin(client, serverUrl_post);
  http.addHeader("Content-Type", "application/json");

  float suhu_udara = dht.readHumidity();
  float kelembapan_udara = dht.readTemperature();

  if (isnan(kelembapan_udara)) kelembapan_udara = 0;
  if (isnan(suhu_udara)) suhu_udara = 0;

  uint8_t status = 1;
  uint8_t suhu_air = analogRead(PIN_SUHU_AIR);
  uint16_t tds = analogRead(PIN_TDS);
  float debit = analogRead(PIN_DEBIT);
  uint8_t level_air = analogRead(PIN_LEVEL_AIR);

  String jsonData = "{";
  jsonData += "\"HidroponikInfo\":{";
  jsonData += "\"status\":" + String(status) + ",";
  jsonData += "\"suhu_air\":" + String(suhu_air) + ",";
  jsonData += "\"tds\":" + String(tds) + ",";
  jsonData += "\"debit\":" + String(debit, 2) + ",";
  jsonData += "\"suhu_udara\":" + String(suhu_udara) + ",";
  jsonData += "\"kelembapan_udara\":" + String(kelembapan_udara) + ",";
  jsonData += "\"level_air\":" + String(level_air);
  jsonData += "}}";

  int httpResponseCode = http.POST(jsonData);

  if (httpResponseCode > 0) {
    String response = http.getString();
    //Serial.println("(POST)-> " + response);
    http.end();
    return true;
  } else {
    //Serial.println("(ERROR_POST)-> " + String(httpResponseCode));
    http.end();
    return false;
  }
}

bool dataGet() {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClient client;
  HTTPClient http;
  http.setTimeout(3000);
  http.begin(client, serverUrl_get);

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

    safeLoadAlarms(doc);

    Serial.println("(GET)-> " + String(httpCode));
    http.end();
    return true;
  } else {
    Serial.println("(ERROR_GET)-> " + String(httpCode));
    http.end();
    return false; 
  }
}

// --- TASK 1: Data GET every 5s ---
void TaskDataGet(void * pvParameters) {
  static uint32_t lastTime = 0;
  while (true) {
    if (millis() - lastTime >= 5000) {
      lastTime = millis();
      if (!dataGet()) {
        Serial.println("(ERROR_GET)-> -1");
      }
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

// --- TASK 2: Data POST every 5s ---
void TaskDataPost(void * pvParameters) {
  static uint32_t lastTime = 0;
  while (true) {
    if (millis() - lastTime >= 5000) {
      lastTime = millis();
      if (!dataPost()) {
        Serial.println("(ERROR_POST)-> -1");
      }
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

// --- TASK 3: Check and run alarms every 1s ---
void TaskCheckAlarms(void * pvParameters) {
  while (true) {
    if (xSemaphoreTake(xAlarmMutex, portMAX_DELAY)) {
      checkAndRunAlarms();
      xSemaphoreGive(xAlarmMutex);
    }
    vTaskDelay(500 / portTICK_PERIOD_MS); 
  }
}


void TaskWiFiMonitor(void* pvParameters) {
  while (true) {
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("⚠️ WiFi Lost, reconnect...");
      WiFi.disconnect(true);
      WiFi.begin(ssid, password);

      int retry = 0;
      while (WiFi.status() != WL_CONNECTED && retry < 20) {
        vTaskDelay(500 / portTICK_PERIOD_MS);
        retry++;
        Serial.print(".");
      }

      if (WiFi.status() == WL_CONNECTED) {
        Serial.println("✅ Reconnected!");
      } else {
        Serial.println("❌ Failed to reconnect.");
      }
    }

    vTaskDelay(10000 / portTICK_PERIOD_MS); // cek tiap 10 detik
  }
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

  xAlarmMutex = xSemaphoreCreateMutex();
  if (xAlarmMutex == NULL) {
    Serial.println("❌ Gagal membuat mutex!");
    while (true); // berhenti agar tidak crash
  }
  
  timeClient.begin();

  // Ambil data awal
  dataGet();
  dataPost();


  // Buat task FreeRTOS dengan prioritas sedang
  xTaskCreate(TaskDataGet, "TaskDataGet", 4096, NULL, 1, NULL);
  xTaskCreate(TaskDataPost, "TaskDataPost", 4096, NULL, 1, NULL);
  xTaskCreate(TaskCheckAlarms, "TaskCheckAlarms", 4096, NULL, 2, NULL);
  xTaskCreate(TaskWiFiMonitor, "TaskWiFiMonitor", 2048, NULL, 1, NULL);

}

void loop() {
  vTaskDelay(portMAX_DELAY);
}
