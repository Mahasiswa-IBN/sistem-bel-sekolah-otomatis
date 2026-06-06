/*
 * Sistem Bel Sekolah Otomatis - SMPN Purworejo
 * Arduino Sketch untuk ESP32 dan ESP8266 (NodeMCU/Wemos D1 Mini)
 * 
 * Fitur:
 * 1. Web Server menyajikan file index.html dari LittleFS
 * 2. API RESTful untuk manajemen jadwal bel (menyimpan & menghapus di LittleFS)
 * 3. Sinkronisasi Waktu dengan DS3231 RTC & sinkronisasi via web browser
 * 4. Memicu DFPlayer Mini untuk memutar file suara MP3 otomatis/manual
 * 5. Wi-Fi Manager: Berjalan dalam mode Station (konek ke Wi-Fi sekolah) atau Access Point (AP)
 */

#include <Wire.h>
#include <SPI.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <RTClib.h>
#include <SoftwareSerial.h>
#include <DFRobotDFPlayerMini.h>

#if defined(ESP32)
  #include <WiFi.h>
  #include <WebServer.h>
  WebServer server(80);
  // Definisikan Pin Rx Tx SoftwareSerial untuk DFPlayer di ESP32
  #define PIN_RX 16 // RX2
  #define PIN_TX 17 // TX2
  HardwareSerial mySerial(2); // Menggunakan UART2 pada ESP32
  #define PIN_RELAY 12 // GPIO12
#elif defined(ESP8266)
  #include <ESP8266WiFi.h>
  #include <ESP8266WebServer.h>
  ESP8266WebServer server(80);
  // Definisikan Pin Rx Tx SoftwareSerial untuk DFPlayer di ESP8266
  // D5 (GPIO14) & D6 (GPIO12) aman dan tidak bentrok dengan I2C (D1/D2)
  #define PIN_RX D5 // RX (D5 / GPIO14) -> terhubung ke TX DFPlayer
  #define PIN_TX D6 // TX (D6 / GPIO12) -> terhubung ke RX DFPlayer (via resistor 1K)
  SoftwareSerial mySerial(PIN_RX, PIN_TX); 
  #define PIN_RELAY D7 // GPIO13 (D7) untuk pemicu Relay
#endif

// Objek Sensor/Perangkat
RTC_DS3231 rtc;
DFRobotDFPlayerMini myDFPlayer;

// Struktur Konfigurasi WiFi
struct Config {
  char wifi_ssid[32] = "Bel_Sekolah_AP";
  char wifi_password[64] = "12345678";
  bool is_ap = true;
};
Config systemConfig;

// State Sistem
unsigned long uptimeStart;
int bellsTodayCount = 0;
int successBellsCount = 0;
String lastActiveBell = "Belum berbunyi hari ini";
int lastPlayedMinute = -1;
int lastPlayedDay = -1;
bool isAmplifierOn = false;
unsigned long ampOffTime = 0;

// Rute File
const char* configFilePath = "/config.json";
const char* schedulesFilePath = "/schedules.json";

// Inisialisasi LittleFS
void initFS() {
  if (!LittleFS.begin(#if defined(ESP8266)
                      
                      #elif defined(ESP32)
                      true // Format jika gagal di ESP32
                      #endif
                      )) {
    Serial.println("Gagal me-mount LittleFS!");
    return;
  }
  Serial.println("LittleFS berhasil di-mount.");
}

// Membaca Konfigurasi WiFi dari LittleFS
void loadConfiguration() {
  if (!LittleFS.exists(configFilePath)) {
    Serial.println("File konfigurasi tidak ditemukan, menggunakan nilai default.");
    saveConfiguration();
    return;
  }

  File configFile = LittleFS.open(configFilePath, "r");
  if (!configFile) {
    Serial.println("Gagal membaca file konfigurasi.");
    return;
  }

  StaticJsonDocument<256> doc;
  DeserializationError error = deserializeJson(doc, configFile);
  configFile.close();

  if (error) {
    Serial.println("Gagal memparsing konfigurasi JSON.");
    return;
  }

  strlcpy(systemConfig.wifi_ssid, doc["wifi_ssid"] | "Bel_Sekolah_AP", sizeof(systemConfig.wifi_ssid));
  strlcpy(systemConfig.wifi_password, doc["wifi_password"] | "12345678", sizeof(systemConfig.wifi_password));
  systemConfig.is_ap = doc["is_ap"] | true;
  
  Serial.println("Konfigurasi Wi-Fi berhasil dimuat.");
}

