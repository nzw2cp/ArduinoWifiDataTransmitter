#include <SPI.h>
#include <WiFiNINA.h>
#include <Arduino_LSM6DS3.h>
#include <Wire.h>

float accelX,            accelY,             accelZ,            // units m/s/s i.e. accelZ if often 9.8 (gravity)
      gyroX,             gyroY,              gyroZ,             // units dps (degrees per second)
      gyroDriftX,        gyroDriftY,         gyroDriftZ,        // units dps
      gyroRoll,          gyroPitch,          gyroYaw,           // units degrees (expect major drift)
      gyroCorrectedRoll, gyroCorrectedPitch, gyroCorrectedYaw,  // units degrees (expect minor drift)
      accRoll,           accPitch,           accYaw,            // units degrees (roll and pitch noisy, yaw not possible)
      complementaryRoll, complementaryPitch, complementaryYaw;  // units degrees (excellent roll, pitch, yaw minor drift)

long lastTime;
long lastInterval;

const char* WIFI_SSID     = "<wifi_name>";
const char* WIFI_PASSWORD = "<password>";
const char* SERVER_IP     = "192.168.1.100";
const uint16_t SERVER_PORT = 8765;

const char* DEVICE_NAME = "Device5";

float x, y, z;
float degreesX = 0;
float degreesY = 0;

constexpr uint8_t STATUS_LED = LED_BUILTIN;
WiFiClient client;

// New non-blocking ACK state + receive buffer
bool ackPending = false;
unsigned long ackExpiry = 0;
char ackBuf[128];
size_t ackBufPos = 0;

// Non-blocking LED blink state for indicateConnecting/send pulse
unsigned long blinkLastMillis = 0;
bool blinkState = false;
unsigned long sendPulseEnd = 0;

unsigned long long startMillis = 0; // epoch ms from server (0 until synced)
unsigned long syncLocalMillis = 0;

// helper: convert unsigned long long to decimal string (returns length, 0 on failure)
size_t ull_to_decstr(unsigned long long v, char* buf, size_t bufsize) {
  if (bufsize == 0) return 0;
  if (v == 0) {
    if (bufsize > 1) { buf[0] = '0'; buf[1] = '\0'; return 1; }
    buf[0] = '\0'; return 0;
  }
  char tmp[32];
  int pos = 0;
  while (v > 0 && pos < (int)(sizeof(tmp) - 1)) {
    tmp[pos++] = '0' + (v % 10);
    v /= 10;
  }
  size_t len = (size_t)pos;
  if (len + 1 > bufsize) return 0; // not enough space
  for (size_t i = 0; i < len; ++i) buf[i] = tmp[len - 1 - i];
  buf[len] = '\0';
  return len;
}

// New tuning / debug flags
constexpr bool DEBUG = false;          // set true to enable occasional Serial logs
constexpr int CHUNK_SIZE = 45;        // samples per packet (keep same as before)

// Replace sample buffer with a payload buffer (approx size = CHUNK_SIZE * avg_line_len + header)
constexpr size_t PAYLOAD_BUF_SIZE = 4096;
static char payloadBuf[PAYLOAD_BUF_SIZE];
static size_t payloadPos = 0;
static int samplesCollected = 0;

void indicateConnecting() {
  // non-blocking toggle every 150ms
  unsigned long now = millis();
  if (now - blinkLastMillis >= 150) {
    blinkLastMillis = now;
    blinkState = !blinkState;
    digitalWrite(STATUS_LED, blinkState ? HIGH : LOW);
  }
}

void indicateConnected() {
  digitalWrite(STATUS_LED, HIGH);
}

void indicateSendPulse() {
  // start a short pulse without blocking
  digitalWrite(STATUS_LED, LOW);
  sendPulseEnd = millis() + 40;
}

bool ensureWiFiConnected() {
  if (WiFi.status() == WL_CONNECTED) return true;
  Serial.println("[arduino] Starting WiFi connection...");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000UL) {
    indicateConnecting();
  }
  bool ok = WiFi.status() == WL_CONNECTED;
  if (ok) {
    Serial.print("[arduino] WiFi connected, IP=");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("[arduino] WiFi connection failed");
  }
  return ok;
}

