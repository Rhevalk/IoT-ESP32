#include <WiFi.h>
#include <WiFiClient.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "esp_camera.h"
#include <NTPClient.h>
#include <WiFiUdp.h>

const char* ssid = "Banyak makan";
const char* password = "rhevan1119";

const char* CONFIG_URL = "https://cinfarm.loca.lt/api/config?camId=cam1";
const char* UPLOAD_URL = "https://cinfarmend.loca.lt/upload/1";

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "time.google.com", 3600 * 7, 60000);

int fps, max_images, interval_seconds;
uint8_t start_hour, end_hour;
bool flash, power;

unsigned long lastCaptureTime = 0;
uint8_t capturedCount = 0;

#define FLASH_PIN 4

void initCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0       = 5;
  config.pin_d1       = 18;
  config.pin_d2       = 19;
  config.pin_d3       = 21;
  config.pin_d4       = 36;
  config.pin_d5       = 39;
  config.pin_d6       = 34;
  config.pin_d7       = 35;
  config.pin_xclk     = 0;
  config.pin_pclk     = 22;
  config.pin_vsync    = 25;
  config.pin_href     = 23;
  config.pin_sscb_sda = 26;
  config.pin_sscb_scl = 27;
  config.pin_pwdn     = 32;
  config.pin_reset    = -1;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;

  if(psramFound()){
    config.frame_size = FRAMESIZE_VGA;
    config.jpeg_quality = 10;
    config.fb_count = 2;
  } else {
    config.frame_size = FRAMESIZE_CIF;
    config.jpeg_quality = 12;
    config.fb_count = 1;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK){
    Serial.printf("Camera init failed with error 0x%x", err);
    return;
  }
}

bool fetchConfig() {
  HTTPClient http;
  http.begin(CONFIG_URL);
  http.addHeader("Content-Type", "application/json");

  bool status = true;
  // Kirim JSON body (status & camId)
  StaticJsonDocument<128> requestBody;
  requestBody["camId"] = "cam1";
  requestBody["status"] = status; 

  String jsonBody;
  serializeJson(requestBody, jsonBody);

  int httpCode = http.POST(jsonBody);

  if (httpCode == 200) {
    String payload = http.getString();
    StaticJsonDocument<512> doc;
    DeserializationError err = deserializeJson(doc, payload);

    if (err) {
      Serial.println("❌ JSON parsing error");
      http.end();
      return false;
    }

    // Ambil data dari response

    JsonObject config = doc["config"];
    JsonObject cam = doc["cam"];

    fps = config["fps"];
    max_images = config["max_images"];
    interval_seconds = config["interval_seconds"];
    start_hour = config["start_hour"];
    end_hour = config["end_hour"];
    flash = config["flash"];

    power = cam["power"];
    
    Serial.println(fps);
    Serial.println(max_images);
    Serial.println(interval_seconds);
    Serial.println(start_hour);
    Serial.println(end_hour);
    Serial.println(flash);
    Serial.println(power);

    Serial.println("✅ Config loaded via POST");
    http.end();
    return true;
  } else {
    Serial.print("❌ HTTP Error: ");
    Serial.println(httpCode);
    http.end();
    return false;
  }
}

void sendPhoto(camera_fb_t *fb) {
  if (!fb) return;

  HTTPClient http;
  http.begin(UPLOAD_URL);
  http.addHeader("Content-Type", "image/jpeg");

  int httpResponseCode = http.POST(fb->buf, fb->len);

  if (httpResponseCode > 0) {
    Serial.printf("📸 Image sent! Status: %d\n", httpResponseCode);
  } else {
    Serial.printf("❌ Error sending image: %s\n", http.errorToString(httpResponseCode).c_str());
  }

  http.end();
}

void TaskFetchConfig(void * pvParameters) {
  uint32_t lastTime = 0;
  while (true) {
    if ((millis() - lastTime) >= 5000) {
      lastTime = millis();
      if (!fetchConfig()) Serial.println("(ERROR_GET)-> -1");
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void TaskSendPhoto(void *pvParameters) {
  while (true) {
    timeClient.update();
    uint8_t currentHour = timeClient.getHours();
    unsigned long now = millis();

    static bool isWithinTimeRange = false;

    if (start_hour < end_hour) {
      isWithinTimeRange = (currentHour >= start_hour && currentHour < end_hour);
    } else {
      isWithinTimeRange = (currentHour >= start_hour || currentHour < end_hour);
    }

    if (power
      && isWithinTimeRange 
      && (now - lastCaptureTime > interval_seconds * 1000)) {

      digitalWrite(FLASH_PIN, flash ? HIGH : LOW);
      vTaskDelay(pdMS_TO_TICKS(100));

      camera_fb_t * fb = esp_camera_fb_get();
      if (fb) {
        sendPhoto(fb);
        esp_camera_fb_return(fb);

        digitalWrite(FLASH_PIN, LOW);
        lastCaptureTime = now;
        capturedCount++;
      }

      if (capturedCount >= 30) ESP.restart();
    }

    vTaskDelay(pdMS_TO_TICKS(100)); // delay ringan
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
  pinMode(FLASH_PIN, OUTPUT);

  WiFi.begin(ssid, password);
  Serial.print("WiFi Connecting");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n✅ WiFi Connected");

  timeClient.begin();
  timeClient.update();

  initCamera();
  fetchConfig();

  camera_fb_t *fb = esp_camera_fb_get();
  esp_camera_fb_return(fb); 
  delay(500);
  fb = esp_camera_fb_get();
  esp_camera_fb_return(fb); 
  delay(5000);

  Serial.println("✅ Kamera siap");


  digitalWrite(FLASH_PIN, flash ? HIGH : LOW);
  delay(500);
  digitalWrite(FLASH_PIN, LOW);

  xTaskCreate(TaskFetchConfig, "TaskFetchConfig", 6666, NULL, 1, NULL);
  xTaskCreate(TaskSendPhoto, "TaskSendPhoto", 4096, NULL, 1, NULL);
  xTaskCreate(TaskWiFiMonitor, "TaskWiFiMonitor", 2048, NULL, 1, NULL);
}

void loop() { vTaskDelay(portMAX_DELAY); }
