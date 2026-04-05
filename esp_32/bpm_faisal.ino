#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "MAX30105.h"
#include "heartRate.h" 
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// --- WiFi Settings ---
const char* ssid = "faisal";
const char* password = "faisal@123";
const char* serverUrl = "https://bpm.eruditech.com/api/record";

// --- Hardware ---
#define MOTOR_PIN 4
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64

// --- Logic Constants ---
#define MAX_SAMPLES 120       // 2 Minutes
#define GRACE_PERIOD_MS 2000  
#define STRESS_HIGH_THRESH 25 // Below 25 = High Stress
#define STRESS_MED_THRESH 40  // Below 40 = Medium, Above 40 = OK

// --- Objects ---
MAX30105 particleSensor;
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// --- Global Variables ---
struct Record {
  int bpm;
  int spo2;
  float stress;
};
Record sessionData[MAX_SAMPLES];
int dataCount = 0;

// State Machine
bool fingerPhysical = false;
bool sessionActive = false;
unsigned long graceTimerStart = 0;
bool inGracePeriod = false;

// Timers for UI
unsigned long lastUiUpdate = 0;
unsigned long lastDataLog = 0;
unsigned long lastBeatTime = 0;
unsigned long lastBeatDetectedTime = 0; // For blinking icon

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
  pinMode(MOTOR_PIN, OUTPUT);
  digitalWrite(MOTOR_PIN, LOW);

  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) for(;;);
  display.setTextColor(WHITE);
  
  // Wifi & Sensor Init
  showStatus("Connecting WiFi...");
  WiFi.begin(ssid, password);
  while(WiFi.status() != WL_CONNECTED) delay(500);

  if (!particleSensor.begin(Wire, I2C_SPEED_FAST)) {
    showStatus("Sensor Missing");
    while (1);
  }

  particleSensor.setup(); 
  particleSensor.setPulseAmplitudeRed(0x0A);
  particleSensor.setPulseAmplitudeGreen(0); 
  
  showIdleScreen();
}

