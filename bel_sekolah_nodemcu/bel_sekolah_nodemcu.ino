/*
 * Sistem Bel Sekolah Otomatis - SMPN Purworejo
 * Khusus Board: NodeMCU ESP8266
 * 
 * Hubungan Pin NodeMCU:
 * 1. RTC DS3231: SDA -> Pin D2, SCL -> Pin D1, VCC -> 3V3, GND -> GND
 * 2. DFPlayer Mini: TX -> Pin D5, RX -> Pin D6 (via Resistor 1K Ohm), VCC -> VIN (5V), GND -> GND
 * 3. Modul Relay: IN -> Pin D7, VCC -> VIN / 3V3, GND -> GND
 */

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <SoftwareSerial.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <RTClib.h>
#include <DFRobotDFPlayerMini.h>

// Definisi Pin NodeMCU ESP8266
#define PIN_RX D5         // RX NodeMCU ke TX DFPlayer
#define PIN_TX D6         // TX NodeMCU ke RX DFPlayer (Gunakan Resistor 1K Ohm)
#define PIN_RELAY D7      // Pin output untuk mengaktifkan Relay Amplifier

// Inisialisasi Objek
ESP8266WebServer server(80);
SoftwareSerial mySerial(PIN_RX, PIN_TX); 
DFRobotDFPlayerMini myDFPlayer;
RTC_DS3231 rtc;

// Struktur Konfigurasi WiFi
struct Config {
  char wifi_ssid[32] = "Bel_Sekolah_AP";
  char wifi_password[64] = "12345678";
  bool is_ap = true;
};
Config systemConfig;

// Variabel State Sistem
unsigned long uptimeStart;
int bellsTodayCount = 0;
int successBellsCount = 0;
String lastActiveBell = "Belum berbunyi hari ini";
int lastPlayedMinute = -1;
int lastPlayedDay = -1;
bool isAmplifierOn = false;
unsigned long ampOffTime = 0;

// Path Penyimpanan File di LittleFS
const char* configFilePath = "/config.json";
const char* schedulesFilePath = "/schedules.json";

// Inisialisasi LittleFS File System
void initFS() {
  if (!LittleFS.begin()) {
    Serial.println("Gagal me-mount LittleFS! Silakan format filesystem.");
    return;
  }
  Serial.println("LittleFS berhasil dimuat.");
}

// Membaca Konfigurasi WiFi dari LittleFS
void loadConfiguration() {
  if (!LittleFS.exists(configFilePath)) {
    Serial.println("File config.json tidak ditemukan, menggunakan WiFi AP bawaan.");
    saveConfiguration();
    return;
  }

  File configFile = LittleFS.open(configFilePath, "r");
  if (!configFile) return;

  StaticJsonDocument<256> doc;
  DeserializationError error = deserializeJson(doc, configFile);
  configFile.close();

  if (error) {
    Serial.println("Gagal parsing config.json.");
    return;
  }

  strlcpy(systemConfig.wifi_ssid, doc["wifi_ssid"] | "Bel_Sekolah_AP", sizeof(systemConfig.wifi_ssid));
  strlcpy(systemConfig.wifi_password, doc["wifi_password"] | "12345678", sizeof(systemConfig.wifi_password));
  systemConfig.is_ap = doc["is_ap"] | true;
}

// Menyimpan Konfigurasi WiFi ke LittleFS
void saveConfiguration() {
  File configFile = LittleFS.open(configFilePath, "w");
  if (!configFile) return;

  StaticJsonDocument<256> doc;
  doc["wifi_ssid"] = systemConfig.wifi_ssid;
  doc["wifi_password"] = systemConfig.wifi_password;
  doc["is_ap"] = systemConfig.is_ap;

  serializeJson(doc, configFile);
  configFile.close();
}

