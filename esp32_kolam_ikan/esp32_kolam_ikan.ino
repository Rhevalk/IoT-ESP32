#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <vector>
#include <NTPClient.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>

const char* botToken = "8164049287:AAE7K3z-sVL-uOd5fnTDazadgrTyCsVy7V4";
const String chat_id = "8164049287";
WiFiClientSecure secured_client;
UniversalTelegramBot bot(botToken, secured_client);

// --- Konfigurasi PIN ---
#define PIN_DEBIT     14
#define PIN_SUHU_AIR  4
uint8_t pinSensor[6] = {32, 33, 34, 35, 25, 26};
bool warningSensor[6] = { false };
uint8_t valueSensor[6] = { 0 };

// --- Konfigurasi WiFi & Server ---
const char* ssid = "Banyak makan";
const char* password = "rhevan1119";
const char* serverUrl_get  = "https://cinfarm.loca.lt/api/data-get?file=kolam-ikan"; 
const char* serverUrl_post = "https://cinfarm.loca.lt/api/data-post?file=kolam-ikan"; 

// --- NTP Time ---
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "time.google.com", 3600 * 7, 60000);  // GMT+7

// --- Struktur Alarm ---
struct Alarm {
  int hari; // -1 = setiap hari
  int pin;
  int jam_N, menit_N, detik_N;
  int jam_L, menit_L, detik_L;
  std::vector<int> pengecualianHari;
  String jenisIkan;
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
  alarms.clear(); // Hindari duplikat alarm
  const char* jenisIkan[] = {"NilaInfo", "LeleInfo"};

  for (int i = 0; i < 2; i++) {
    if (!doc.containsKey(jenisIkan[i])) continue;
    JsonArray array = doc[jenisIkan[i]]["jadwal"].as<JsonArray>();

    for (JsonObject obj : array) {
      Alarm a;
      String hariStr = obj["hari"] | "";
      a.hari = hariStr == "Setiap Hari" ? -1 : convertHariToInt(hariStr);

      if (obj.containsKey("pengecualian")) {
        JsonArray exc = obj["pengecualian"].as<JsonArray>();
        for (JsonVariant h : exc) {
          int hInt = convertHariToInt(h.as<String>());
          if (hInt >= 0) a.pengecualianHari.push_back(hInt);
        }
      }

      a.pin = obj["pin"].as<int>();
      String start = obj["start"] | "00:00:00";
      String end   = obj["end"] | "00:00:00";
      parseTimeFull(start, a.jam_N, a.menit_N, a.detik_N);
      parseTimeFull(end, a.jam_L, a.menit_L, a.detik_L);

      pinMode(a.pin, OUTPUT);
      a.jenisIkan = String(jenisIkan[i]).substring(0, String(jenisIkan[i]).indexOf("Info"));

      alarms.push_back(a);
    }
  }
}

void checkAndRunAlarms() {
  timeClient.update();
  uint8_t jam   = timeClient.getHours();
  uint8_t menit = timeClient.getMinutes();
  uint8_t detik = timeClient.getSeconds();

  time_t rawTime = (time_t)timeClient.getEpochTime();
  struct tm *ptm = localtime(&rawTime);
  uint8_t currentDay = ptm->tm_wday;

  int nowSeconds = jam * 3600 + menit * 60 + detik;

  for (auto& a : alarms) {
    bool aktifHariIni = (a.hari == -1)
      ? std::find(a.pengecualianHari.begin(), a.pengecualianHari.end(), currentDay) == a.pengecualianHari.end()
      : (a.hari == currentDay);

    if (!aktifHariIni) {
      digitalWrite(a.pin, HIGH);
      Serial.println("[" + a.jenisIkan + "] PIN " + String(a.pin) + " HIGH");
      continue;
    }

    int onTime  = a.jam_N * 3600 + a.menit_N * 60 + a.detik_N;
    int offTime = a.jam_L * 3600 + a.menit_L * 60 + a.detik_L;

    if (nowSeconds >= onTime && nowSeconds < offTime) {
      digitalWrite(a.pin, LOW);
      Serial.println("[" + a.jenisIkan + "] PIN " + String(a.pin) + " LOW");
    } else {
      digitalWrite(a.pin, HIGH);
      Serial.println("[" + a.jenisIkan + "] PIN " + String(a.pin) + " HIGH");
    }
  }
}

