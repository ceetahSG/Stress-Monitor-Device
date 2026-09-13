#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "MAX30105.h"
#include "heartRate.h"
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// --- WiFi Settings ---
const char* ssid = "Zenetic Esports";
const char* password = "oneStudio";
const char* serverUrl = "http://192.168.0.179:5000/api/real-time-data";
const char* endSessionUrl = "http://192.168.0.179:5000/api/end-session";

// --- Hardware ---
#define MOTOR_PIN 4
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64

// --- Logic Constants ---
#define GRACE_PERIOD_MS 2000  
#define IR_THRESHOLD 50000

// --- Objects ---
MAX30105 particleSensor;
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
HTTPClient http;

// --- Global Variables ---
int currentSessionId = 0;

// State Machine
bool fingerPhysical = false;
bool sessionActive = false;
unsigned long graceTimerStart = 0;
bool inGracePeriod = false;

// Timers
unsigned long lastUiUpdate = 0;
unsigned long lastDataLog = 0;
unsigned long lastBeatTime = 0;
unsigned long lastBeatDetectedTime = 0;
unsigned long lastWiFiCheck = 0;
unsigned long sessionStartTime = 0;

// Math Variables
const byte RATE_SIZE = 4;
byte rates[RATE_SIZE]; 
byte rateSpot = 0;
float beatsPerMinute;
int beatAvg = 0;

// BEAT VARIABILITY FOR STRESS
float lastBeatInterval = 0;
float beatIntervalVariation = 0;
float stress = 20;

// SpO2 Estimator
double minRed = 200000, maxRed = 0;
double minIR = 200000, maxIR = 0;
int estimatedSpO2 = 98;

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  pinMode(MOTOR_PIN, OUTPUT);
  digitalWrite(MOTOR_PIN, LOW);

  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("❌ Display failed!");
    while(1);
  }
  display.setTextColor(WHITE);
  
  showStatus("Connecting WiFi...");
  connectToWiFi();

  if (!particleSensor.begin(Wire, I2C_SPEED_FAST)) {
    Serial.println("❌ Sensor Missing!");
    showStatus("Sensor Missing");
    while (1);
  }

  particleSensor.setup(); 
  particleSensor.setPulseAmplitudeRed(0x0A);
  particleSensor.setPulseAmplitudeGreen(0); 
  
  Serial.println("✅ Setup Complete!");
  showIdleScreen();
}

