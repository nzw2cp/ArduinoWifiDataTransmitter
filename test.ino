#include <SPI.h>
#include <WiFiNINA.h>
#include <Arduino_LSM6DS3.h>


const char* WIFI_SSID     = "<wifi_name>";
const char* WIFI_PASSWORD = "<password>";
const char* SERVER_IP     = "172.20.13.6";
const uint16_t SERVER_PORT = 8765;

const char* DEVICE_NAME = "Device1";

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

unsigned long startMillis = 0; // new: record sketch start time

// New tuning / debug flags
constexpr bool DEBUG = false;          // set true to enable occasional Serial logs
constexpr int CHUNK_SIZE = 100;        // samples per packet (keep same as before)

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

void setup() {
  pinMode(STATUS_LED, OUTPUT);
  digitalWrite(STATUS_LED, LOW);
  Serial.begin(115200);
  randomSeed(analogRead(0));
  startMillis = millis(); // start time reference

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
}

void loop() {
  // sample IMU when data is available
  if (IMU.accelerationAvailable()) {
    IMU.readAcceleration(x, y, z);
    float t = (millis() - startMillis) / 1000.0f; // seconds since start

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

    // write formatted line into buffer
    int wrote = snprintf(payloadBuf + payloadPos, rem + 1, "%.3f,%.2f,%.2f,%.2f\n", t, x, y, z);
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
      int wrote2 = snprintf(payloadBuf + payloadPos, rem2 + 1, "%.3f,%.2f,%.2f,%.2f\n", t, x, y, z);
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