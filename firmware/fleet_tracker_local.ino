/*
 * TTGO T-Call V1.4 (ESP32-WROVER + SIM800L) GPS Tracker -> LOCAL FLEET-TRACKER Server
 *
 * This is the LOCAL SERVER version of your original GeoLinker sketch.
 * It posts to your own server running at LOCAL_SERVER_HOST:LOCAL_SERVER_PORT
 * instead of circuitdigest.cloud
 *
 * Features:
 * - Supports multiple devices via DEVICE_ID (change per car)
 * - Plain HTTP on configurable port (SIM800L can't do modern HTTPS)
 * - Compatible with local server's /api/v1/geolinker endpoint (drop-in)
 * - Also supports simple /api/track endpoint
 * - Logs coordinate history per device on server side (SQLite)
 *
 * HOW TO USE:
 * 1. Run backend: cd backend && pip install -r requirements.txt && python app.py
 * 2. Find your PC's local IP: Windows -> ipconfig, Linux/Mac -> ifconfig
 *    Example: 192.168.1.50
 * 3. Set LOCAL_SERVER_HOST below to that IP
 * 4. Flash this sketch to each tracker, changing DEVICE_ID per car:
 *    car_tracker_01, car_tracker_02, car_tracker_03, etc.
 * 5. Open http://localhost:8000 in browser to see live map
 *
 * WIRING (same as before):
 *   GY-GPS6MV2 : VCC->3V3  GND->GND  TXD->GPIO32  RXD->GPIO33
 *   GSM antenna must be attached!
 *
 * POWER: Use 5V/2A supply or LiPo - SIM800L pulls ~2A bursts
 */

#include <TinyGPSPlus.h>
#include <Wire.h>

// ============================== USER CONFIG ==================================
// --- Local Server ---
static const char* LOCAL_SERVER_HOST = "192.168.1.50";  // <-- CHANGE TO YOUR PC IP!
static const int   LOCAL_SERVER_PORT = 8000;            // Backend port
static const char* LOCAL_API_PATH    = "/api/v1/geolinker"; // GeoLinker compatible path
// Alternative simple path: "/api/track" - uncomment if you want simpler JSON
// static const char* LOCAL_API_PATH = "/api/track";

static const char* API_KEY   = "local_fleet_key_123"; // Optional, set same in server env FLEET_API_KEY or leave blank
static const char* DEVICE_ID = "car_tracker_01";      // CHANGE PER CAR: car_tracker_01, car_tracker_02, etc.

// --- GPRS APN ---
//   MTN:     "web.gprs.mtnnigeria.net"   user ""      pass ""
//   Airtel:  "internet.ng.airtel.com"    user ""      pass ""
//   Glo:     "gloflat"                   user "flat"  pass "flat"
//   9mobile: "9mobile"                   user ""      pass ""
static const char* MODEM_APN  = "internet.ng.airtel.com";
static const char* MODEM_USER = "";
static const char* MODEM_PASS = "";

static const uint32_t SEND_INTERVAL_MS = 15000;  // 15 seconds
static const long     TZ_OFFSET_MIN    = 60;     // WAT UTC+1

// ============================== GPS (UART2) ==================================
static const long GPS_BAUD   = 9600;
static const int  GPS_RX_PIN = 32;
static const int  GPS_TX_PIN = 33;
#define PRINT_RAW_NMEA 0

TinyGPSPlus gps;

// ===================== MODEM: onboard SIM800L (UART1) ========================
static const int MODEM_PWRKEY   = 4;
static const int MODEM_RST      = 5;
static const int MODEM_POWER_ON = 23;
static const int MODEM_RX       = 26;
static const int MODEM_TX       = 27;
static const long MODEM_BAUD    = 115200;

static const int I2C_SDA = 21;
static const int I2C_SCL = 22;
#define IP5306_ADDR         0x75
#define IP5306_REG_SYS_CTL0 0x00

#define SerialAT Serial1

static bool netRegistered = false;
static bool gprsUp        = false;
static int  lastHttpCode  = 0;
static uint8_t consecutiveFails = 0;

