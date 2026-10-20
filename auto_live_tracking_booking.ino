#define TINY_GSM_MODEM_SIM800 // Define modem for TinyGSM

#include <SoftwareSerial.h>
#include <TinyGPS++.h>
#include <ESP8266WiFi.h>
#include <TinyGsmClient.h>
#include <PubSubClient.h>

// --- Pin Definitions ---
// D2 (GPIO4) connected to SIM800L TX
// D1 (GPIO5) connected to SIM800L RX (via voltage divider)
// D5 (GPIO14) connected to NEO-6M GPS TX
const int GSM_RX = 4;   
const int GSM_TX = 5;   
const int GPS_RX = 14;  
const int GPS_TX = 12;  // Unused

SoftwareSerial gsmSerial(GSM_RX, GSM_TX); 
SoftwareSerial gpsSerial(GPS_RX, GPS_TX); 
TinyGPSPlus gps;

// --- Vehicle Identity ---
const char* VEHICLE_NO = "WB19M3264";

// --- Network Credentials ---
const char* apn = "bsnlnet";
const char* gprsUser = "";
const char* gprsPass = "";

const char* ssid = "Infinix";
const char* password = "1234567890";

// --- MQTT Settings ---
const char* mqtt_server = "broker.hivemq.com";
const int mqtt_port = 1883;
const char* mqtt_topic = "autox/father/location";

// --- Clients ---
TinyGsm modem(gsmSerial);
TinyGsmClient gsmClient(modem);
WiFiClient wifiClient;
PubSubClient mqttClient;

// --- State Tracking ---
bool useGSM = true;
unsigned long lastPublishTime = 0;
const long publishInterval = 5000; // Publish every 5 seconds
int gsmFailureCount = 0;
const int MAX_GSM_FAILURES = 3;

void setup() {
  Serial.begin(115200);
  delay(10);
  
  Serial.println("\n====================================");
  Serial.print("[INFO] Starting Auto Tracker - Vehicle: ");
  Serial.println(VEHICLE_NO);
  Serial.println("====================================");

  // Initialize GSM
  gsmSerial.begin(9600);
  Serial.println("[GSM] Initializing SIM800L...");
  
  if (modem.restart()) {
    Serial.println("[GSM] Modem restarted successfully.");
    Serial.print("[GSM] Connecting to BSNL GPRS...");
    if (modem.gprsConnect(apn, gprsUser, gprsPass)) {
      Serial.println(" OK!");
      useGSM = true;
      mqttClient.setClient(gsmClient);
    } else {
      Serial.println(" FAILED. Data uncharged or poor signal.");
      triggerWiFiFallback();
    }
  } else {
    Serial.println("[GSM] Modem not responding.");
    triggerWiFiFallback();
  }

  mqttClient.setServer(mqtt_server, mqtt_port);
  gpsSerial.begin(9600);
}

void loop() {
  // ESP8266 SoftwareSerial limitation: We must explicitly listen to the GPS 
  // when we want coordinates, because the GSM module might interrupt it.
  gpsSerial.listen();
  
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

      String payload = "{\"vehicle\":\"" + String(VEHICLE_NO) + 
                       "\",\"lat\":" + String(lat, 6) + 
                       ",\"lng\":" + String(lng, 6) + 
                       ",\"speed\":" + String((int)speed) + "}";

      // Switch listening back to GSM module to send data
      gsmSerial.listen(); 
      publishTelemetry(payload);

    } else {
      Serial.println("[GPS] Searching for satellite lock in West Bengal sky...");
    }
  }
}

void triggerWiFiFallback() {
  Serial.println("[WARNING] Switching to Wi-Fi Fallback (Infinix Hotspot)...");
  useGSM = false;
  WiFi.begin(ssid, password);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[SUCCESS] Connected to Infinix hotspot!");
    mqttClient.setClient(wifiClient); // Point MQTT to Wi-Fi instead of GSM
  } else {
    Serial.println("\n[ERROR] Wi-Fi connection failed.");
  }
}

void reconnectMQTT() {
  while (!mqttClient.connected()) {
    Serial.print("[MQTT] Connecting to HiveMQ...");
    String clientId = "AutoTracker_" + String(VEHICLE_NO) + "_" + String(random(0xffff), HEX);
    
    if (mqttClient.connect(clientId.c_str())) {
      Serial.println(" connected!");
      gsmFailureCount = 0; // Reset failures on successful connect
    } else {
      Serial.print(" failed, state=");
      Serial.println(mqttClient.state());
      
      if (useGSM) {
        gsmFailureCount++;
        if (gsmFailureCount >= MAX_GSM_FAILURES) {
           Serial.println("[CRITICAL] BSNL connection dropping repeatedly.");
           triggerWiFiFallback();
           return;
        }
      }
      delay(3000);
    }
  }
}

void publishTelemetry(String payload) {
  if (!mqttClient.connected()) {
    reconnectMQTT();
  }
  
  if (mqttClient.connected()) {
    mqttClient.loop();
    if (mqttClient.publish(mqtt_topic, payload.c_str())) {
      if (useGSM) {
        Serial.println("[GSM Mode] Published: " + payload);
      } else {
        Serial.println("[WiFi Mode] Published: " + payload);
      }
    } else {
      Serial.println("[ERROR] MQTT Publish failed.");
    }
  }
}