// Menyimpan Konfigurasi WiFi ke LittleFS
void saveConfiguration() {
  File configFile = LittleFS.open(configFilePath, "w");
  if (!configFile) {
    Serial.println("Gagal membuat file konfigurasi.");
    return;
  }

  StaticJsonDocument<256> doc;
  doc["wifi_ssid"] = systemConfig.wifi_ssid;
  doc["wifi_password"] = systemConfig.wifi_password;
  doc["is_ap"] = systemConfig.is_ap;

  if (serializeJson(doc, configFile) == 0) {
    Serial.println("Gagal menulis konfigurasi ke file.");
  }
  configFile.close();
}

// Inisialisasi WiFi (Koneksi ke Router atau buat Access Point sendiri)
void setupWiFi() {
  loadConfiguration();
  
  if (systemConfig.is_ap) {
    Serial.println("Memulai Wi-Fi Mode Access Point (AP)...");
    WiFi.mode(WIFI_AP);
    WiFi.softAP(systemConfig.wifi_ssid, systemConfig.wifi_password);
    IPAddress myIP = WiFi.softAPIP();
    Serial.print("Access Point SSID: ");
    Serial.println(systemConfig.wifi_ssid);
    Serial.print("IP Address Alat: ");
    Serial.println(myIP);
  } else {
    Serial.println("Memulai Wi-Fi Mode Station (STA)...");
    WiFi.mode(WIFI_STA);
    WiFi.begin(systemConfig.wifi_ssid, systemConfig.wifi_password);
    
    int counter = 0;
    while (WiFi.status() != WL_CONNECTED && counter < 20) { // Tunggu maks 10 detik
      delay(500);
      Serial.print(".");
      counter++;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\nWi-Fi Terhubung!");
      Serial.print("IP Address Alat: ");
      Serial.println(WiFi.localIP());
    } else {
      Serial.println("\nGagal terhubung ke Wi-Fi sekolah. Beralih ke Mode AP Darurat...");
      WiFi.mode(WIFI_AP);
      WiFi.softAP("Bel_Sekolah_DARURAT", "12345678");
      Serial.print("AP Darurat IP: ");
      Serial.println(WiFi.softAPIP());
    }
  }
}

// Get Uptime string helper
String getUptime() {
  unsigned long ms = millis() - uptimeStart;
  unsigned long seconds = ms / 1000;
  unsigned long minutes = seconds / 60;
  unsigned long hours = minutes / 60;
  unsigned long days = hours / 24;
  
  char buffer[32];
  snprintf(buffer, sizeof(buffer), "%dd %02dh %02dm", (int)days, (int)(hours % 24), (int)(minutes % 60));
  return String(buffer);
}

// Response JSON helper
void sendJSONResponse(int code, DynamicJsonDocument& doc) {
  String response;
  serializeJson(doc, response);
  server.send(code, "application/json", response);
}

// API Handler: Get Status
void handleGetStatus() {
  if (!rtc.begin()) {
    server.send(500, "text/plain", "Gagal mendeteksi RTC DS3231!");
    return;
  }
  
  DateTime now = rtc.now();
  
  char timeStr[16];
  char dateStr[16];
  snprintf(timeStr, sizeof(timeStr), "%02d:%02d:%02d", now.hour(), now.minute(), now.second());
  snprintf(dateStr, sizeof(dateStr), "%04d-%02d-%02d", now.year(), now.month(), now.day());
  
  DynamicJsonDocument doc(512);
  doc["time"] = timeStr;
  doc["date"] = dateStr;
  doc["day"] = now.dayOfTheWeek() == 0 ? 7 : now.dayOfTheWeek(); // Map 0 (Sunday) to 7
  doc["system_status"] = "OK";
  doc["wifi_ssid"] = systemConfig.wifi_ssid;
  
  #if defined(ESP32)
    int rssi = WiFi.RSSI();
  #elif defined(ESP8266)
    int rssi = WiFi.RSSI();
  #endif
  
  if (systemConfig.is_ap) {
    doc["wifi_signal"] = "Mode AP (Aktif)";
  } else {
    doc["wifi_signal"] = String(rssi) + " dBm";
  }
  
  doc["bells_today"] = bellsTodayCount;
  doc["success_rate"] = String(successBellsCount) + "/" + String(bellsTodayCount);
  doc["uptime"] = getUptime();
  doc["last_active"] = lastActiveBell;
  
  sendJSONResponse(200, doc);
}