// Forward declarations
void  printStatus();
void  printDMS(const RawDegrees &raw, char posHemi, char negHemi);
void  pumpGps();
bool  makeTimestamp(char* out, size_t n);
static long daysFromCivil(int y, int m, int d);
static void civilFromDays(long z, int &y, int &m, int &d);
static bool setPowerBoostKeepOn(bool en);
void  modemPowerOn();
void  modemInit();
String sendAT(const String& cmd, uint32_t timeoutMs, const char* expect);
bool  ensureGprsUp();
String collectFor(uint32_t ms);
bool  postToLocalServer(double lat, double lon, const char* ts, float speedKmh, uint32_t sats);

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println();
  Serial.println("=== T-Call V1.4 GPS Tracker -> LOCAL FLEET-TRACKER ===");
  Serial.printf("Server: http://%s:%d%s\n", LOCAL_SERVER_HOST, LOCAL_SERVER_PORT, LOCAL_API_PATH);
  Serial.printf("Device ID: %s\n", DEVICE_ID);

  Serial2.setRxBufferSize(2048);
  Serial2.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);

  modemPowerOn();
  modemInit();

  Serial.print("GPS listening on UART2 @ ");
  Serial.print(GPS_BAUD);
  Serial.println(" baud. Cold start can take 30-90 s.");
}

void loop() {
  pumpGps();

  static unsigned long lastPrint = 0;
  if (millis() - lastPrint >= 1000) {
    lastPrint = millis();
    printStatus();
  }

  static unsigned long lastAttempt = 0;
  bool fixFresh = gps.location.isValid() && gps.location.age() < 5000;

  if (fixFresh && millis() - lastAttempt >= SEND_INTERVAL_MS) {
    lastAttempt = millis();

    char ts[20];
    if (!makeTimestamp(ts, sizeof(ts))) {
      Serial.println("[LOCAL] Fix OK but no date/time yet - skipping");
      return;
    }

    Serial.printf("[LOCAL] Uploading %s  lat=%.6f long=%.6f to %s:%d\n",
                  ts, gps.location.lat(), gps.location.lng(), LOCAL_SERVER_HOST, LOCAL_SERVER_PORT);

    bool ok = false;
    for (uint8_t attempt = 1; attempt <= 3 && !ok; attempt++) {
      if (attempt > 1) {
        Serial.printf("[LOCAL] Retry %u/3...\n", attempt);
        delay(3000);
      }
      ok = postToLocalServer(gps.location.lat(), gps.location.lng(), ts,
                             gps.speed.isValid() ? gps.speed.kmph() : 0.0f,
                             gps.satellites.isValid() ? gps.satellites.value() : 0);
    }

    if (ok) {
      consecutiveFails = 0;
      Serial.println(lastHttpCode == 201 ? "[LOCAL] Stored (201 Created)" : "[LOCAL] Stored");
    } else {
      consecutiveFails++;
      Serial.printf("[LOCAL] Upload FAILED (%u in a row)\n", consecutiveFails);
      if (consecutiveFails >= 5) {
        Serial.println("[GSM] Too many failures, power-cycling modem...");
        consecutiveFails = 0;
        modemPowerOn();
        modemInit();
      }
    }
    Serial.println("----------------------------------------");
  }
}

// ============================================================================
// GPS
// ============================================================================
void pumpGps() {
  while (Serial2.available()) {
    char c = Serial2.read();
#if PRINT_RAW_NMEA
    Serial.write(c);
#endif
    gps.encode(c);
  }
}

