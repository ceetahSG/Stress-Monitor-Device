
#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "MAX30105.h"
#include "heartRate.h"
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

// --- WiFi Settings ---
const char *ssid = "Zenetic Esports";
const char *password = "oneStudio";
const char *serverUrl = "http://192.168.0.178:5000/api/real-time-data";
const char *endSessionUrl = "http://192.168.0.178:5000/api/end-session";

// --- Hardware ---
#define MOTOR_PIN 4
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64

// --- Logic Constants ---
#define GRACE_PERIOD_MS 2000     // Automatically end session 2 seconds after finger removal
#define STRESS_HIGH_SCORE_THRESH 70  // 70-100 = High Stress
#define STRESS_MED_SCORE_THRESH 40   // 40-69 = Medium Stress; below 40 = Low Stress
#define BEAT_STALE_MS 2500     // If no real beat in this long, treat the current BPM as stale

// --- Objects ---
MAX30105 particleSensor;
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// --- Global Variables ---
// Replaces the old sessionData[]/dataCount local buffer. The server now
// assigns a session_id on the first real-time POST, and every reading is
// sent to it live instead of being held on-device until the end.
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
unsigned long lastBeatDetectedTime = 0;  // For blinking icon
unsigned long lastWiFiCheck = 0;
unsigned long sessionStartTime = 0;  // Used for the on-screen timer now that dataCount is gone

// Network uploads run on the ESP32 WiFi/core task instead of inside the
// sensor-reading loop. This prevents HTTP/TCP operations from blocking
// MAX30105 sampling and corrupting RR intervals/RMSSD.
TaskHandle_t uploadTaskHandle = nullptr;
SemaphoreHandle_t uploadMutex = nullptr;
volatile bool uploadPending = false;
int pendingBpm = 0;
int pendingSpo2 = 0;
float pendingStress = 0.0f;

// Math Variables (identical math to the base code)
const byte RATE_SIZE = 4;
byte rates[RATE_SIZE];
byte rateSpot = 0;
byte samplesCollected = 0;  // FIX: only average over slots that hold a real reading (see note at bottom)
float beatsPerMinute;
int beatAvg = 0;

// HRV
// NOTE: previous version stored raw deltas in a 10-slot circular buffer
// (rrIntervals[]) and diffed by array position. That silently breaks once
// the buffer wraps every 10 beats: slot i and slot i+1 stop being
// temporally adjacent beats (one holds a brand-new delta, its neighbor
// still holds one from up to 9 beats earlier), which inflates RMSSD
// indefinitely after the first ~10 beats of a session. This version keeps
// only "last beat's delta" plus a small rolling window of squared
// consecutive diffs, so every value that ever enters the average really is
// beat[n] vs beat[n-1].
const int RMSSD_WINDOW = 8;
float prevRR = 0;
float sqDiffBuffer[RMSSD_WINDOW];
int sqDiffIndex = 0;
int sqDiffCount = 0;
const int RMSSD_MIN_DIFFS = 3;  // don't trust/report a score on 1-2 diffs alone
float rmssd = 0;
float avgRR = 0;  // running-average RR interval, used to catch outlier beats
int consecutiveOutliers = 0;
const int MAX_CONSECUTIVE_OUTLIERS = 3;  // after this many rejections in a
                                          // row, trust the new rate instead
                                          // of staying stuck on a stale one

// SpO2 Estimator
double minRed = 200000, maxRed = 0;
double minIR = 200000, maxIR = 0;
int estimatedSpO2 = 98;

void connectToWiFi();
void queueRealTimeData(int bpm, int spo2_val, float stress_val);
void realTimeUploadTask(void *parameter);
void endSessionAndUpload();
void updateDisplay();
void startSession();
void processSignal(long irValue, long redValue);
float stressScoreFromRMSSD(float rmssdVal);
void showStatus(const char *msg);
void showIdleScreen();