// API Handler: Get Schedules
void handleGetSchedules() {
  if (!LittleFS.exists(schedulesFilePath)) {
    server.send(200, "application/json", "[]");
    return;
  }
  
  File file = LittleFS.open(schedulesFilePath, "r");
  if (!file) {
    server.send(500, "text/plain", "Gagal membaca berkas jadwal.");
    return;
  }
  
  // Kirim data langsung dari file untuk menghemat memori
  server.streamFile(file, "application/json");
  file.close();
}

// API Handler: Save Schedule
void handleSaveSchedule() {
  if (!server.hasArg("plain")) {
    server.send(400, "text/plain", "Data POST kosong");
    return;
  }
  
  String requestBody = server.arg("plain");
  StaticJsonDocument<256> newSchedule;
  DeserializationError err = deserializeJson(newSchedule, requestBody);
  
  if (err) {
    server.send(400, "text/plain", "Format JSON salah");
    return;
  }
  
  // Baca jadwal lama
  DynamicJsonDocument doc(8192);
  if (LittleFS.exists(schedulesFilePath)) {
    File file = LittleFS.open(schedulesFilePath, "r");
    deserializeJson(doc, file);
    file.close();
  }
  
  JsonArray schedules = doc.as<JsonArray>();
  int id = newSchedule["id"] | 0;
  
  if (id == 0) {
    // Generate ID baru
    int maxId = 0;
    for (JsonObject s : schedules) {
      int sId = s["id"] | 0;
      if (sId > maxId) maxId = sId;
    }
    newSchedule["id"] = maxId + 1;
    schedules.add(newSchedule);
  } else {
    // Cari & update jadwal yang sudah ada
    bool found = false;
    for (int i = 0; i < schedules.size(); i++) {
      if (schedules[i]["id"] == id) {
        schedules[i] = newSchedule;
        found = true;
        break;
      }
    }
    if (!found) {
      schedules.add(newSchedule);
    }
  }
  
  // Tulis kembali ke file
  File file = LittleFS.open(schedulesFilePath, "w");
  if (!file) {
    server.send(500, "text/plain", "Gagal menulis file jadwal.");
    return;
  }
  
  serializeJson(doc, file);
  file.close();
  
  DynamicJsonDocument res(128);
  res["status"] = "success";
  res["message"] = "Jadwal berhasil disimpan";
  sendJSONResponse(200, res);
}

// API Handler: Delete Schedule
void handleDeleteSchedule() {
  if (!server.hasArg("plain")) {
    server.send(400, "text/plain", "Data POST kosong");
    return;
  }
  
  String requestBody = server.arg("plain");
  StaticJsonDocument<128> deleteReq;
  DeserializationError err = deserializeJson(deleteReq, requestBody);
  
  if (err) {
    server.send(400, "text/plain", "Format JSON salah");
    return;
  }
  
  int id = deleteReq["id"] | 0;
  if (id == 0) {
    server.send(400, "text/plain", "ID tidak valid");
    return;
  }
  
  if (!LittleFS.exists(schedulesFilePath)) {
    server.send(404, "text/plain", "Berkas jadwal tidak ditemukan");
    return;
  }
  
  // Baca berkas lama
  DynamicJsonDocument doc(8192);
  File file = LittleFS.open(schedulesFilePath, "r");
  deserializeJson(doc, file);
  file.close();
  
  JsonArray schedules = doc.as<JsonArray>();
  
  // Hapus elemen
  for (int i = 0; i < schedules.size(); i++) {
    if (schedules[i]["id"] == id) {
      schedules.remove(i);
      break;
    }
  }
  
  // Tulis kembali
  File fileWrite = LittleFS.open(schedulesFilePath, "w");
  if (!fileWrite) {
    server.send(500, "text/plain", "Gagal mengupdate berkas.");
    return;
  }
  
  serializeJson(doc, fileWrite);
  fileWrite.close();
  
  DynamicJsonDocument res(128);
  res["status"] = "success";
  res["message"] = "Jadwal berhasil dihapus";
  sendJSONResponse(200, res);
}

// API Handler: Manual Play Bell
void handlePlayBell() {
  if (!server.hasArg("plain")) {
    server.send(400, "text/plain", "Data POST kosong");
    return;
  }
  
  String requestBody = server.arg("plain");
  StaticJsonDocument<128> playReq;
  DeserializationError err = deserializeJson(playReq, requestBody);
  
  if (err) {
    server.send(400, "text/plain", "Format JSON salah");
    return;
  }
  
  int folder = playReq["folder"] | 1;
  int file = playReq["file"] | 1;
  
  // Hidupkan relay amplifier sebelum bunyi bel
  digitalWrite(PIN_RELAY, HIGH);
  isAmplifierOn = true;
  ampOffTime = millis() + 45000; // Matikan amplifier 45 detik lagi (durasi rata-rata lagu bel)
  
  delay(1000); // Tunggu amplifier stabil
  myDFPlayer.playFolder(folder, file);
  
  DynamicJsonDocument res(128);
  res["status"] = "success";
  res["message"] = "Memutar Folder " + String(folder) + " Track " + String(file);
  sendJSONResponse(200, res);
}