void loop() {
  // 1. READ SENSOR
  long irValue = particleSensor.getIR();
  long redValue = particleSensor.getRed();

  // 2. FINGER CHECK
  if (irValue > 50000) {
    if (!fingerPhysical) {
       // Finger just touched!
       fingerPhysical = true;
       inGracePeriod = false;
       if (!sessionActive) startSession(); 
    }
  } else {
    if (fingerPhysical) {
       // Finger just removed
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
        // High Stress (RMSSD < 25) triggers vibration
        if (rmssd > 0 && rmssd < STRESS_HIGH_THRESH) digitalWrite(MOTOR_PIN, HIGH);
        else digitalWrite(MOTOR_PIN, LOW);

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

     // 4. TIME-DRIVEN UPDATES (The Fix for Lag)
     unsigned long currentMillis = millis();

     // A. UI Update (Every 250ms - Smooth Timer)
     if (currentMillis - lastUiUpdate > 250) {
        updateDisplay(); 
        lastUiUpdate = currentMillis;
     }

     // B. Data Logging (Every 1000ms - Steady Graph)
     if (fingerPhysical && currentMillis - lastDataLog > 1000) {
        if (dataCount < MAX_SAMPLES) {
           // TIMEOUT CHECK: Has it been > 2.5 seconds since the last real beat?
           // If yes, we consider the current 'beatAvg' to be stale/old.
           bool isFresh = (millis() - lastBeatDetectedTime < 2500);

           // If fresh, save the value. If stale, save 0.
           sessionData[dataCount].bpm = isFresh ? beatAvg : 0;
           
           // Keep SpO2 as is (it doesn't fluctuate as fast)
           sessionData[dataCount].spo2 = estimatedSpO2;
           
           // If BPM is stale, Stress is definitely stale/invalid
           sessionData[dataCount].stress = isFresh ? rmssd : 0;
           
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
  dataCount = 0;
  beatAvg = 0;
  rmssd = 0;
  rrIndex = 0;
  minRed = 200000; maxRed = 0;
  minIR = 200000; maxIR = 0;
  lastDataLog = millis(); // Reset timer
  display.clearDisplay(); // clear old screens
}

void processSignal(long irValue, long redValue) {
  // Beat Detection
  if (checkForBeat(irValue) == true) {
    long delta = millis() - lastBeatTime;
    lastBeatTime = millis();
    lastBeatDetectedTime = millis(); // Record beat time for the icon

    if (delta > 250 && delta < 2000) {
       beatsPerMinute = 60 / (delta / 1000.0);
       
       if (beatsPerMinute < 255 && beatsPerMinute > 20) {
         rates[rateSpot++] = (byte)beatsPerMinute; 
         rateSpot %= RATE_SIZE;
         beatAvg = 0;
         for (byte x = 0 ; x < RATE_SIZE ; x++) beatAvg += rates[x];
         beatAvg /= RATE_SIZE;
       }

       // HRV
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

  // SpO2 Tracker
  if (redValue < minRed) minRed = redValue;
  if (redValue > maxRed) maxRed = redValue;
  if (irValue < minIR) minIR = irValue;
  if (irValue > maxIR) maxIR = irValue;

  static int spo2Counter = 0;
  spo2Counter++;
  if (spo2Counter > 500) { // Faster update
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

void updateDisplay() {
  display.clearDisplay();
  
  // 1. Timer
  display.setTextSize(1);
  display.setCursor(0,0);
  if (inGracePeriod) {
     display.print("Resume? "); 
     display.print((GRACE_PERIOD_MS - (millis() - graceTimerStart))/100);
  } else {
     display.print("Time: "); display.print(dataCount); display.print("s");
  }

  // 2. Heart Icon (Blinks for 100ms after a beat)
  bool beatFlash = (millis() - lastBeatDetectedTime < 150);
  if (beatFlash) {
    // Filled Heart
    display.fillCircle(118, 5, 4, WHITE);
    display.fillCircle(124, 5, 4, WHITE);
    display.fillTriangle(114, 5, 128, 5, 121, 14, WHITE);
  } else {
    // Empty Heart (Outline)
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

void endSessionAndUpload() {
  sessionActive = false;
  inGracePeriod = false;

  display.clearDisplay();
  display.setCursor(0, 20);
  display.setTextSize(2);
  display.println("Uploading...");
  display.display();

  if (WiFi.status() == WL_CONNECTED && dataCount > 5) {
      HTTPClient http;
      http.begin(serverUrl);
      http.addHeader("Content-Type", "application/json");

      // Use a smaller document if memory is tight, but 16k is usually fine for 2 mins
      DynamicJsonDocument doc(16384);
      JsonArray readings = doc.createNestedArray("readings");
      
      long sumBPM = 0;
      long sumSpO2 = 0;
      float sumStress = 0;
      int validCount = 0;
      
      for(int i=0; i<dataCount; i++) {
         if(sessionData[i].bpm > 0) { // Basic filter
           JsonObject r = readings.createNestedObject();
           r["o"] = i;
           r["b"] = sessionData[i].bpm;
           r["s"] = sessionData[i].spo2;
           r["h"] = sessionData[i].stress;
           
           sumBPM += sessionData[i].bpm;
           sumSpO2 += sessionData[i].spo2;
           sumStress += sessionData[i].stress;
           validCount++;
         }
      }
      
      if(validCount > 0) {
        doc["avg_bpm"] = sumBPM / validCount;
        doc["avg_spo2"] = sumSpO2 / validCount;
        doc["avg_stress"] = sumStress / validCount;
        
        String jsonStr;
        serializeJson(doc, jsonStr);
        int code = http.POST(jsonStr);
        
        display.clearDisplay();
        display.setCursor(0, 20);
        if(code == 200 || code == 201) display.println("Saved!");
        else display.println("Error");
        display.display();
      }
      delay(2000);
  }
  dataCount = 0;
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