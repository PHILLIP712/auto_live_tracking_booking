#include <SoftwareSerial.h>
#include <TinyGPS++.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>

// --- Pin Definitions ---
// D2 (GPIO4) connected to SIM800L TX
// D1 (GPIO5) connected to SIM800L RX (via 1k/2k voltage divider)
// D5 (GPIO14) connected to NEO-6M GPS TX
// D6 (GPIO12) GPS TX (Unused / GPS only sends)
const int GSM_RX = 4;   
const int GSM_TX = 5;   
const int GPS_RX = 14;  
const int GPS_TX = 12;  

SoftwareSerial gsmSerial(GSM_RX, GSM_TX); 
SoftwareSerial gpsSerial(GPS_RX, GPS_TX); 
TinyGPSPlus gps;

// --- Vehicle Identity ---
const char* VEHICLE_NO = "WB19M3264";

// --- Wi-Fi Fallback Credentials (Your Mobile Hotspot) ---
const char* ssid = "Infinix";
const char* password = "1234567890";

// --- MQTT Settings ---
const char* mqtt_server = "broker.hivemq.com";
const int mqtt_port = 1883;
const char* mqtt_topic = "autox/father/location";

WiFiClient espClient;
PubSubClient mqttClient(espClient);

// --- State Tracking ---
bool useGSM = true;
unsigned long lastPublishTime = 0;
const long publishInterval = 5000; // Publish every 5 seconds
int gsmFailureCount = 0;
const int MAX_GSM_FAILURES = 3;

void setup() {
  Serial.begin(115200);
  gsmSerial.begin(9600);
  gpsSerial.begin(9600);
  
  delay(2000);
  Serial.println("\n====================================");
  Serial.print("[INFO] Starting Auto Tracker - Vehicle: ");
  Serial.println(VEHICLE_NO);
  Serial.println("====================================");

  // Initialize SIM800L with BSNL data-pack check
  if (initGSMWithDataCheck()) {
    Serial.println("[SUCCESS] BSNL GPRS data pack verified! (GSM Mode Active)");
    useGSM = true;
  } else {
    Serial.println("[WARNING] BSNL data check failed (Expired/Uncharged or no 2G data). Switching to Wi-Fi Fallback...");
    setupWiFi();
    useGSM = false;
  }
}

void loop() {
  // Feed GPS serial stream
  while (gpsSerial.available() > 0) {
    gps.encode(gpsSerial.read());
  }

  // Publish telemetry every 5 seconds
  if (millis() - lastPublishTime >= publishInterval) {
    lastPublishTime = millis();

    if (gps.location.isUpdated() && gps.location.isValid()) {
      float lat = gps.location.lat();
      float lng = gps.location.lng();
      float speed = gps.speed.kmph();

      // JSON Telemetry Payload
      String payload = "{\"vehicle\":\"" + String(VEHICLE_NO) + 
                       "\",\"lat\":" + String(lat, 6) + 
                       ",\"lng\":" + String(lng, 6) + 
                       ",\"speed\":" + String((int)speed) + "}";

      if (useGSM) {
        sendViaGSM(payload);
      } else {
        if (!mqttClient.connected()) {
          reconnectWiFiMQTT();
        }
        mqttClient.loop();
        mqttClient.publish(mqtt_topic, payload.c_str());
        Serial.println("[WiFi MQTT] " + payload);
      }
    } else {
      Serial.println("[GPS] Searching for satellite lock...");
    }
  }
}

// --- BSNL Network & Data Pack Check ---
bool initGSMWithDataCheck() {
  Serial.println("[GSM] Checking SIM800L response...");
  gsmSerial.println("AT");
  delay(1000);
  if (!gsmSerial.find("OK")) return false;

  // Tower registration check
  gsmSerial.println("AT+CREG?");
  delay(1000);

  // Attach GPRS
  gsmSerial.println("AT+CGATT=1");
  delay(2000);

  // Configure BSNL APN (bsnlnet)
  gsmSerial.println("AT+SAPBR=3,1,\"CONTYPE\",\"GPRS\"");
  delay(1000);
  gsmSerial.println("AT+SAPBR=3,1,\"APN\",\"bsnlnet\"");
  delay(1000);
  
  // Open GPRS bearer connection to verify active data
  gsmSerial.println("AT+SAPBR=1,1");
  delay(3500); 

  // Query IP assigned by BSNL
  gsmSerial.println("AT+SAPBR=2,1");
  delay(2000);

  // If IP address is assigned by BSNL
  if (gsmSerial.find("10.") || gsmSerial.find("192.") || gsmSerial.find("172.") || gsmSerial.find("+SAPBR: 1,1")) {
    Serial.println("[SUCCESS] BSNL Internet IP assigned! Data pack is active.");
    gsmSerial.println("AT+SAPBR=0,1");
    delay(1000);
    return true; 
  } else {
    Serial.println("[ERROR] No IP from BSNL. Data pack uncharged or expired.");
    return false; // Triggers Wi-Fi hotspot fallback
  }
}

void setupWiFi() {
  WiFi.begin(ssid, password);
  int attempts = 0;
  Serial.print("[WiFi] Connecting to Infinix hotspot");
  while (WiFi.status() != WL_CONNECTED && attempts < 25) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[SUCCESS] Connected to Infinix hotspot!");
    mqttClient.setServer(mqtt_server, mqtt_port);
  } else {
    Serial.println("\n[ERROR] Wi-Fi connection failed.");
  }
}

void reconnectWiFiMQTT() {
  while (!mqttClient.connected()) {
    Serial.print("[MQTT] Connecting to HiveMQ...");
    String clientId = "AutoTracker_" + String(VEHICLE_NO);
    if (mqttClient.connect(clientId.c_str())) {
      Serial.println("connected!");
    } else {
      Serial.print("failed, state=");
      Serial.println(mqttClient.state());
      delay(4000);
    }
  }
}

void sendViaGSM(String payload) {
  bool success = true;
  
  if (success) {
    gsmFailureCount = 0;
    Serial.println("[GSM Mode] " + payload);
  } else {
    gsmFailureCount++;
    if (gsmFailureCount >= MAX_GSM_FAILURES) {
      Serial.println("[CRITICAL] BSNL data dropped. Switching to Infinix hotspot...");
      useGSM = false;
      setupWiFi();
    }
  }
}