bool ensureTCPConnected() {
  if (client.connected()) return true;
  if (!ensureWiFiConnected()) return false;
  Serial.print("[arduino] Connecting to server ");
  Serial.print(SERVER_IP);
  Serial.print(":");
  Serial.println(SERVER_PORT);
  if (!client.connect(SERVER_IP, SERVER_PORT)) {
    Serial.println("[arduino] TCP connect failed");
    return false;
  }
  Serial.println("[arduino] TCP connected");

  // Attempt to read initial epoch ms sent by server (short blocking window)
  {
    unsigned long start = millis();
    char linebuf[64];
    size_t pos = 0;
    const unsigned long timeoutMs = 2000UL;
    while (millis() - start < timeoutMs) {
      while (client && client.available()) {
        int c = client.read();
        if (c < 0) break;
        if (c == '\r') continue;
        if (c == '\n') {
          linebuf[pos] = '\0';
          if (pos > 0) {
            // try parse numeric epoch ms
            unsigned long long val = (unsigned long long)atoll(linebuf);
            if (val > 1000) { // sanity check
              startMillis = val;
              syncLocalMillis = millis();
              if (DEBUG) {
                Serial.print("[arduino] Synced startMillis=");
                Serial.println((unsigned long long)startMillis);
              }
              return true;
            }
          }
          pos = 0;
        } else {
          if (pos < sizeof(linebuf) - 1) linebuf[pos++] = (char)c;
        }
      }
      // small yield to let other processing occur
      yield();
    }
    // if we didn't parse a time here, we'll accept it later in non-blocking receive loop
  }

  return true;
}

bool sendTCP(const char* data, size_t len) {
  if (!client.connected()) {
    if (DEBUG) Serial.println("[arduino] sendTCP called but client not connected");
    return false;
  }
  size_t written = client.write((const uint8_t*)data, len);
  if (written != len) {
    if (DEBUG) {
      Serial.print("[arduino] sendTCP wrote ");
      Serial.print(written);
      Serial.print(" of ");
      Serial.print(len);
      Serial.println(" bytes");
    }
    return false;
  }
  // set pending state and return immediately
  ackPending = true;
  ackExpiry = millis() + 2000UL; // give up after 2 seconds
  if (DEBUG) {
    Serial.print("[arduino] Sent payload bytes=");
    Serial.print(len);
    Serial.println(", waiting for ACK");
  }
  return true;
}

// helper: ensure a terminating blank line (single '\n' appended so last data-line + '\n' -> blank line)
bool finalizePayloadBeforeSend() {
  if (payloadPos < PAYLOAD_BUF_SIZE - 1) {
    payloadBuf[payloadPos++] = '\n';
    return true;
  }
  // no space to append terminator
  return false;
}



void calibrateIMU(int delayMillis, int calibrationMillis) {

  int calibrationCount = 0;

  delay(delayMillis); // to avoid shakes after pressing reset button

  float sumX, sumY, sumZ;
  int startTime = millis();
  while (millis() < startTime + calibrationMillis) {
    if (readIMU()) {
      // in an ideal world gyroX/Y/Z == 0, anything higher or lower represents drift
      sumX += gyroX;
      sumY += gyroY;
      sumZ += gyroZ;

      calibrationCount++;
    }
  }

  if (calibrationCount == 0) {
    Serial.println("Failed to calibrate");
  }

  gyroDriftX = sumX / calibrationCount;
  gyroDriftY = sumY / calibrationCount;
  gyroDriftZ = sumZ / calibrationCount;

}

/**
   Read accel and gyro data.
   returns true if value is 'new' and false if IMU is returning old cached data
*/
bool readIMU() {
  if (IMU.accelerationAvailable() && IMU.gyroscopeAvailable() ) {
    IMU.readAcceleration(accelX, accelY, accelZ);
    IMU.readGyroscope(gyroX, gyroY, gyroZ);
    return true;
  }
  return false;
}

void doCalculations() {
  accRoll = atan2(accelY, accelZ) * 180 / M_PI;
  accPitch = atan2(-accelX, sqrt(accelY * accelY + accelZ * accelZ)) * 180 / M_PI;

  float lastFrequency = 1000000 / lastInterval;
  gyroRoll = gyroRoll + (gyroX / lastFrequency);
  gyroPitch = gyroPitch + (gyroY / lastFrequency);
  gyroYaw = gyroYaw + (gyroZ / lastFrequency);

  gyroCorrectedRoll = gyroCorrectedRoll + ((gyroX - gyroDriftX) / lastFrequency);
  gyroCorrectedPitch = gyroCorrectedPitch + ((gyroY - gyroDriftY) / lastFrequency);
  gyroCorrectedYaw = gyroCorrectedYaw + ((gyroZ - gyroDriftZ) / lastFrequency);

  complementaryRoll = complementaryRoll + ((gyroX - gyroDriftX) / lastFrequency);
  complementaryPitch = complementaryPitch + ((gyroY - gyroDriftY) / lastFrequency);
  complementaryYaw = complementaryYaw + ((gyroZ - gyroDriftZ) / lastFrequency);

  complementaryRoll = 0.98 * complementaryRoll + 0.02 * accRoll;
  complementaryPitch = 0.98 * complementaryPitch + 0.02 * accPitch;
}




