#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <vector>
#include <NTPClient.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <set> 

const char* botToken = "8164049287:AAE7K3z-sVL-uOd5fnTDazadgrTyCsVy7V4";
const String chat_id = "7802571705";
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
const char* serverUrl_get  = "http://orangepi.local:3000/api/data-get?file=kolam-ikan"; 
const char* serverUrl_post = "http://orangepi.local:3000/api/data-post?file=kolam-ikan"; 

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
  String deskripsi;
  bool wasActive = false;
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
  return -1;
}

void parseTimeFull(const String& timeStr, int& jam, int& menit, int& detik) {
  sscanf(timeStr.c_str(), "%d:%d:%d", &jam, &menit, &detik);
}


/*================================LOAD FROM JSON==============================*/
void safeLoadAlarmsKolamIkan(JsonDocument& doc) {
  if (xSemaphoreTake(xAlarmMutex, portMAX_DELAY)) {
    alarms.clear();

    const char* jenisIkan[] = {"NilaInfo", "LeleInfo"};

    for (int i = 0; i < 2; i++) {
      if (!doc.containsKey(jenisIkan[i])) continue;
      JsonArray array = doc[jenisIkan[i]]["jadwal"].as<JsonArray>();

      for (JsonObject obj : array) {
        Alarm a;
        String hariStr = obj["hari"] | "";
        a.hari = (hariStr == "Setiap Hari") ? -1 : convertHariToInt(hariStr);
        a.pin = obj["pin"].as<int>();
        a.deskripsi = obj["deskripsi"].as<const char*>();

        // Pengecualian hanya digunakan jika Setiap Hari
        a.pengecualianHari.clear();
        if (a.hari == -1 && obj.containsKey("pengecualian")) {
          JsonArray pArray = obj["pengecualian"].as<JsonArray>();
          for (const auto& hariEx : pArray) {
            int8_t hariExInt = convertHariToInt(hariEx.as<String>());
            if (hariExInt >= 0) {
              a.pengecualianHari.push_back(hariExInt);
            }
          }
        }

        // Parsing waktu
        String start = obj["start"] | "00:00:00";
        String end   = obj["end"] | "00:00:00";
        parseTimeFull(start, a.jam_N, a.menit_N, a.detik_N);
        parseTimeFull(end, a.jam_L, a.menit_L, a.detik_L);

        // Set pinMode hanya jika belum
        if (!initializedPins.count(a.pin)) {
          pinMode(a.pin, OUTPUT);
          digitalWrite(a.pin, HIGH);  // Pastikan relay OFF awalnya
          initializedPins.insert(a.pin);
        }

        // Set nama jenis ikan
        a.jenisIkan = String(jenisIkan[i]).substring(0, String(jenisIkan[i]).indexOf("Info"));

        alarms.push_back(a);
      }
    }

    xSemaphoreGive(xAlarmMutex);
  }
}