void printStatus() {
  static bool warned = false;
  static unsigned long started = millis();
  if (!warned && gps.charsProcessed() == 0 && millis() - started > 15000) {
    warned = true;
    Serial.println("!! No NMEA data received. Check GPS wiring, power and baud.");
    return;
  }

  if (gps.location.isValid()) {
    Serial.print("LAT: ");
    Serial.print(gps.location.lat(), 6);
    Serial.print("  LON: ");
    Serial.print(gps.location.lng(), 6);
    Serial.print("   (");
    printDMS(gps.location.rawLat(), 'N', 'S');
    Serial.print("  ");
    printDMS(gps.location.rawLng(), 'E', 'W');
    Serial.print(")  ");
    if (gps.satellites.isValid() && gps.satellites.value() >= 4)
      Serial.print("Fix: 3D ");
  } else {
    Serial.print("LAT/LON: no fix yet...");
  }

  if (gps.satellites.isValid()) {
    Serial.print("  SAT: ");
    Serial.print(gps.satellites.value());
  }
  if (gps.hdop.isValid()) {
    Serial.print("  HDOP: ");
    Serial.print(gps.hdop.hdop(), 1);
  }
  Serial.println();

  if (gps.altitude.isValid()) {
    Serial.print("ALT: ");
    Serial.print(gps.altitude.meters(), 1);
    Serial.print(" m   SPEED: ");
    Serial.print(gps.speed.isValid() ? gps.speed.kmph() : 0.0, 1);
    Serial.println(" km/h");
  }

  if (gps.date.isValid() && gps.time.isValid()) {
    Serial.printf("UTC: %04u-%02u-%02u  %02u:%02u:%02u\n",
                  (unsigned)gps.date.year(), (unsigned)gps.date.month(), (unsigned)gps.date.day(),
                  (unsigned)gps.time.hour(), (unsigned)gps.time.minute(), (unsigned)gps.time.second());
  }

  Serial.printf("NET: %s   GPRS: %s   Last HTTP: %d   Server: %s:%d\n",
                netRegistered ? "registered" : "searching...",
                gprsUp ? "up" : "down",
                lastHttpCode,
                LOCAL_SERVER_HOST, LOCAL_SERVER_PORT);
  Serial.println("----------------------------------------");
}

void printDMS(const RawDegrees &raw, char posHemi, char negHemi) {
  double fracDeg  = raw.billionths / 1000000000.0;
  double totalMin = fracDeg * 60.0;
  int    min      = (int)totalMin;
  double sec      = (totalMin - min) * 60.0;

  Serial.print(raw.deg, DEC);
  Serial.print(" ");
  Serial.print(min, DEC);
  Serial.print("' ");
  Serial.print(sec, 3);
  Serial.print("\" ");
  Serial.print(raw.negative ? negHemi : posHemi);
}

bool makeTimestamp(char* out, size_t n) {
  if (!gps.date.isValid() || !gps.time.isValid()) return false;

  long days  = daysFromCivil((int)gps.date.year(), (int)gps.date.month(), (int)gps.date.day());
  long epoch = days * 86400L + (long)gps.time.hour() * 3600L
             + (long)gps.time.minute() * 60L + (long)gps.time.second()
             + TZ_OFFSET_MIN * 60L;

  long d   = epoch / 86400L;
  long rem = epoch % 86400L;
  if (rem < 0) { rem += 86400L; d -= 1; }

  int y, m, dd;
  civilFromDays(d, y, m, dd);
  snprintf(out, n, "%04d-%02d-%02d %02d:%02d:%02d",
           y, m, dd, (int)(rem / 3600), (int)((rem % 3600) / 60), (int)(rem % 60));
  return true;
}

static long daysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  long era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097L + (long)doe - 719468;
}
static void civilFromDays(long z, int &y, int &m, int &d) {
  z += 719468;
  long era = (z >= 0 ? z : z - 146096) / 146097;
  unsigned doe = (unsigned)(z - era * 146097);
  unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int yy = (int)yoe + (int)era * 400;
  unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  unsigned mp = (5 * doy + 2) / 153;
  d = (int)(doy - (153 * mp + 2) / 5 + 1);
  m = (int)(mp + (mp < 10 ? 3 : -9));
  y = yy + (m <= 2);
}

// ============================================================================
// SIM800L / GPRS
// ============================================================================
static bool setPowerBoostKeepOn(bool en) {
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.beginTransmission(IP5306_ADDR);
  Wire.write(IP5306_REG_SYS_CTL0);
  Wire.write(en ? 0x37 : 0x35);
  return Wire.endTransmission() == 0;
}

