#include <SPI.h>
#include <WiFiNINA.h>
#include <Arduino_LSM6DS3.h>


const char* WIFI_SSID     = "connection=Terminated";
const char* WIFI_PASSWORD = "alllowercasenospaces";
const char* SERVER_IP     = "192.168.1.100";
const uint16_t SERVER_PORT = 8765;

const char* DEVICE_NAME = "Device1";

float x, y, z;
float degreesX = 0;
float degreesY = 0;

constexpr uint8_t STATUS_LED = LED_BUILTIN;
WiFiClient client;

void indicateConnecting() {
  digitalWrite(STATUS_LED, HIGH);
  delay(150);
  digitalWrite(STATUS_LED, LOW);
  delay(150);
}

void indicateConnected() {
  digitalWrite(STATUS_LED, HIGH);
}

void indicateSendPulse() {
  digitalWrite(STATUS_LED, LOW);
  delay(40);
  digitalWrite(STATUS_LED, HIGH);
}

bool ensureWiFiConnected() {
  if (WiFi.status() == WL_CONNECTED) return true;
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000UL) {
    indicateConnecting();
  }
  return WiFi.status() == WL_CONNECTED;
}

bool performWebSocketHandshake() {
  if (!client.connected()) return false;

  const char* wsKey = "dGhlIHNhbXBsZSBub25jZQ=="; // 16-byte base64 token
  client.print(
    "GET / HTTP/1.1\r\n"
    "Host: "); client.print(SERVER_IP); client.print(":"); client.print(SERVER_PORT); client.print("\r\n"
    "Upgrade: websocket\r\n"
    "Connection: Upgrade\r\n"
    "Sec-WebSocket-Version: 13\r\n"
    "Sec-WebSocket-Key: "); client.print(wsKey); client.print("\r\n\r\n");

  unsigned long start = millis();
  while (!client.available() && millis() - start < 5000UL) {
    delay(10);
  }
  if (!client.available()) return false;

  String statusLine = client.readStringUntil('\n');
  statusLine.trim();
  if (!statusLine.startsWith("HTTP/1.1 101")) return false;

  while (client.connected()) {
    String header = client.readStringUntil('\n');
    header.trim();
    if (header.length() == 0) break;
  }
  return true;
}

bool ensureWebSocketConnected() {
  if (client.connected()) return true;
  if (!ensureWiFiConnected()) return false;

  if (!client.connect(SERVER_IP, SERVER_PORT)) return false;
  if (!performWebSocketHandshake()) {
    client.stop();
    return false;
  }
  return true;
}

bool sendWebSocketText(const String& data) {
  if (!client.connected()) return false;

  const uint8_t opcode = 0x1; // text frame
  size_t payloadLen = data.length();
  uint8_t header[10];
  size_t headerLen = 0;

  header[headerLen++] = 0x80 | opcode; // FIN + opcode

  if (payloadLen <= 125) {
    header[headerLen++] = 0x80 | payloadLen; // mask bit + length
  } else if (payloadLen <= 65535) {
    header[headerLen++] = 0x80 | 126;
    header[headerLen++] = (payloadLen >> 8) & 0xFF;
    header[headerLen++] = payloadLen & 0xFF;
  } else {
    header[headerLen++] = 0x80 | 127;
    for (int i = 7; i >= 0; --i) {
      header[headerLen++] = (payloadLen >> (8 * i)) & 0xFF;
    }
  }

  uint8_t mask[4];
  for (int i = 0; i < 4; ++i) mask[i] = random(0, 256);
  for (int i = 0; i < 4; ++i) header[headerLen++] = mask[i];

  if (client.write(header, headerLen) != headerLen) return false;

  for (size_t i = 0; i < payloadLen; ++i) {
    uint8_t masked = data[i] ^ mask[i % 4];
    if (client.write(masked) != 1) return false;
  }
  return true;
}