// Pengaturan Wi-Fi (Mode AP atau Station)
void setupWiFi() {
  loadConfiguration();
  
  if (systemConfig.is_ap) {
    Serial.println("Menyalakan WiFi Mode Access Point (AP)...");
    WiFi.mode(WIFI_AP);
    WiFi.softAP(systemConfig.wifi_ssid, systemConfig.wifi_password);
    Serial.print("SSID AP: "); Serial.println(systemConfig.wifi_ssid);
    Serial.print("IP Web Server: "); Serial.println(WiFi.softAPIP());
  } else {
    Serial.print("Menghubungkan ke WiFi Router: ");
    Serial.println(systemConfig.wifi_ssid);
    WiFi.mode(WIFI_STA);
    WiFi.begin(systemConfig.wifi_ssid, systemConfig.wifi_password);
    
    int retries = 0;
    while (WiFi.status() != WL_CONNECTED && retries < 20) {
      delay(500);
      Serial.print(".");
      retries++;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\nWiFi Terhubung!");
      Serial.print("IP Web Server: "); Serial.println(WiFi.localIP());
    } else {
      Serial.println("\nGagal terhubung. Kembali ke Mode AP Darurat...");
      WiFi.mode(WIFI_AP);
      WiFi.softAP("Bel_Sekolah_DARURAT", "12345678");
      Serial.print("IP Web Server AP Darurat: "); Serial.println(WiFi.softAPIP());
    }
  }
}

// Menghitung Uptime
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