// API Handler: Set RTC Time
void handleSetTime() {
  if (!server.hasArg("plain")) {
    server.send(400, "text/plain", "Data POST kosong");
    return;
  }
  
  String requestBody = server.arg("plain");
  StaticJsonDocument<128> timeReq;
  DeserializationError err = deserializeJson(timeReq, requestBody);
  
  if (err) {
    server.send(400, "text/plain", "Format JSON salah");
    return;
  }
  
  // Format DateTime: "YYYY-MM-DDTHH:MM:SS" (e.g. 2026-06-06T10:35:00)
  String datetime = timeReq["datetime"];
  if (datetime.length() >= 19) {
    int year = datetime.substring(0, 4).toInt();
    int month = datetime.substring(5, 7).toInt();
    int day = datetime.substring(8, 10).toInt();
    int hour = datetime.substring(11, 13).toInt();
    int minute = datetime.substring(14, 16).toInt();
    int second = datetime.substring(17, 19).toInt();
    
    rtc.adjust(DateTime(year, month, day, hour, minute, second));
    
    DynamicJsonDocument res(128);
    res["status"] = "success";
    res["message"] = "Waktu RTC berhasil disesuaikan.";
    sendJSONResponse(200, res);
  } else {
    server.send(400, "text/plain", "Format datetime tidak valid");
  }
}

// API Handler: Save WiFi Settings
void handleSetWiFi() {
  if (!server.hasArg("plain")) {
    server.send(400, "text/plain", "Data POST kosong");
    return;
  }
  
  String requestBody = server.arg("plain");
  StaticJsonDocument<128> wifiReq;
  DeserializationError err = deserializeJson(wifiReq, requestBody);
  
  if (err) {
    server.send(400, "text/plain", "Format JSON salah");
    return;
  }
  
  String ssid = wifiReq["ssid"];
  String password = wifiReq["password"] | "";
  
  strlcpy(systemConfig.wifi_ssid, ssid.c_str(), sizeof(systemConfig.wifi_ssid));
  strlcpy(systemConfig.wifi_password, password.c_str(), sizeof(systemConfig.wifi_password));
  systemConfig.is_ap = false; // Karena diisi oleh user, kita anggap ingin konek ke Router (STA)
  
  saveConfiguration();
  
  DynamicJsonDocument res(128);
  res["status"] = "success";
  res["message"] = "Pengaturan disimpan. Alat akan merestart dalam 3 detik.";
  sendJSONResponse(200, res);
  
  delay(3000);
  #if defined(ESP32)
    ESP.restart();
  #elif defined(ESP8266)
    ESP.restart();
  #endif
}

// Serve static index.html from LittleFS
void handleServeHTML() {
  if (LittleFS.exists("/index.html")) {
    File file = LittleFS.open("/index.html", "r");
    // server.streamFile akan mengirim file dengan kompresi gzip jika didukung browser
    // dan file .gz ada di filesystem. Sementara kita kirim file biasa
    server.streamFile(file, "text/html");
    file.close();
  } else {
    server.send(404, "text/plain", "index.html tidak ditemukan di LittleFS! Silakan upload file index.html menggunakan tool ESP Uploader ke LittleFS.");
  }
}

// Setup Rute Web Server
void setupWebServer() {
  server.on("/", HTTP_GET, handleServeHTML);
  server.on("/index.html", HTTP_GET, handleServeHTML);
  
  // REST APIs
  server.on("/api/status", HTTP_GET, handleGetStatus);
  server.on("/api/schedules", HTTP_GET, handleGetSchedules);
  server.on("/api/schedules/save", HTTP_POST, handleSaveSchedule);
  server.on("/api/schedules/delete", HTTP_POST, handleDeleteSchedule);
  server.on("/api/play", HTTP_POST, handlePlayBell);
  server.on("/api/settings/time", HTTP_POST, handleSetTime);
  server.on("/api/settings/wifi", HTTP_POST, handleSetWiFi);
  
  // Mulai Server
  server.begin();
  Serial.println("HTTP Web Server berjalan.");
}