void modemPowerOn() {
  Serial.println(setPowerBoostKeepOn(true)
                 ? "[PWR] IP5306 boost keep-on enabled"
                 : "[PWR] IP5306 not found - continuing anyway");

  pinMode(MODEM_PWRKEY,   OUTPUT);
  pinMode(MODEM_RST,      OUTPUT);
  pinMode(MODEM_POWER_ON, OUTPUT);
  digitalWrite(MODEM_PWRKEY,   LOW);
  digitalWrite(MODEM_RST,      HIGH);
  digitalWrite(MODEM_POWER_ON, HIGH);

  SerialAT.setRxBufferSize(1024);
  SerialAT.begin(MODEM_BAUD, SERIAL_8N1, MODEM_RX, MODEM_TX);
  delay(3000);

  bool ok = false;
  for (uint8_t round = 0; round < 2 && !ok; round++) {
    for (uint8_t i = 0; i < 10 && !ok; i++) {
      ok = sendAT("AT", 1000, "OK").indexOf("OK") != -1;
      delay(300);
    }
    if (!ok) {
      Serial.println("[GSM] No answer - pulsing PWRKEY");
      digitalWrite(MODEM_PWRKEY, HIGH);
      delay(1200);
      digitalWrite(MODEM_PWRKEY, LOW);
      delay(5000);
    }
  }
  Serial.println(ok ? "[GSM] SIM800L answering AT" : "[GSM] !! modem not answering");
}

void modemInit() {
  sendAT("ATE0", 1000, "OK");

  String cpin = sendAT("AT+CPIN?", 3000, "CPIN");
  if (cpin.indexOf("READY") == -1) {
    Serial.println("[GSM] !! SIM problem: " + cpin);
    if (cpin.indexOf("SIM PIN") != -1)
      Serial.println("[GSM] !! Remove SIM PIN using a phone, then retry.");
  }

  netRegistered = false;
  Serial.print("[GSM] Registering on network");
  unsigned long t0 = millis();
  while (millis() - t0 < 90000) {
    String r = sendAT("AT+CREG?", 2000, "+CREG:");
    int comma = r.indexOf(',');
    int stat  = (comma != -1) ? r.substring(comma + 1, comma + 2).toInt() : -1;
    if (stat == 1 || stat == 5) { netRegistered = true; break; }
    Serial.print('.');
    delay(1500);
  }
  Serial.println(netRegistered ? "\n[GSM] Registered" : "\n[GSM] !! No network");

  Serial.println("[GSM] Signal: " + sendAT("AT+CSQ", 1000, "+CSQ"));
  sendAT("AT+CGATT=1", 10000, "OK");
  gprsUp = false;
}

String sendAT(const String& cmd, uint32_t timeoutMs, const char* expect) {
  while (SerialAT.available()) SerialAT.read();
  if (cmd.length()) SerialAT.println(cmd);

  String resp;
  unsigned long t0 = millis();
  while (millis() - t0 < timeoutMs) {
    pumpGps();
    while (SerialAT.available()) {
      resp += (char)SerialAT.read();
      if (resp.length() > 384) resp = resp.substring(resp.length() - 256);
    }
    if (expect && resp.indexOf(expect) != -1) break;
    delay(5);
  }
  return resp;
}

bool ensureGprsUp() {
  if (gprsUp && sendAT("AT+CIFSR", 2000, ".").indexOf('.') != -1) return true;

  sendAT("AT+CIPSHUT", 5000, "SHUT OK");
  String cstt = "AT+CSTT=\"" + String(MODEM_APN) + "\"";
  if (strlen(MODEM_USER) || strlen(MODEM_PASS))
    cstt += ",\"" + String(MODEM_USER) + "\",\"" + String(MODEM_PASS) + "\"";
  sendAT(cstt, 2000, "OK");

  if (sendAT("AT+CIICR", 30000, "OK").indexOf("OK") == -1) {
    gprsUp = false;
    return false;
  }
  String ip = sendAT("AT+CIFSR", 3000, ".");
  gprsUp = ip.indexOf('.') != -1;
  if (gprsUp) Serial.println("[GPRS] Up, IP: " + ip);
  return gprsUp;
}