void setup() {
  Serial.begin(115200);
  pinMode(MOTOR_PIN, OUTPUT);
  digitalWrite(MOTOR_PIN, LOW);

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C))
    for (;;)
      ;
  display.setTextColor(WHITE);

  // Wifi & Sensor Init
  showStatus("Connecting WiFi...");
  connectToWiFi();

  if (!particleSensor.begin(Wire, I2C_SPEED_FAST)) {
    showStatus("Sensor Missing");
    while (1)
      ;
  }

  particleSensor.setup();
  particleSensor.setPulseAmplitudeRed(0x0A);
  particleSensor.setPulseAmplitudeGreen(0);

  uploadMutex = xSemaphoreCreateMutex();
  if (uploadMutex != nullptr) {
    xTaskCreatePinnedToCore(
      realTimeUploadTask,
      "RTUpload",
      8192,
      nullptr,
      1,
      &uploadTaskHandle,
      0);
  }

  showIdleScreen();
}

void connectToWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(500);
    attempts++;
  }
}

void loop() {
  // Real-time streaming needs a live connection, so reconnect quietly
  // in the background if WiFi ever drops mid-session.
  if (millis() - lastWiFiCheck > 5000) {
    if (WiFi.status() != WL_CONNECTED) {
      connectToWiFi();
    }
    lastWiFiCheck = millis();
  }

  // 1. READ SENSOR
  long irValue = particleSensor.getIR();
  long redValue = particleSensor.getRed();

  // 2. FINGER CHECK
  if (irValue > 50000) {
    if (!fingerPhysical) {
      // Finger just touched!
      fingerPhysical = true;
      inGracePeriod = false;
      if (!sessionActive)
        startSession();
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

      // Vibration Logic: use the same 0-100 stress score shown on the dashboard.
      // 70-100 = High Stress.
      float currentStressScore = stressScoreFromRMSSD(rmssd);
      if (currentStressScore >= STRESS_HIGH_SCORE_THRESH)
        digitalWrite(MOTOR_PIN, HIGH);
      else
        digitalWrite(MOTOR_PIN, LOW);
    } else {
      // Finger Missing - Handle Grace Period
      digitalWrite(MOTOR_PIN, LOW);

      if (!inGracePeriod) {
        inGracePeriod = true;
        graceTimerStart = millis();
      } else {
        if (millis() - graceTimerStart >= GRACE_PERIOD_MS) {
          endSessionAndUpload();
        }
      }
    }

    // 4. TIME-DRIVEN UPDATES
    unsigned long currentMillis = millis();

    // A. UI Update (Every 250ms - Smooth Timer)
    if (currentMillis - lastUiUpdate > 250) {
      updateDisplay();
      lastUiUpdate = currentMillis;
    }

    // B. QUEUE LIVE UPDATE (Every 1000ms)
    // IMPORTANT: do not perform HTTP here. HTTP/TCP operations can block
    // this loop long enough to make MAX30105 beat sampling miss beats.
    // We only copy the latest values; a separate FreeRTOS task performs
    // the network upload.
    if (fingerPhysical && currentMillis - lastDataLog > 1000) {
      bool isFresh = (millis() - lastBeatDetectedTime < BEAT_STALE_MS);

      queueRealTimeData(
        isFresh ? beatAvg : 0,
        estimatedSpO2,
        isFresh ? stressScoreFromRMSSD(rmssd) : 0);

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
  currentSessionId = 0;  // Tells the server "start a new session" on the next POST
  if (uploadMutex != nullptr &&
      xSemaphoreTake(uploadMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
    uploadPending = false;
    pendingBpm = 0;
    pendingSpo2 = 0;
    pendingStress = 0.0f;
    xSemaphoreGive(uploadMutex);
  }
  beatAvg = 0;
  rmssd = 0;
  prevRR = 0;
  avgRR = 0;
  consecutiveOutliers = 0;
  sqDiffIndex = 0;
  sqDiffCount = 0;

  // FIX: clear the BPM/HRV buffers so leftover values from a previous
  // session (or the zero-initialized boot state) can't leak into the
  // very first readings of this new session.
  rateSpot = 0;
  samplesCollected = 0;
  for (byte x = 0; x < RATE_SIZE; x++)
    rates[x] = 0;
  for (int i = 0; i < RMSSD_WINDOW; i++)
    sqDiffBuffer[i] = 0;

  minRed = 200000;
  maxRed = 0;
  minIR = 200000;
  maxIR = 0;
  lastDataLog = millis();
  sessionStartTime = millis();
  display.clearDisplay();  // clear old screens
}

void processSignal(long irValue, long redValue) {
  // Beat Detection
  if (checkForBeat(irValue) == true) {
    long delta = millis() - lastBeatTime;
    lastBeatTime = millis();
    lastBeatDetectedTime = millis();  // Record beat time for the icon

    if (delta > 250 && delta < 2000) {
      Serial.print("delta=");
      Serial.println(delta);
      beatsPerMinute = 60 / (delta / 1000.0);

      if (beatsPerMinute < 255 && beatsPerMinute > 20) {
        rates[rateSpot++] = (byte)beatsPerMinute;
        rateSpot %= RATE_SIZE;

        // FIX: only average over slots that have actually been filled
        // this session, instead of always dividing by RATE_SIZE.
        if (samplesCollected < RATE_SIZE)
          samplesCollected++;

        beatAvg = 0;
        for (byte x = 0; x < samplesCollected; x++)
          beatAvg += rates[x];
        beatAvg /= samplesCollected;
      }

      // HRV — compare this beat's delta only to the immediately previous
      // beat's delta (true beat[n] vs beat[n-1]), then keep a small rolling
      // window of those squared diffs. No array-position aliasing, so RMSSD
      // can't get permanently inflated once the window fills up.
      //
      // OUTLIER GUARD: a single missed/double-counted beat produces one
      // wildly-off delta. Without this check, that ONE bad beat gets diffed
      // against its neighbor and the resulting huge squared-diff sits in
      // the rolling window poisoning every RMSSD read until it ages out.
      // So: if a new delta is more than 35% off the recent average RR,
      // treat it as noise and skip it.
      //
      // RECOVERY: avgRR only moves on accepted beats, so if the real heart
      // rate genuinely drifts (or the signal gets temporarily worse), every
      // beat after that first rejection keeps comparing against a stale
      // baseline and gets rejected forever - a permanent lockup. To prevent
      // that, count consecutive rejections; after too many in a row, trust
      // that this is the new normal, recalibrate to it, and resume.
      if (prevRR > 0) {
        bool isOutlier = avgRR > 0 && fabs(delta - avgRR) > 0.35 * avgRR;

        if (isOutlier) {
          consecutiveOutliers++;
        }

        if (isOutlier && consecutiveOutliers < MAX_CONSECUTIVE_OUTLIERS) {
          // Reject this beat only - don't diff it, don't move prevRR or
          // avgRR forward yet, in case it really was just one bad beat.
        } else {
          if (isOutlier) {
            // Forced recalibration: too many "outliers" in a row means the
            // baseline itself is stale, not the beats. Accept this beat as
            // the new normal but don't diff it against the old, no-longer-
            // relevant prevRR - that comparison would be meaningless.
            prevRR = delta;
            avgRR = delta;
            consecutiveOutliers = 0;
          } else {
            float diff = delta - prevRR;
            sqDiffBuffer[sqDiffIndex] = diff * diff;
            sqDiffIndex = (sqDiffIndex + 1) % RMSSD_WINDOW;
            if (sqDiffCount < RMSSD_WINDOW)
              sqDiffCount++;

            float sumSqDiff = 0;
            for (int i = 0; i < sqDiffCount; i++)
              sumSqDiff += sqDiffBuffer[i];

            // Require a few diffs before trusting the number - one huge
            // outlier delta shouldn't single-handedly swing RMSSD from 0
            // to 700+.
            if (sqDiffCount >= RMSSD_MIN_DIFFS) {
              rmssd = sqrt(sumSqDiff / sqDiffCount);
            }

            prevRR = delta;
            avgRR = (avgRR == 0) ? delta : (0.8 * avgRR + 0.2 * delta);
            consecutiveOutliers = 0;
          }
        }
      } else {
        prevRR = delta;
        avgRR = delta;
        consecutiveOutliers = 0;
      }

      Serial.print("validDiffs=");
      Serial.print(sqDiffCount);
      Serial.print("  RAW RMSSD=");
      Serial.println(rmssd);
    }
  }

  // SpO2 Tracker
  if (redValue < minRed)
    minRed = redValue;
  if (redValue > maxRed)
    maxRed = redValue;
  if (irValue < minIR)
    minIR = irValue;
  if (irValue > maxIR)
    maxIR = irValue;

  static int spo2Counter = 0;
  spo2Counter++;
  if (spo2Counter > 500) {  // Faster update
    double redAC = maxRed - minRed;
    double irAC = maxIR - minIR;
    if (irAC > 0 && redAC > 0) {
      float R = (redAC / maxRed) / (irAC / maxIR);
      float spo2 = 104 - 17 * R;
      if (spo2 > 100)
        spo2 = 100;
      if (spo2 < 80)
        spo2 = 80;
      estimatedSpO2 = (int)spo2;
    }
    minRed = 200000;
    maxRed = 0;
    minIR = 200000;
    maxIR = 0;
    spo2Counter = 0;
  }
}

// The server's /api/real-time-data (and /api/record) endpoints reject any
// "stress" value outside 0-100. Raw RMSSD is a millisecond value that can
// easily go over 100, which would make the server silently 400-reject
// exactly the calmest, healthiest readings. This converts RMSSD into a
// bounded 0-100 normalized stress score used by both the dashboard and OLED.
// Higher score = more stress.
float stressScoreFromRMSSD(float rmssdVal) {
  if (rmssdVal <= 0)
    return 0;

  // The old mapping used 120 ms as the relaxed endpoint. Your actual
  // MAX30105 readings are commonly 110-190 ms, so almost everything above
  // 120 ms became exactly 0. That made the dashboard look stuck at zero.
  //
  // New display mapping:
  //   RMSSD <= 40 ms  -> stress 100 (high)
  //   RMSSD = 110 ms  -> stress 50 (medium)
  //   RMSSD >= 180 ms -> stress 0 (low)
  //
  // This is a project-specific normalized score, NOT a clinical diagnosis.
  // It is intentionally tuned to the observed RMSSD range of this device.
  const float RMSSD_HIGH_STRESS = 40.0f;
  const float RMSSD_LOW_STRESS = 180.0f;

  float score = 100.0f -
                ((rmssdVal - RMSSD_HIGH_STRESS) /
                 (RMSSD_LOW_STRESS - RMSSD_HIGH_STRESS)) * 100.0f;

  if (score < 0)
    score = 0;
  if (score > 100)
    score = 100;

  return score;
}

void queueRealTimeData(int bpm, int spo2_val, float stress_val) {
  if (uploadMutex == nullptr) return;

  if (xSemaphoreTake(uploadMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    pendingBpm = bpm;
    pendingSpo2 = spo2_val;
    pendingStress = stress_val;
    uploadPending = true;
    xSemaphoreGive(uploadMutex);
  }
}

void realTimeUploadTask(void *parameter) {
  (void)parameter;

  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000));

    if (uploadMutex == nullptr) continue;

    int bpm = 0;
    int spo2_val = 0;
    float stress_val = 0.0f;
    int sessionIdSnapshot = 0;
    bool shouldUpload = false;

    if (xSemaphoreTake(uploadMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
      if (uploadPending) {
        bpm = pendingBpm;
        spo2_val = pendingSpo2;
        stress_val = pendingStress;
        sessionIdSnapshot = currentSessionId;
        uploadPending = false;
        shouldUpload = true;
      }
      xSemaphoreGive(uploadMutex);
    }

    if (!shouldUpload) continue;

    if (WiFi.status() != WL_CONNECTED) {
      connectToWiFi();
      if (WiFi.status() != WL_CONNECTED) continue;
    }

    // Do not send an empty measurement before RMSSD has become valid.
    if (bpm == 0 && stress_val == 0) continue;

    HTTPClient localHttp;
    localHttp.setConnectTimeout(1000);
    localHttp.setTimeout(1000);

    if (!localHttp.begin(serverUrl)) {
      Serial.println("HTTP begin failed");
      continue;
    }

    localHttp.addHeader("Content-Type", "application/json");

    DynamicJsonDocument doc(256);
    if (sessionIdSnapshot > 0) {
      doc["session_id"] = sessionIdSnapshot;
    }
    doc["bpm"] = bpm;
    doc["spo2"] = spo2_val;
    doc["stress"] = stress_val;

    String jsonStr;
    serializeJson(doc, jsonStr);

    Serial.print("RT SEND: ");
    Serial.println(jsonStr);

    int httpCode = localHttp.POST(jsonStr);

    if (httpCode == 200 || httpCode == 201) {
      String response = localHttp.getString();

      DynamicJsonDocument responseDoc(256);
      DeserializationError err = deserializeJson(responseDoc, response);

      if (!err && responseDoc.containsKey("session_id")) {
        int returnedSessionId = responseDoc["session_id"];

        if (xSemaphoreTake(uploadMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
          if (currentSessionId == 0) {
            currentSessionId = returnedSessionId;
            Serial.printf("Session ID: %d\n", currentSessionId);
          }
          xSemaphoreGive(uploadMutex);
        }
      }
    } else {
      Serial.printf("RT HTTP Error: %d\n", httpCode);
    }

    localHttp.end();
  }
}

void endSessionAndUpload() {
  // Stop creating new live readings immediately.
  sessionActive = false;
  inGracePeriod = false;

  display.clearDisplay();
  display.setCursor(0, 20);
  display.setTextSize(2);
  display.println("Saving...");
  display.display();

  int sessionIdToEnd = 0;

  // The upload task can be in the middle of the first POST when the finger
  // is removed. In that case currentSessionId is still 0 for a short time.
  // Wait for that POST to finish before trying to end the session.
  unsigned long waitStart = millis();
  while (millis() - waitStart < 3500) {
    if (uploadMutex != nullptr &&
        xSemaphoreTake(uploadMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
      sessionIdToEnd = currentSessionId;
      uploadPending = false;
      xSemaphoreGive(uploadMutex);
    }

    if (sessionIdToEnd > 0) break;
    delay(50);
  }

  Serial.printf("AUTO END: session ID = %d\n", sessionIdToEnd);

  if (WiFi.status() == WL_CONNECTED && sessionIdToEnd > 0) {
    HTTPClient localHttp;
    localHttp.setConnectTimeout(1000);
    localHttp.setTimeout(1000);

    if (localHttp.begin(endSessionUrl)) {
      localHttp.addHeader("Content-Type", "application/json");

      DynamicJsonDocument doc(128);
      doc["session_id"] = sessionIdToEnd;

      String jsonStr;
      serializeJson(doc, jsonStr);

      int code = localHttp.POST(jsonStr);

      display.clearDisplay();
      display.setCursor(0, 20);
      display.setTextSize(2);
      display.println(code == 200 ? "Saved!" : "Error");
      display.display();

      localHttp.end();
    }
  }

  if (uploadMutex != nullptr &&
      xSemaphoreTake(uploadMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
    currentSessionId = 0;
    xSemaphoreGive(uploadMutex);
  } else {
    currentSessionId = 0;
  }

  delay(2000);
}

void updateDisplay() {
  display.clearDisplay();

  // 1. Timer
  display.setTextSize(1);
  display.setCursor(0, 0);
  if (inGracePeriod) {
    display.print("Resume? ");
    display.print((GRACE_PERIOD_MS - (millis() - graceTimerStart)) / 100);
  } else {
    unsigned long elapsed = (millis() - sessionStartTime) / 1000;
    display.print("Time: ");
    display.print(elapsed);
    display.print("s");
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
  display.print("SpO2: ");
  display.print(estimatedSpO2);
  display.print("%");

  // 5. Stress Category (unchanged - still your original raw-RMSSD thresholds)
  display.setCursor(70, 54);
  String stressStr = "WAIT";
  if (rmssd > 0) {
    float displayStressScore = stressScoreFromRMSSD(rmssd);
    if (displayStressScore >= STRESS_HIGH_SCORE_THRESH)
      stressStr = "HIGH";
    else if (displayStressScore >= STRESS_MED_SCORE_THRESH)
      stressStr = "MED";
    else
      stressStr = "LOW";
  }
  display.print("Str: ");
  display.print(stressStr);

  display.display();
}

void showStatus(const char *msg) {
  display.clearDisplay();
  display.setCursor(0, 0);
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