// Memeriksa Alarm Setiap Menit
void checkAlarm(DateTime now) {
  // Hanya jalankan pencocokan jika detiknya 0
  if (now.second() != 0) return;
  
  // Hindari memicu alarm ganda di menit yang sama
  if (now.minute() == lastPlayedMinute && now.day() == lastPlayedDay) return;
  
  if (!LittleFS.exists(schedulesFilePath)) return;
  
  File file = LittleFS.open(schedulesFilePath, "r");
  if (!file) return;
  
  DynamicJsonDocument doc(8192);
  DeserializationError err = deserializeJson(doc, file);
  file.close();
  
  if (err) return;
  
  JsonArray schedules = doc.as<JsonArray>();
  
  char timeNowStr[8];
  snprintf(timeNowStr, sizeof(timeNowStr), "%02d:%02d", now.hour(), now.minute());
  
  int dayToday = now.dayOfTheWeek() == 0 ? 7 : now.dayOfTheWeek(); // Map 0 (Sunday) to 7
  
  for (JsonObject s : schedules) {
    int active = s["active"] | 0;
    if (active != 1) continue;
    
    String timeStr = s["time"];
    if (timeStr == String(timeNowStr)) {
      // Periksa apakah hari ini termasuk dalam hari aktif jadwal
      JsonArray days = s["days"].as<JsonArray>();
      bool dayMatches = false;
      for (int d : days) {
        if (d == dayToday) {
          dayMatches = true;
          break;
        }
      }
      
      if (dayMatches) {
        int folder = s["folder"] | 1;
        int file = s["file"] | 1;
        String name = s["name"] | "Bel Sekolah";
        
        Serial.print("Membunyikan alarm otomatis: ");
        Serial.println(name);
        
        // Aktifkan Relay Amplifier
        digitalWrite(PIN_RELAY, HIGH);
        isAmplifierOn = true;
        ampOffTime = millis() + 45000; // Amplifier hidup selama 45 detik
        
        delay(1000); // Beri jeda amplifier stabil
        myDFPlayer.playFolder(folder, file);
        
        bellsTodayCount++;
        successBellsCount++;
        lastActiveBell = name + " (" + String(timeNowStr) + " WIB)";
        
        lastPlayedMinute = now.minute();
        lastPlayedDay = now.day();
        break; // Hanya bunyikan satu alarm pada satu waktu
      }
    }
  }
}

void setup() {
  Serial.begin(115200);
  uptimeStart = millis();
  
  // Setup pin relay
  pinMode(PIN_RELAY, OUTPUT);
  digitalWrite(PIN_RELAY, LOW); // Amplifier off by default
  
  // Inisialisasi LittleFS & Konfigurasi
  initFS();
  
  // Setup Wi-Fi
  setupWiFi();
  
  // Inisialisasi RTC DS3231
  if (!rtc.begin()) {
    Serial.println("Gagal mendeteksi RTC DS3231! Pastikan wiring I2C benar.");
  } else {
    if (rtc.lostPower()) {
      Serial.println("RTC kehilangan daya, set waktu ke default compile time...");
      rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    }
    Serial.println("RTC DS3231 siap.");
  }
  
  // Inisialisasi Serial untuk DFPlayer
  #if defined(ESP32)
    mySerial.begin(9600, SERIAL_8N1, PIN_RX, PIN_TX);
  #elif defined(ESP8266)
    mySerial.begin(9600);
  #endif
  
  Serial.println("Mengoneksikan ke DFPlayer Mini...");
  if (!myDFPlayer.begin(mySerial)) {
    Serial.println("Gagal terhubung dengan DFPlayer Mini:");
    Serial.println("1. Pastikan kabel RX/TX tidak terbalik.");
    Serial.println("2. Pastikan kartu MicroSD sudah dimasukkan.");
  } else {
    Serial.println("DFPlayer Mini Online.");
    myDFPlayer.volume(25); // Set Volume (0-30)
  }
  
  // Setup server web
  setupWebServer();
}

void loop() {
  // Jalankan server
  server.handleClient();
  
  // Dapatkan waktu saat ini
  if (rtc.begin()) {
    DateTime now = rtc.now();
    
    // Periksa alarm jadwal
    checkAlarm(now);
  }
  
  // Kontrol Otomatis Matikan Relay Amplifier (untuk efisiensi daya listrik)
  if (isAmplifierOn && millis() > ampOffTime) {
    digitalWrite(PIN_RELAY, LOW);
    isAmplifierOn = false;
    Serial.println("Relay Amplifier dimatikan otomatis (Mode Hemat Daya).");
  }
  
  delay(100);
}