void readSensor() {
  for (uint8_t i = 0; i < 6; i++) {
    valueSensor[i] = digitalRead(pinSensor[i]);

    if (i < 4) {
      warningSensor[i] = (valueSensor[i] == 1);
      if (warningSensor[i])
        Serial.println("⚠️ Pakan kolam " + String(i) + " habis!");
        bot.sendMessage(chat_id, "Pakan kolam " + String(i) + " habis!", "");
    } else {
      warningSensor[i] = (valueSensor[i] == 0);
      if (warningSensor[i])
        Serial.println("⚠️ Air kolam " + String(i - 4) + " habis!");
        bot.sendMessage(chat_id, "Air kolam " + String(i - 4) + " habis!", "");
    }
  }
}

bool dataPost() {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setTimeout(3000);
  http.begin(client, serverUrl_post);
  http.addHeader("Content-Type", "application/json");

  String jsonData = "{"
    "\"NilaInfo\":{"
      "\"status\":1,"
      "\"suhu_air\":0,"
      "\"debit\":0,"
      "\"warning_ir_1\":" + String(warningSensor[0]) + ","
      "\"warning_ir_2\":" + String(warningSensor[1]) + ","
      "\"warning_level_1\":" + String(warningSensor[4]) +
    "},"
    "\"LeleInfo\":{"
      "\"status\":1,"
      "\"suhu_air\":0,"
      "\"debit\":0,"
      "\"warning_ir_1\":" + String(warningSensor[2]) + ","
      "\"warning_ir_2\":" + String(warningSensor[3]) + ","
      "\"warning_level_1\":" + String(warningSensor[5]) +
    "}"
  "}";

  int code = http.POST(jsonData);
  if (code > 0) {
    Serial.println("(POST)-> " + http.getString());
    http.end();
    return true;
  } else {
    Serial.println("(ERROR_POST)-> " + String(code));
    http.end();
    return false;
  }
}

bool dataGet() {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setTimeout(3000);
  http.begin(client, serverUrl_get);

  int code = http.GET();
  if (code == 200) {
    DynamicJsonDocument doc(2048);
    DeserializationError err = deserializeJson(doc, http.getString());

    if (err) {
      Serial.println("JSON parsing failed!");
      http.end();
      return false;
    }

    loadAlarmsFromJson(doc);
    Serial.println("(GET)-> OK");
    http.end();
    return true;
  } else {
    Serial.println("(ERROR_GET)-> " + String(code));
    http.end();
    return false;
  }
}

// --- Task: Data GET every 30s ---
void TaskDataGet(void * pvParameters) {
  uint32_t lastTime = 0;
  while (true) {
    if ((millis() - lastTime) >= 30000) {
      lastTime = millis();
      if (!dataGet()) Serial.println("(ERROR_GET)-> -1");
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

// --- Task: Data POST every 5s ---
void TaskDataPost(void * pvParameters) {
  uint32_t lastTime = 0;
  while (true) {
    if ((millis() - lastTime) >= 5000) {
      lastTime = millis();
      if (!dataPost()) Serial.println("(ERROR_POST)-> -1");
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

// --- Task: Check alarms every 1s ---
void TaskCheckAlarms(void * pvParameters) {
  while (true) {
    if (WiFi.status() != WL_CONNECTED) {

    }
    checkAndRunAlarms();
    readSensor();
    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}

void TaskWiFiMonitor(void* pvParameters) {
  while (true) {
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("⚠️ WiFi Lost, reconnect...");
      WiFi.disconnect();
      WiFi.begin(ssid, password);
      delay(1000);
    }
    vTaskDelay(10000 / portTICK_PERIOD_MS); // setiap 10 detik
  }
}


void setup() {
  Serial.begin(115200);

  pinMode(PIN_SUHU_AIR, INPUT);
  pinMode(PIN_DEBIT, INPUT);
  for (uint8_t i = 0; i < 6; i++) pinMode(pinSensor[i], INPUT);

  WiFi.begin(ssid, password);
  Serial.print("🔌 Menghubungkan ke WiFi");
  int attempt = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500); Serial.print(".");
    if (++attempt > 20) {
      Serial.println("❌ Gagal konek WiFi");
      return;
    }
  }
  Serial.println("\n✅ WiFi terhubung");

  delay(1000);
  bot.sendMessage(chat_id, "✅ESP-kolam-ikan aktif", "");

  timeClient.begin();
  delay(500);
  dataGet();
  dataPost();

  xTaskCreate(TaskDataGet, "TaskDataGet", 4096, NULL, 1, NULL);
  xTaskCreate(TaskDataPost, "TaskDataPost", 4096, NULL, 2, NULL);
  xTaskCreate(TaskCheckAlarms, "TaskCheckAlarms", 4096, NULL, 3, NULL);
  xTaskCreate(TaskWiFiMonitor, "TaskWiFiMonitor", 512, NULL, 1, NULL);
}

void loop() {
  vTaskDelay(portMAX_DELAY); 
}