void setup() {
  pinMode(STATUS_LED, OUTPUT);
  digitalWrite(STATUS_LED, LOW);
  Serial.begin(115200);
  randomSeed(analogRead(0));
  // do NOT set startMillis here; it'll be set by server on first connect.
  // record a local reference time so we don't divide by zero if needed
  syncLocalMillis = millis();

  // initialize payload buffer to empty
  payloadPos = 0;
  samplesCollected = 0;

  if (!IMU.begin()) {
    Serial.println("[arduino] Failed to initialize IMU!");
    while (1);
  }
  Serial.println("[arduino] IMU initialized");

  while (!ensureWiFiConnected()) {
    indicateConnecting();
  }
  indicateConnected();

  unsigned long retryStart = millis();
  while (!ensureTCPConnected() && millis() - retryStart < 30000UL) {
    indicateConnecting();
  }
  if (!client.connected()) {
    Serial.println("[arduino] Could not connect to server after retries");
    delay(2000);
    return;
  }
  indicateConnected();
  Serial.println("[arduino] Setup complete");



  calibrateIMU(250, 250);

  lastTime = micros();
}

void loop() {
  // sample IMU when data is available
  // if (readIMU()) {
  //   long currentTime = micros();
  //   lastInterval = currentTime - lastTime; // expecting this to be ~104Hz +- 4%
  //   lastTime = currentTime;

  //   doCalculations();
  //   printCalculations();

  // }



  if (readIMU()) {
    // IMU.readAcceleration(x, y, z);
    long currentTime = micros();
    lastInterval = currentTime - lastTime; // expecting this to be ~104Hz +- 4%
    lastTime = currentTime;

    doCalculations();

    // compute epoch ms: if startMillis==0 (not yet synced) fall back to local millis()
    unsigned long long nowEpochMs;
    if (startMillis != 0) {
      nowEpochMs = startMillis + (unsigned long long)(millis() - syncLocalMillis);
    } else {
      nowEpochMs = (unsigned long long)millis();
    }

    // use ms since epoch as first CSV field
    // if payload empty, write header (device name + newline)
    if (payloadPos == 0) {
      int hlen = snprintf(payloadBuf, PAYLOAD_BUF_SIZE, "%s\n", DEVICE_NAME);
      if (hlen < 0 || (size_t)hlen >= PAYLOAD_BUF_SIZE) {
        // header failed (shouldn't happen) — reset
        payloadPos = 0;
        samplesCollected = 0;
        if (DEBUG) Serial.println("[arduino] payload header write failed");
        return;
      }
      payloadPos = (size_t)hlen;
    }

    // append one CSV row: time,x,y,z\n
    // keep reasonable formatting to limit length
    int rem = (int)(PAYLOAD_BUF_SIZE - payloadPos - 1);
    if (rem <= 0) {
      // no space left: send current payload now to avoid overflow
      // append blank line terminator if possible
      finalizePayloadBeforeSend();
      if (client.connected()) {
        if (!sendTCP(payloadBuf, payloadPos)) {
          client.stop();
          samplesCollected = 0;
          payloadPos = 0;
          yield();
          return;
        }
      } else {
        // can't send, drop buffer
        samplesCollected = 0;
        payloadPos = 0;
        return;
      }
    }

    // write formatted line into buffer: epoch_ms (converted to string) + floats
    char epochStr[32];
    if (ull_to_decstr(nowEpochMs, epochStr, sizeof(epochStr)) == 0) {
      // conversion failed; fallback to "0"
      epochStr[0] = '0'; epochStr[1] = '\0';
    }
    int wrote = snprintf(payloadBuf + payloadPos, rem + 1, "%s,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f\n", epochStr, accelX, accelY, accelZ, gyroX, gyroY, gyroZ, gyroDriftX, gyroDriftY, gyroDriftZ, complementaryRoll, complementaryPitch, complementaryYaw);

    if (wrote < 0) {
      // formatting error; drop sample
    } else if (wrote > rem) {
      // formatted line doesn't fit: flush current payload and start new one with this sample
      // append blank line terminator if possible
      finalizePayloadBeforeSend();
      // send existing payload
      if (client.connected()) {
        if (!sendTCP(payloadBuf, payloadPos)) {
          client.stop();
          samplesCollected = 0;
          payloadPos = 0;
          yield();
          return;
        }
      } else {
        // can't send, drop buffer
        samplesCollected = 0;
        payloadPos = 0;
        return;
      }
      // start new payload header
      int hlen2 = snprintf(payloadBuf, PAYLOAD_BUF_SIZE, "%s\n", DEVICE_NAME);
      if (hlen2 < 0 || (size_t)hlen2 >= PAYLOAD_BUF_SIZE) {
        payloadPos = 0;
        samplesCollected = 0;
        return;
      }
      payloadPos = (size_t)hlen2;
      // try append again (assume it fits now)
      int rem2 = (int)(PAYLOAD_BUF_SIZE - payloadPos - 1);
      char epochStr2[32];
      if (ull_to_decstr(nowEpochMs, epochStr2, sizeof(epochStr2)) == 0) {
        epochStr2[0] = '0'; epochStr2[1] = '\0';
      }
      int wrote2 = snprintf(payloadBuf + payloadPos, rem2 + 1, "%s,%.2f,%.2f,%.2f\n", epochStr2, x, y, z);
      if (wrote2 > 0 && wrote2 <= rem2) {
        payloadPos += (size_t)wrote2;
        samplesCollected = 1;
      } else {
        // give up on this sample
      }
    } else {
      payloadPos += (size_t)wrote;
      samplesCollected++;
    }
  }

  // handle send-pulse timing (non-blocking)
  if (sendPulseEnd != 0 && millis() > sendPulseEnd) {
    // restore connected LED state after pulse
    indicateConnected();
    sendPulseEnd = 0;
  }

  // Non-blocking read: consume available bytes and build lines into ackBuf (unchanged)
  while (client && client.available()) {
    int c = client.read();
    if (c < 0) break;
    if (ackBufPos < sizeof(ackBuf) - 1) {
      ackBuf[ackBufPos++] = (char)c;
    }
    if (c == '\n') {
      ackBuf[ackBufPos] = '\0';
      String line = String(ackBuf);
      line.trim();
      if (line.length() > 0) {
        if (DEBUG) {
          Serial.print("[arduino] Received line: ");
          Serial.println(line);
        }
      }
      // If we haven't synced startMillis yet, accept a numeric line as epoch ms
      if (startMillis == 0 && line.length() > 0) {
        bool allDigits = true;
        for (size_t i = 0; i < line.length(); ++i) {
          char ch = line.charAt(i);
          if (!(ch >= '0' && ch <= '9')) { allDigits = false; break; }
        }
        if (allDigits) {
          unsigned long long val = (unsigned long long)atoll(line.c_str());
          if (val > 1000) {
            startMillis = val;
            syncLocalMillis = millis();
            if (DEBUG) {
              Serial.print("[arduino] Synced startMillis=");
              Serial.println((unsigned long long)startMillis);
            }
          }
        }
      }
      if (line.length() > 0 && ackPending && line.startsWith("ACK")) {
        ackPending = false;
        if (DEBUG) {
          Serial.print("[arduino] ACK matched, cleared pending: ");
          Serial.println(line);
        }
      }
      ackBufPos = 0;
    }
  }

  // handle ACK timeout without blocking
  if (ackPending && millis() > ackExpiry) {
    if (DEBUG) Serial.println("[arduino] ACK timeout, closing connection");
    client.stop();
    ackPending = false;
    // drop current payload to avoid sending partial on reconnect
    samplesCollected = 0;
    payloadPos = 0;
    yield();
    return;
  }

  // send when chunk full (by sample count)
  if (samplesCollected >= CHUNK_SIZE) {
    if (!client.connected()) {
      if (DEBUG) Serial.println("[arduino] client not connected, attempting reconnect");
      client.stop();
      if (!ensureTCPConnected()) {
        samplesCollected = 0;
        payloadPos = 0;
        if (DEBUG) Serial.println("[arduino] reconnect failed, dropping buffer");
        yield();
        return;
      }
    }

    // finalize with blank line then send payload in a single write
    finalizePayloadBeforeSend();
    if (!sendTCP(payloadBuf, payloadPos)) {
      if (DEBUG) Serial.println("[arduino] sendTCP failed, stopping client");
      client.stop();
      samplesCollected = 0;
      payloadPos = 0;
      yield();
      return;
    }

    if (DEBUG) {
      Serial.print("[arduino] Payload sent bytes=");
      Serial.println(payloadPos);
    }
    indicateSendPulse();
    // reset payload
    samplesCollected = 0;
    payloadPos = 0;
  }

  // allow background processing, no blocking delays here
  yield();
}