bool sendWebSocketClose(uint16_t code = 1000) {
  if (!client.connected()) return false;
  uint8_t header[8];
  size_t headerLen = 0;
  header[headerLen++] = 0x88;
  header[headerLen++] = 0x80 | 0x02;
  uint8_t mask[4];
  for (int i = 0; i < 4; ++i) {
    mask[i] = random(0, 256);
    header[headerLen++] = mask[i];
  }
  uint8_t payload[2] = {uint8_t(code >> 8), uint8_t(code & 0xFF)};
  uint8_t masked[2];
  for (int i = 0; i < 2; ++i) masked[i] = payload[i] ^ mask[i];
  if (client.write(header, headerLen) != headerLen) return false;
  return client.write(masked, 2) == 2;
}

void closeWebSocket(uint16_t code = 1000) {
  if (client.connected()) {
    sendWebSocketClose(code);
    delay(20);
  }
  client.stop();
}

void setup() {
  pinMode(STATUS_LED, OUTPUT);
  digitalWrite(STATUS_LED, LOW);
  Serial.begin(115200);
  randomSeed(analogRead(0));

  if (!IMU.begin()) {
    Serial.println("Failed to initialize IMU!");
    while (1);
  }

  while (!ensureWiFiConnected()) {
    indicateConnecting();
  }
  indicateConnected();

  if (!ensureWebSocketConnected()) {
    unsigned long retryStart = millis();
    while (!ensureWebSocketConnected() && millis() - retryStart < 30000UL) {
      indicateConnecting();
    }
    if (!client.connected()) {
      delay(2000);
      return;
    }
    indicateConnected();
  }
}

void loop() {
  // send values 0..1000 step 1, in chunks of 100 per packet
  const int CHUNK_SIZE = 100;
  float buffer[CHUNK_SIZE][3];
  int bufCount = 0;

  while (bufCount < CHUNK_SIZE) {
    if (IMU.accelerationAvailable()) {
      IMU.readAcceleration(x, y, z);
    }

    // if (x > 0.1) {
    //   x = 100 * x;
    //   degreesX = map(x, 0, 97, 0, 90);
    //   Serial.print("Tilting up ");
    //   Serial.print(degreesX);
    //   Serial.println("  degrees");
    // }

    // if (x < -0.1) {
    //   x = 100 * x;
    //   degreesX = map(x, 0, -100, 0, 90);
    //   Serial.print("Tilting down ");
    //   Serial.print(degreesX);
    //   Serial.println("  degrees");
    // }

    // if (y > 0.1) {
    //   y = 100 * y;
    //   degreesY = map(y, 0, 97, 0, 90);
    //   Serial.print("Tilting left ");
    //   Serial.print(degreesY);
    //   Serial.println("  degrees");
    // }

    // if (y < -0.1) {
    //   y = 100 * y;
    //   degreesY = map(y, 0, -100, 0, 90);
    //   Serial.print("Tilting right ");
    //   Serial.print(degreesY);
    //   Serial.println("  degrees");
    // }

    buffer[bufCount][0] = x;
    buffer[bufCount][1] = y;
    buffer[bufCount][2] = z;
    bufCount++;
  }

  // build CSV payload
  String payload = DEVICE_NAME;
  payload += '\n';
  for (int i = 0; i < bufCount; ++i) {
    payload += String(buffer[i][0], 2);
    payload += ',';
    payload += String(buffer[i][1], 2);
    payload += ',';
    payload += String(buffer[i][2], 2);
    payload += '\n';
  }
  payload += "\n"; // newline terminated

  // send, with reconnect check
  if (!client.connected()) {
    closeWebSocket();
    if (!ensureWebSocketConnected()) {
      bufCount = 0;
      return;
    }
  }

  if (!sendWebSocketText(payload)) {
    closeWebSocket();
    return;
  }

  indicateSendPulse();
  bufCount = 0;

  unsigned long waitStart = millis();
  while (millis() - waitStart < 500) {
    if (client.available()) {
      (void)client.read(); // consume inbound bytes (ACK frames)
    } else {
      break;
    }
  }

  bufCount = 0;

  // finished one full run, pause before repeating
  // delay(5000);
}