void connectToWiFi() {
  Serial.print("🔗 Connecting to WiFi: ");
  Serial.println(ssid);
  
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  
  int attempts = 0;
  while(WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if(WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.println("✅ WiFi Connected!");
    Serial.print("📍 IP: ");
    Serial.println(WiFi.localIP());
    
    showStatus("WiFi OK");
    delay(500);
  } else {
    Serial.println("\n❌ WiFi Failed!");
    showStatus("WiFi Failed!");
  }
}

void loop() {
  if (millis() - lastWiFiCheck > 5000) {
    if(WiFi.status() != WL_CONNECTED) {
      Serial.println("⚠️ WiFi disconnected");
      connectToWiFi();
    }
    lastWiFiCheck = millis();
  }

  long irValue = particleSensor.getIR();
  long redValue = particleSensor.getRed();

  // Finger detection
  if (irValue > IR_THRESHOLD) {
    if (!fingerPhysical) {
       Serial.println("🎯 Finger detected");
       fingerPhysical = true;
       inGracePeriod = false;
       if (!sessionActive) startSession(); 
    }
  } else {
    if (fingerPhysical) {
       Serial.println("👋 Finger removed");
       fingerPhysical = false;
    }
  }

  if (sessionActive) {
     if (fingerPhysical) {
        processSignal(irValue, redValue);
        inGracePeriod = false; 

        if (stress > 60) {
          digitalWrite(MOTOR_PIN, HIGH);
        } else {
          digitalWrite(MOTOR_PIN, LOW);
        }

     } else {
        digitalWrite(MOTOR_PIN, LOW);
        
        if (!inGracePeriod) {
           inGracePeriod = true;
           graceTimerStart = millis();
        } else {
           if (millis() - graceTimerStart > GRACE_PERIOD_MS) {
              endSessionAndUpload();
           }
        }
     }

     unsigned long currentMillis = millis();

     if (currentMillis - lastUiUpdate > 250) {
        updateDisplay(); 
        lastUiUpdate = currentMillis;
     }

     // INFINITE: Send data every second, no 120s limit!
     if (fingerPhysical && currentMillis - lastDataLog > 1000) {
        sendRealTimeData(beatAvg, estimatedSpO2, stress);
        lastDataLog = currentMillis;
     }

  } else {
     digitalWrite(MOTOR_PIN, LOW);
     static long lastIdle = 0;
     if (millis() - lastIdle > 1000) {
        showIdleScreen();
        lastIdle = millis();
     }
  }
}

void startSession() {
  sessionActive = true;
  currentSessionId = 0;
  beatAvg = 0;
  stress = 20;
  lastBeatInterval = 0;
  beatIntervalVariation = 0;
  minRed = 200000; maxRed = 0;
  minIR = 200000; maxIR = 0;
  lastDataLog = millis();
  sessionStartTime = millis();
  display.clearDisplay();
  Serial.println("📊 Session started - INFINITE MODE");
}

void processSignal(long irValue, long redValue) {
  if (checkForBeat(irValue) == true) {
    long delta = millis() - lastBeatTime;
    lastBeatTime = millis();
    lastBeatDetectedTime = millis();

    if (delta > 250 && delta < 2000) {
       beatsPerMinute = 60 / (delta / 1000.0);
       
       if (beatsPerMinute < 255 && beatsPerMinute > 20) {
         rates[rateSpot++] = (byte)beatsPerMinute; 
         rateSpot %= RATE_SIZE;
         beatAvg = 0;
         for (byte x = 0 ; x < RATE_SIZE ; x++) beatAvg += rates[x];
         beatAvg /= RATE_SIZE;
       }

       if (lastBeatInterval > 0) {
          float intervalDiff = abs(delta - lastBeatInterval);
          beatIntervalVariation = (0.7 * beatIntervalVariation) + (0.3 * intervalDiff);
          
          if (beatIntervalVariation < 20) {
             stress = 70.0 + (20.0 - beatIntervalVariation) * 1.5;
          } 
          else if (beatIntervalVariation < 50) {
             stress = 40.0 + ((50.0 - beatIntervalVariation) / 30.0) * 30.0;
          } 
          else {
             stress = (beatIntervalVariation - 50.0) / 10.0;
          }

          if (stress < 0) stress = 0;
          if (stress > 100) stress = 100;
       }

       lastBeatInterval = delta;
    }
  }

  // SpO2 Tracking
  if (redValue < minRed) minRed = redValue;
  if (redValue > maxRed) maxRed = redValue;
  if (irValue < minIR) minIR = irValue;
  if (irValue > maxIR) maxIR = irValue;

  static int spo2Counter = 0;
  spo2Counter++;
  if (spo2Counter > 500) {
     double redAC = maxRed - minRed;
     double irAC = maxIR - minIR;
     if (irAC > 0 && redAC > 0) {
        float R = (redAC / maxRed) / (irAC / maxIR);
        float spo2 = 104 - 17 * R; 
        if (spo2 > 100) spo2 = 100;
        if (spo2 < 80) spo2 = 80;
        estimatedSpO2 = (int)spo2;
     }
     minRed = 200000; maxRed = 0;
     minIR = 200000; maxIR = 0;
     spo2Counter = 0;
  }
}

void sendRealTimeData(int bpm, int spo2_val, float stress_val) {
  if(WiFi.status() != WL_CONNECTED) {
    connectToWiFi();
    delay(1000);
    if(WiFi.status() != WL_CONNECTED) {
      return;
    }
  }

  if(bpm == 0 && stress_val == 0) {
    return;
  }

  http.end();
  http.begin(serverUrl);
  http.addHeader("Content-Type", "application/json");
  
  DynamicJsonDocument doc(256);
  if(currentSessionId > 0) {
    doc["session_id"] = currentSessionId;
  }
  doc["bpm"] = bpm;
  doc["spo2"] = spo2_val;
  doc["stress"] = stress_val;
  
  String jsonStr;
  serializeJson(doc, jsonStr);
  
  Serial.print("📤 Sending: ");
  Serial.println(jsonStr);
  
  int httpCode = http.POST(jsonStr);
  
  if (httpCode == 201 || httpCode == 200) {
    String response = http.getString();
    DynamicJsonDocument responseDoc(256);
    deserializeJson(responseDoc, response);
    
    if (responseDoc.containsKey("session_id") && currentSessionId == 0) {
      currentSessionId = responseDoc["session_id"];
      Serial.printf("✅ Session ID: %d\n", currentSessionId);
    }
    
    Serial.printf("✅ Data sent - BPM: %d, SpO2: %d, Stress: %.1f\n", 
                  bpm, spo2_val, stress_val);
  } else {
    Serial.printf("❌ HTTP Error: %d\n", httpCode);
  }
  
  http.end();
}

void endSessionAndUpload() {
  sessionActive = false;
  inGracePeriod = false;

  unsigned long sessionDuration = (millis() - sessionStartTime) / 1000;
  Serial.printf("🛑 Session ended after %lu seconds\n", sessionDuration);
  
  display.clearDisplay();
  display.setCursor(0, 20);
  display.setTextSize(2);
  display.println("Saving...");
  display.display();

  if (WiFi.status() == WL_CONNECTED && currentSessionId > 0) {
      http.begin(endSessionUrl);
      http.addHeader("Content-Type", "application/json");

      DynamicJsonDocument doc(256);
      doc["session_id"] = currentSessionId;
      String jsonStr;
      serializeJson(doc, jsonStr);
      
      int code = http.POST(jsonStr);
      
      if(code == 200) {
        Serial.println("✅ Session saved");
        display.clearDisplay();
        display.setCursor(0, 20);
        display.setTextSize(2);
        display.println("Saved!");
        display.display();
      } else {
        Serial.printf("❌ Error: HTTP %d\n", code);
      }
      
      http.end();
  }
  
  delay(2000);
  currentSessionId = 0;
}

void updateDisplay() {
  display.clearDisplay();
  
  unsigned long elapsed = (millis() - sessionStartTime) / 1000;
  
  display.setTextSize(1);
  display.setCursor(0,0);
  display.print("Time: "); display.print(elapsed); display.print("s");

  bool beatFlash = (millis() - lastBeatDetectedTime < 150);
  if (beatFlash) {
    display.fillCircle(118, 5, 4, WHITE);
    display.fillCircle(124, 5, 4, WHITE);
  } else {
    display.drawCircle(118, 5, 4, WHITE);
    display.drawCircle(124, 5, 4, WHITE);
  }

  display.setCursor(35, 18);
  display.setTextSize(3); 
  display.print(beatAvg > 0 ? String(beatAvg) : "--");
  
  display.setTextSize(1);
  display.setCursor(95, 35);
  display.print("BPM");

  display.drawLine(0, 48, 128, 48, WHITE);
  display.setCursor(0, 54);
  display.print("SpO2: "); display.print(estimatedSpO2); display.print("%");
  
  display.setCursor(70, 54);
  String stressStr = "OK";
  if (stress > 0) {
     if (stress > 70) stressStr = "HIGH";
     else if (stress > 40) stressStr = "MED";
     else stressStr = "LOW";
  }
  display.print("Str: "); display.print((int)stress);
  
  display.display();
}

void showStatus(const char* msg) {
  display.clearDisplay();
  display.setCursor(0,0);
  display.setTextSize(1);
  display.println(msg);
  display.display();
}

void showIdleScreen() {
  display.clearDisplay();
  display.setTextSize(2);
  display.setCursor(30, 10);
  display.println("Ready");
  display.setTextSize(1);
  display.setCursor(20, 35);
  display.println("Place Finger");
  display.display();
}