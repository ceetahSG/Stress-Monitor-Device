#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "MAX30105.h"
#include "heartRate.h"
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// --- WiFi Settings ---
const char* ssid = "cheetah";
const char* password = "cheetah123";
const char* serverUrl = "http://192.168.137.1:5000/api/real-time-data";
const char* endSessionUrl = "http://192.168.137.1:5000/api/end-session";

// --- Hardware ---
#define MOTOR_PIN 4
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64

// --- Logic Constants ---
#define MAX_SAMPLES 120       // 2 Minutes
#define GRACE_PERIOD_MS 2000  
#define STRESS_HIGH_THRESH 25 // Below 25 = High Stress
#define STRESS_MED_THRESH 40  // Below 40 = Medium, Above 40 = OK
#define IR_THRESHOLD 50000    // Finger detection threshold

// --- Objects ---
MAX30105 particleSensor;
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
HTTPClient http;

// --- Global Variables ---
struct Record {
  int bpm;
  int spo2;
  float stress;
};
Record sessionData[MAX_SAMPLES];
int dataCount = 0;
int currentSessionId = 0;

// State Machine
bool fingerPhysical = false;
bool sessionActive = false;
unsigned long graceTimerStart = 0;
bool inGracePeriod = false;

// Timers for UI
unsigned long lastUiUpdate = 0;
unsigned long lastDataLog = 0;
unsigned long lastBeatTime = 0;
unsigned long lastBeatDetectedTime = 0;
unsigned long lastWiFiCheck = 0;

// Math Variables
const byte RATE_SIZE = 4;
byte rates[RATE_SIZE]; 
byte rateSpot = 0;
float beatsPerMinute;
int beatAvg = 0;

// HRV
float rrIntervals[10];
int rrIndex = 0;
float rmssd = 0;

// SpO2 Estimator
double minRed = 200000, maxRed = 0;
double minIR = 200000, maxIR = 0;
int estimatedSpO2 = 98;

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  pinMode(MOTOR_PIN, OUTPUT);
  digitalWrite(MOTOR_PIN, LOW);

  // Initialize Display
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("❌ Display failed!");
    while(1);
  }
  display.setTextColor(WHITE);
  
  // Connect WiFi
  showStatus("Connecting WiFi...");
  connectToWiFi();

  // Initialize Sensor
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
  // Check WiFi periodically
  if (millis() - lastWiFiCheck > 5000) {
    if(WiFi.status() != WL_CONNECTED) {
      Serial.println("⚠️ WiFi disconnected - Reconnecting...");
      connectToWiFi();
    }
    lastWiFiCheck = millis();
  }

  // 1. READ SENSOR
  long irValue = particleSensor.getIR();
  long redValue = particleSensor.getRed();

  // 2. FINGER CHECK
  if (irValue > IR_THRESHOLD) {
    if (!fingerPhysical) {
       // Finger just touched!
       Serial.println("🎯 Finger detected - Starting session");
       fingerPhysical = true;
       inGracePeriod = false;
       if (!sessionActive) startSession(); 
    }
  } else {
    if (fingerPhysical) {
       // Finger just removed
       Serial.println("👋 Finger removed - Grace period started");
       fingerPhysical = false;
    }
  }

  // 3. LOGIC HANDLER
  if (sessionActive) {
     if (fingerPhysical) {
        // Normal Operation
        processSignal(irValue, redValue);
        
        // Reset grace period flag if we recovered
        inGracePeriod = false; 

        // Vibration Logic (Based on Category)
        if (rmssd > 0 && rmssd < STRESS_HIGH_THRESH) {
          digitalWrite(MOTOR_PIN, HIGH);
        } else {
          digitalWrite(MOTOR_PIN, LOW);
        }

     } else {
        // Finger Missing - Handle Grace Period
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

     // 4. TIME-DRIVEN UPDATES
     unsigned long currentMillis = millis();

     // A. UI Update (Every 250ms)
     if (currentMillis - lastUiUpdate > 250) {
        updateDisplay(); 
        lastUiUpdate = currentMillis;
     }

     // B. Data Logging (Every 1000ms)
     if (fingerPhysical && currentMillis - lastDataLog > 1000) {
        if (dataCount < MAX_SAMPLES) {
           // Check if data is fresh
           bool isFresh = (millis() - lastBeatDetectedTime < 2500);

           sessionData[dataCount].bpm = isFresh ? beatAvg : 0;
           sessionData[dataCount].spo2 = estimatedSpO2;
           sessionData[dataCount].stress = isFresh ? rmssd : 0;
           
           // Send to server
           sendRealTimeData(sessionData[dataCount].bpm, 
                           sessionData[dataCount].spo2, 
                           sessionData[dataCount].stress);
           
           dataCount++;
        }
        lastDataLog = currentMillis;
     }

  } else {
     // Idle Mode
     digitalWrite(MOTOR_PIN, LOW);
     static long lastIdle = 0;
     if (millis() - lastIdle > 1000) {
        showIdleScreen();
        lastIdle = millis();
     }
  }
}

// --- LOGIC FUNCTIONS ---