/*================================CHECK ALARM================================*/
void checkAndRunAlarms() {
  timeClient.update();
  uint8_t jam   = timeClient.getHours();
  uint8_t menit = timeClient.getMinutes();
  uint8_t detik = timeClient.getSeconds();

  time_t rawTime = (time_t)timeClient.getEpochTime();
  struct tm *ptm = localtime(&rawTime);
  uint8_t currentDay = ptm->tm_wday; // 0 = Minggu, ..., 6 = Sabtu

  int nowSeconds = jam * 3600 + menit * 60 + detik;

  //Serial.printf("⏰ Sekarang: %02d:%02d:%02d | Hari: %d\n", jam, menit, detik, currentDay);

  for (auto& a : alarms) {
    bool aktifHariIni = false;
    //String alasan = "";

    if (a.hari == -1) {
      if (std::find(a.pengecualianHari.begin(), a.pengecualianHari.end(), currentDay) == a.pengecualianHari.end()) {
        aktifHariIni = true;
      } else {
        //alasan = "Masuk pengecualian";
      }
    } else {
      aktifHariIni = (a.hari == currentDay);
      //if (!aktifHariIni) alasan = "Bukan hari yang dijadwalkan";
    }

    if (!aktifHariIni) {
      //Serial.printf("❌ Alarm pin %d TIDAK aktif hari ini (%s)\n", a.pin, alasan.c_str());
      digitalWrite(a.pin, HIGH);
      continue;
    }

    int onTime  = a.jam_N * 3600 + a.menit_N * 60 + a.detik_N;
    int offTime = a.jam_L * 3600 + a.menit_L * 60 + a.detik_L;

    bool isActive = false;
    if (onTime <= offTime) {
      isActive = nowSeconds >= onTime && nowSeconds < offTime;
    } else {
      isActive = nowSeconds >= onTime || nowSeconds < offTime;
    }

    if (isActive) {
      digitalWrite(a.pin, LOW);

      if (!a.wasActive) {
        bot.sendMessage(chat_id, "✅ Alarm " + a.deskripsi + " Aktif", "Markdown");
        a.wasActive = true;
      }

      //Serial.printf("✅ Alarm pin %d AKTIF sekarang (%02d:%02d:%02d - %02d:%02d:%02d)\n", a.pin, a.jam_N, a.menit_N, a.detik_N, a.jam_L, a.menit_L, a.detik_L);
    } else {
      digitalWrite(a.pin, HIGH);

      if (a.wasActive) {
        bot.sendMessage(chat_id, "🛑 Alarm " + a.deskripsi + " Selesai", "Markdown");
        a.wasActive = false;
      }

      //Serial.printf("🛑 Alarm pin %d TIDAK AKTIF sekarang (di luar waktu %02d:%02d:%02d - %02d:%02d:%02d)\n",a.pin, a.jam_N, a.menit_N, a.detik_N, a.jam_L, a.menit_L, a.detik_L);
    }
  }
}

/*================================READ SENSOR=================================*/
void readSensor() {
  for (uint8_t i = 0; i < 6; i++) {
    valueSensor[i] = digitalRead(pinSensor[i]);

    if (i < 4) {
      warningSensor[i] = (valueSensor[i] == 0);
      if (warningSensor[i])
        bot.sendMessage(chat_id, "🛑 Pakan kolam " + String(i) + " habis!", "Markdown");
    } else {
      warningSensor[i] = (valueSensor[i] == 0);
      if (warningSensor[i])
        bot.sendMessage(chat_id, "🛑 Air kolam " + String(i - 4) + " habis!", "Markdown");
    }
  }
}
/*================================DATA POST===================================*/
bool dataPost() {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClient client;
  

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
    //Serial.println("(POST)-> " + http.getString());
    http.end();
    return true;
  } else {
    //Serial.println("(ERROR_POST)-> " + String(code));
    http.end();
    return false;
  }
}

/*================================DATA GET===================================*/
bool dataGet() {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClient client;
  

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

    safeLoadAlarmsKolamIkan(doc);

    //Serial.println("(GET)-> OK");
    http.end();
    return true;
  } else {
    //Serial.println("(ERROR_GET)-> " + String(code));
    http.end();
    return false;
  }
}

// --- Task: Data GET every 5s ---
void TaskDataGet(void * pvParameters) {
  uint32_t lastTime = 0;
  while (true) {
    if ((millis() - lastTime) >= 5000) {
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
    if (xSemaphoreTake(xAlarmMutex, portMAX_DELAY)) {
      checkAndRunAlarms();
      readSensor();
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

  xAlarmMutex = xSemaphoreCreateMutex();
  if (xAlarmMutex == NULL) {
    Serial.println("❌ Gagal membuat mutex!");
    while (true); // berhenti agar tidak crash
  }

  delay(1000);
  secured_client.setCACert(TELEGRAM_CERTIFICATE_ROOT);

  bot.sendMessage(chat_id, "✅ESP-kolam-ikan aktif", "Markdown");

  timeClient.begin();
  delay(500);
  dataGet();
  dataPost();

  xTaskCreate(TaskDataGet, "TaskDataGet", 4096, NULL, 1, NULL);
  xTaskCreate(TaskDataPost, "TaskDataPost", 4096, NULL, 1, NULL);
  xTaskCreate(TaskCheckAlarms, "TaskCheckAlarms", 6144, NULL, 2, NULL);
  xTaskCreate(TaskWiFiMonitor, "TaskWiFiMonitor", 2048, NULL, 1, NULL);
}

void loop() {
  vTaskDelay(portMAX_DELAY); 
}