String collectFor(uint32_t ms) {
  String s;
  unsigned long t0 = millis();
  while (millis() - t0 < ms) {
    pumpGps();
    while (SerialAT.available()) {
      s += (char)SerialAT.read();
      if (s.length() > 384) s = s.substring(s.length() - 256);
    }
    delay(5);
  }
  return s;
}

bool postToLocalServer(double lat, double lon, const char* ts, float speedKmh, uint32_t sats) {
  if (!netRegistered) { Serial.println("[LOCAL] No network reg"); return false; }

  sendAT("AT+CIPSHUT", 4000, "SHUT");
  gprsUp = false;
  if (!ensureGprsUp())  { Serial.println("[LOCAL] GPRS bring-up failed"); return false; }

  // Build TCP connect string with local IP
  String cmd = "AT+CIPSTART=\"TCP\",\"" + String(LOCAL_SERVER_HOST) + "\"," + String(LOCAL_SERVER_PORT);
  String conn = sendAT(cmd, 20000, "CONNECT");
  conn += collectFor(1500);
  bool connected = conn.indexOf("CONNECT") != -1
                && conn.indexOf("FAIL")     == -1
                && conn.indexOf("ERROR")    == -1;
  if (!connected) {
    Serial.println("[LOCAL] TCP connect failed: " + conn);
    gprsUp = false;
    return false;
  }

  // Build JSON body - GeoLinker compatible format
  char json[256];
  snprintf(json, sizeof(json),
           "{\"device_id\":\"%s\",\"timestamp\":[\"%s\"],\"lat\":[%.6f],"
           "\"long\":[%.6f],\"payload\":[{\"speed\":%.1f,\"sats\":%u}]}",
           DEVICE_ID, ts, lat, lon, speedKmh, (unsigned)sats);

  if (sendAT("AT+CIPSEND", 5000, ">").indexOf(">") == -1) {
    Serial.println("[LOCAL] No CIPSEND prompt");
    sendAT("AT+CIPCLOSE", 3000, "CLOSE");
    return false;
  }

  // HTTP POST to local server
  SerialAT.print("POST "); SerialAT.print(LOCAL_API_PATH); SerialAT.print(" HTTP/1.1\r\n");
  SerialAT.print("Host: "); SerialAT.print(LOCAL_SERVER_HOST); SerialAT.print(":"); SerialAT.print(LOCAL_SERVER_PORT); SerialAT.print("\r\n");
  SerialAT.print("Authorization: "); SerialAT.print(API_KEY); SerialAT.print("\r\n");
  SerialAT.print("Content-Type: application/json\r\n");
  SerialAT.print("Connection: close\r\n");
  SerialAT.printf("Content-Length: %u\r\n", (unsigned)strlen(json));
  SerialAT.print("\r\n");
  SerialAT.print(json);
  SerialAT.write(0x1A);

  String buf;
  int code = 0;
  unsigned long t0 = millis(), gotCodeAt = 0;
  while (millis() - t0 < 25000) {
    pumpGps();
    while (SerialAT.available()) {
      buf += (char)SerialAT.read();
      if (buf.length() > 512) buf = buf.substring(buf.length() - 384);
    }
    if (!code) {
      int i = buf.indexOf("HTTP/1.1 ");
      if (i == -1) i = buf.indexOf("HTTP/1.0 ");
      if (i != -1) { code = buf.substring(i + 9, i + 12).toInt(); gotCodeAt = millis(); }
    } else if (buf.indexOf("CLOSED") != -1 || millis() - gotCodeAt > 1500) {
      break;
    }
    if (buf.indexOf("+CME ERROR") != -1 || buf.indexOf("SEND FAIL") != -1) break;
    delay(5);
  }
  sendAT("AT+CIPCLOSE", 2000, "CLOSE");

  lastHttpCode = code;
  int bodyIdx = buf.indexOf("\r\n\r\n");
  if (bodyIdx != -1) Serial.println("[LOCAL] Server: " + buf.substring(bodyIdx + 4));

  if (code == 200 || code == 201) return true;
  Serial.printf("[LOCAL] HTTP status %d\n", code);
  return false;
}