void startSession() {
  sessionActive = true;
  currentSessionId = 0;
  dataCount = 0;
  beatAvg = 0;
  rmssd = 0;
  rrIndex = 0;
  minRed = 200000; maxRed = 0;
  minIR = 200000; maxIR = 0;
  lastDataLog = millis();
  display.clearDisplay();
  Serial.println("📊 Session started");
}

void processSignal(long irValue, long redValue) {
  // Beat Detection
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

       // HRV Calculation
       rrIntervals[rrIndex] = (float)delta;
       rrIndex = (rrIndex + 1) % 10;
       
       float sumSqDiff = 0;
       int validPairs = 0;
       for (int i=0; i<9; i++) {
          if (rrIntervals[i] > 0 && rrIntervals[i+1] > 0) {
             float diff = rrIntervals[i] - rrIntervals[i+1];
             sumSqDiff += (diff * diff);
             validPairs++;
          }
       }
       if(validPairs > 0) rmssd = sqrt(sumSqDiff / validPairs);
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

void sendRealTimeData(int bpm, int spo2_val, float stress) {
  if(WiFi.status() != WL_CONNECTED) {
    Serial.println("⚠️ WiFi not connected");
    return;
  }

  if(bpm == 0 && stress == 0) {
    return; // Skip if data is stale
  }

  http.begin(serverUrl);
  http.addHeader("Content-Type", "application/json");
  
  DynamicJsonDocument doc(256);
  if(currentSessionId > 0) {
    doc["session_id"] = currentSessionId;
  }
  doc["bpm"] = bpm;
  doc["spo2"] = spo2_val;
  doc["stress"] = stress;
  
  String jsonStr;
  serializeJson(doc, jsonStr);
  
  Serial.print("📤 Sending: ");
  Serial.println(jsonStr);
  
  int httpCode = http.POST(jsonStr);
  
  if (httpCode == 201 || httpCode == 200) {
    DynamicJsonDocument response(256);
    deserializeJson(response, http.getString());
    
    if (response.containsKey("session_id") && currentSessionId == 0) {
      currentSessionId = response["session_id"];
      Serial.printf("✅ Session ID: %d\n", currentSessionId);
    }
    
    Serial.printf("✅ Data sent - BPM: %d, SpO2: %d, HRV: %.1f\n", 
                  bpm, spo2_val, stress);
  } else {
    Serial.printf("❌ HTTP Error: %d\n", httpCode);
  }
  
  http.end();
}

void endSessionAndUpload() {
  sessionActive = false;
  inGracePeriod = false;

  Serial.println("🛑 Session ended - Uploading...");
  
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
        DynamicJsonDocument response(512);
        deserializeJson(response, http.getString());
        
        Serial.printf("✅ Session ended - Avg BPM: %d, Avg SpO2: %d, Avg HRV: %.1f\n",
                      (int)response["avg_bpm"],
                      (int)response["avg_spo2"],
                      (float)response["avg_stress"]);
        
        display.clearDisplay();
        display.setCursor(0, 20);
        display.setTextSize(2);
        display.println("Saved!");
        display.display();
      } else {
        Serial.printf("❌ Error: HTTP %d\n", code);
        display.clearDisplay();
        display.setCursor(0, 20);
        display.println("Error");
        display.display();
      }
      
      http.end();
  }
  
  delay(2000);
  currentSessionId = 0;
  dataCount = 0;
}

void updateDisplay() {
  display.clearDisplay();
  
  // 1. Timer
  display.setTextSize(1);
  display.setCursor(0,0);
  if (inGracePeriod) {
     int remaining = (GRACE_PERIOD_MS - (millis() - graceTimerStart)) / 100;
     display.print("Resume? "); 
     display.print(remaining);
  } else {
     display.print("Time: "); display.print(dataCount); display.print("s");
  }

  // 2. Heart Icon (Blinks for 150ms after a beat)
  bool beatFlash = (millis() - lastBeatDetectedTime < 150);
  if (beatFlash) {
    display.fillCircle(118, 5, 4, WHITE);
    display.fillCircle(124, 5, 4, WHITE);
    display.fillTriangle(114, 5, 128, 5, 121, 14, WHITE);
  } else {
    display.drawCircle(118, 5, 4, WHITE);
    display.drawCircle(124, 5, 4, WHITE);
    display.drawLine(114, 5, 121, 14, WHITE);
    display.drawLine(128, 5, 121, 14, WHITE);
  }

  // 3. Hero BPM
  display.setCursor(35, 18);
  display.setTextSize(3); 
  display.print(beatAvg > 0 ? String(beatAvg) : "--");
  
  display.setTextSize(1);
  display.setCursor(95, 35);
  display.print("BPM");

  // 4. Footer
  display.drawLine(0, 48, 128, 48, WHITE);
  display.setCursor(0, 54);
  display.print("SpO2: "); display.print(estimatedSpO2); display.print("%");
  
  // 5. Stress Category
  display.setCursor(70, 54);
  String stressStr = "WAIT";
  if (rmssd > 0) {
     if (rmssd < STRESS_HIGH_THRESH) stressStr = "HIGH";
     else if (rmssd < STRESS_MED_THRESH) stressStr = "MED";
     else stressStr = "OK";
  }
  display.print("Str: "); display.print(stressStr);
  
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