// API Handler: Mengirim Status Sistem
void handleGetStatus() {
  if (!rtc.begin()) {
    server.send(500, "text/plain", "RTC DS3231 Error");
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
  doc["day"] = now.dayOfTheWeek() == 0 ? 7 : now.dayOfTheWeek();
  doc["system_status"] = "OK";
  doc["wifi_ssid"] = systemConfig.wifi_ssid;
  doc["wifi_signal"] = systemConfig.is_ap ? "Mode AP (Aktif)" : String(WiFi.RSSI()) + " dBm";
  doc["bells_today"] = bellsTodayCount;
  doc["success_rate"] = String(successBellsCount) + "/" + String(bellsTodayCount);
  doc["uptime"] = getUptime();
  doc["last_active"] = lastActiveBell;
  
  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

// API Handler: Mengambil List Jadwal
void handleGetSchedules() {
  if (!LittleFS.exists(schedulesFilePath)) {
    server.send(200, "application/json", "[]");
    return;
  }
  File file = LittleFS.open(schedulesFilePath, "r");
  server.streamFile(file, "application/json");
  file.close();
}

// API Handler: Menyimpan/Mengedit Jadwal Bel
void handleSaveSchedule() {
  if (!server.hasArg("plain")) {
    server.send(400, "text/plain", "Data POST Kosong");
    return;
  }
  
  String body = server.arg("plain");
  StaticJsonDocument<256> newS;
  DeserializationError err = deserializeJson(newS, body);
  if (err) {
    server.send(400, "text/plain", "JSON Error");
    return;
  }
  
  DynamicJsonDocument doc(8192);
  if (LittleFS.exists(schedulesFilePath)) {
    File file = LittleFS.open(schedulesFilePath, "r");
    deserializeJson(doc, file);
    file.close();
  }
  
  JsonArray arr = doc.as<JsonArray>();
  int id = newS["id"] | 0;
  
  if (id == 0) {
    int maxId = 0;
    for (JsonObject s : arr) {
      if ((s["id"] | 0) > maxId) maxId = s["id"];
    }
    newS["id"] = maxId + 1;
    arr.add(newS);
  } else {
    bool found = false;
    for (int i = 0; i < arr.size(); i++) {
      if (arr[i]["id"] == id) {
        arr[i] = newS;
        found = true;
        break;
      }
    }
    if (!found) arr.add(newS);
  }
  
  File file = LittleFS.open(schedulesFilePath, "w");
  serializeJson(doc, file);
  file.close();
  server.send(200, "application/json", "{\"status\":\"success\",\"message\":\"Jadwal disimpan\"}");
}

// API Handler: Menghapus Jadwal Bel
void handleDeleteSchedule() {
  if (!server.hasArg("plain")) {
    server.send(400, "text/plain", "Data POST Kosong");
    return;
  }
  
  String body = server.arg("plain");
  StaticJsonDocument<128> delReq;
  deserializeJson(delReq, body);
  int id = delReq["id"] | 0;
  
  DynamicJsonDocument doc(8192);
  File file = LittleFS.open(schedulesFilePath, "r");
  deserializeJson(doc, file);
  file.close();
  
  JsonArray arr = doc.as<JsonArray>();
  for (int i = 0; i < arr.size(); i++) {
    if (arr[i]["id"] == id) {
      arr.remove(i);
      break;
    }
  }
  
  File fileW = LittleFS.open(schedulesFilePath, "w");
  serializeJson(doc, fileW);
  fileW.close();
  server.send(200, "application/json", "{\"status\":\"success\",\"message\":\"Jadwal dihapus\"}");
}

// API Handler: Bunyikan Manual
void handlePlayBell() {
  if (!server.hasArg("plain")) {
    server.send(400, "text/plain", "Data POST Kosong");
    return;
  }
  
  String body = server.arg("plain");
  StaticJsonDocument<128> playReq;
  deserializeJson(playReq, body);
  
  int folder = playReq["folder"] | 1;
  int file = playReq["file"] | 1;
  
  // Aktifkan Relay Amplifier
  digitalWrite(PIN_RELAY, HIGH);
  isAmplifierOn = true;
  ampOffTime = millis() + 45000; // Hidupkan Amplifier selama 45 detik
  
  delay(1000); // Beri jeda agar amplifier menyala stabil
  myDFPlayer.playFolder(folder, file);
  
  server.send(200, "application/json", "{\"status\":\"success\",\"message\":\"Bel manual berbunyi\"}");
}

// API Handler: Mengatur Waktu RTC
void handleSetTime() {
  if (!server.hasArg("plain")) {
    server.send(400, "text/plain", "Data POST Kosong");
    return;
  }
  
  String body = server.arg("plain");
  StaticJsonDocument<128> timeReq;
  deserializeJson(timeReq, body);
  
  String datetime = timeReq["datetime"]; // Format: "YYYY-MM-DDTHH:MM:SS"
  if (datetime.length() >= 19) {
    int yr = datetime.substring(0, 4).toInt();
    int mt = datetime.substring(5, 7).toInt();
    int dy = datetime.substring(8, 10).toInt();
    int hr = datetime.substring(11, 13).toInt();
    int mn = datetime.substring(14, 16).toInt();
    int sc = datetime.substring(17, 19).toInt();
    
    rtc.adjust(DateTime(yr, mt, dy, hr, mn, sc));
    server.send(200, "application/json", "{\"status\":\"success\",\"message\":\"RTC terupdate\"}");
  } else {
    server.send(400, "text/plain", "Format salah");
  }
}

// API Handler: Menyimpan Pengaturan WiFi Baru
void handleSetWiFi() {
  if (!server.hasArg("plain")) {
    server.send(400, "text/plain", "Data POST Kosong");
    return;
  }
  
  String body = server.arg("plain");
  StaticJsonDocument<128> wifiReq;
  deserializeJson(wifiReq, body);
  
  String ssid = wifiReq["ssid"];
  String password = wifiReq["password"] | "";
  
  strlcpy(systemConfig.wifi_ssid, ssid.c_str(), sizeof(systemConfig.wifi_ssid));
  strlcpy(systemConfig.wifi_password, password.c_str(), sizeof(systemConfig.wifi_password));
  systemConfig.is_ap = false;
  
  saveConfiguration();
  server.send(200, "application/json", "{\"status\":\"success\",\"message\":\"WiFi Disimpan. Restarting...\"}");
  
  delay(3000);
  ESP.restart();
}

// Melayani file index.html dari LittleFS
void handleServeHTML() {
  if (LittleFS.exists("/index.html")) {
    File file = LittleFS.open("/index.html", "r");
    server.streamFile(file, "text/html");
    file.close();
  } else {
    server.send(404, "text/plain", "index.html tidak ditemukan di LittleFS!");
  }
}

// Setup Rute Web Server
void setupWebServer() {
  server.on("/", HTTP_GET, handleServeHTML);
  server.on("/index.html", HTTP_GET, handleServeHTML);
  
  // REST API Endpoints
  server.on("/api/status", HTTP_GET, handleGetStatus);
  server.on("/api/schedules", HTTP_GET, handleGetSchedules);
  server.on("/api/schedules/save", HTTP_POST, handleSaveSchedule);
  server.on("/api/schedules/delete", HTTP_POST, handleDeleteSchedule);
  server.on("/api/play", HTTP_POST, handlePlayBell);
  server.on("/api/settings/time", HTTP_POST, handleSetTime);
  server.on("/api/settings/wifi", HTTP_POST, handleSetWiFi);
  
  server.begin();
  Serial.println("HTTP Server berjalan.");
}

// Cek Alarm Setiap Menit
void checkAlarm(DateTime now) {
  if (now.second() != 0) return;
  if (now.minute() == lastPlayedMinute && now.day() == lastPlayedDay) return;
  if (!LittleFS.exists(schedulesFilePath)) return;
  
  File file = LittleFS.open(schedulesFilePath, "r");
  if (!file) return;
  
  DynamicJsonDocument doc(8192);
  deserializeJson(doc, file);
  file.close();
  
  JsonArray arr = doc.as<JsonArray>();
  char timeNowStr[8];
  snprintf(timeNowStr, sizeof(timeNowStr), "%02d:%02d", now.hour(), now.minute());
  
  int dayToday = now.dayOfTheWeek() == 0 ? 7 : now.dayOfTheWeek();
  
  for (JsonObject s : arr) {
    if ((s["active"] | 0) != 1) continue;
    
    String timeStr = s["time"];
    if (timeStr == String(timeNowStr)) {
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
        
        Serial.print("BUNYI OTOMATIS: "); Serial.println(name);
        
        // Aktifkan Relay Amplifier
        digitalWrite(PIN_RELAY, HIGH);
        isAmplifierOn = true;
        ampOffTime = millis() + 45000;
        
        delay(1000);
        myDFPlayer.playFolder(folder, file);
        
        bellsTodayCount++;
        successBellsCount++;
        lastActiveBell = name + " (" + String(timeNowStr) + " WIB)";
        lastPlayedMinute = now.minute();
        lastPlayedDay = now.day();
        break;
      }
    }
  }
}

void setup() {
  Serial.begin(115200);
  uptimeStart = millis();
  
  // Konfigurasi pin Relay
  pinMode(PIN_RELAY, OUTPUT);
  digitalWrite(PIN_RELAY, LOW); // Mati secara default
  
  initFS();
  setupWiFi();
  
  // Hubungkan ke DS3231 RTC
  if (!rtc.begin()) {
    Serial.println("Modul RTC DS3231 tidak terdeteksi!");
  } else {
    if (rtc.lostPower()) {
      Serial.println("RTC kehilangan daya baterai, menyinkronkan waktu compile.");
      rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    }
  }
  
  // Mulai Komunikasi Serial DFPlayer
  mySerial.begin(9600);
  Serial.println("Menghubungkan ke DFPlayer Mini...");
  if (!myDFPlayer.begin(mySerial)) {
    Serial.println("Koneksi ke DFPlayer gagal! Periksa kabel TX/RX.");
  } else {
    Serial.println("DFPlayer Mini terkoneksi.");
    myDFPlayer.volume(25); // Set volume awal
  }
  
  setupWebServer();
}

void loop() {
  server.handleClient();
  
  if (rtc.begin()) {
    DateTime now = rtc.now();
    checkAlarm(now);
  }
  
  // Otomatis matikan Amplifier
  if (isAmplifierOn && millis() > ampOffTime) {
    digitalWrite(PIN_RELAY, LOW);
    isAmplifierOn = false;
    Serial.println("Amplifier dimatikan otomatis.");
  }
  
  delay(100);
}
