/*
 * TTGO T-Call V1.4 (ESP32-WROVER + SIM800L) GPS Tracker -> CircuitDigest Cloud "GeoLinker"
 * ORIGINAL CODE - kept for reference
 * See fleet_tracker_local.ino for local server version
 */

/*
 * TTGO T-Call V1.4 (ESP32-WROVER + SIM800L) GPS Tracker -> CircuitDigest Cloud "GeoLinker"
 *
 * GPS side is the proven WROVER-E sketch (TinyGPSPlus, UART2 @ 9600 on GPIO32/33).
 * GSM side uploads each valid fix to the GeoLinker API over the onboard SIM800L's
 * 2G GPRS link, using PLAIN HTTP on port 80 over a raw TCP socket - the exact
 * transport used & tested by CircuitDigest's own SIM800L GeoLinkerLite library
 * (SIM800L's embedded TLS is too old for modern HTTPS servers).
 *
 * SETUP CHECKLIST:
 *   1. Account: register at https://circuitdigest.cloud -> My Account -> API Keys,
 *      generate a key and paste it into API_KEY below.FF
 *   2. Set MODEM_APN to match your SIM (Nigerian cheat-sheet below).
 *   3. SIM card: 2G data enabled, airtime/data bundle loaded, PIN lock REMOVED
 *      (put it in a phone and disable the PIN first).
 *   4. GSM antenna attached to the onboard SIM800L u.fl connector!
 *   5. Arduino IDE board: "ESP32 WROVER Module" (Dev Module also works). Monitor 115200.
 *   6. Library: TinyGPSPlus by Mikal Hart (Library Manager). No GSM lib needed.
 *
 * WIRING (GPS only - the modem is on the board):
 *   GY-GPS6MV2 : VCC->3V3  GND->GND  TXD->GPIO32  RXD->GPIO33
 *
 * POWER: SIM800L pulls ~2A bursts. Use a decent 5V/2A USB supply or a LiPo on the
 *        battery connector - a weak USB port causes brown-out resets on transmit.
 *
 * SERVER CONTRACT (from the GeoLinker docs - https://circuitdigest.com/tutorial/
 * gps-visualizer-for-iot-based-gps-tracking-projects):
 *   POST  www.circuitdigest.cloud/api/v1/geolinker   (Host: www.circuitdigest.cloud)
 *   Headers: Authorization: <API key> , Content-Type: application/json
 *   Body   : {"device_id":"...", "timestamp":["YYYY-MM-DD HH:MM:SS"],
 *             "lat":[<float>], "long":[<float>], "payload":[{...}]}   // battery[] optional
 *   Codes  : 200/201 = stored | 400 bad payload | 401 bad key | 500 server error
 *   Limits : 1 request / 10 s ; 10 000 points per key, oldest overwritten (FIFO).
 *            At 15 s intervals, 10 000 points ~ 41 hours of continuous tracking.
 */

#include <TinyGPSPlus.h>
#include <Wire.h>

// ============================== USER CONFIG ==================================
static const char* API_KEY   = "cd_sam_080926_JY1Jhs";
static const char* DEVICE_ID = "car_tracker_01";   // shows up under "My Trackers"

// --- Nigerian GPRS APN cheat-sheet (keep the one that matches your SIM) ---
//   MTN:     "web.gprs.mtnnigeria.net"   user ""      pass ""
//   Airtel:  "internet.ng.airtel.com"    user ""      pass ""
//   Glo:     "gloflat"                   user "flat"  pass "flat"
//   9mobile: "9mobile"                   user ""      pass ""
static const char* MODEM_APN  = "internet.ng.airtel.com";
static const char* MODEM_USER = "";        // Glo: "flat"
static const char* MODEM_PASS = "";        // Glo: "flat"

static const uint32_t SEND_INTERVAL_MS = 15000;  // API: max 1 request / 10 s
static const long     TZ_OFFSET_MIN    = 60;     // timestamps sent in WAT (UTC+1); set 0 for raw UTC

// ============================== GPS (UART2) ==================================
static const long GPS_BAUD   = 9600;   // GY-GPS6MV2 factory default
static const int  GPS_RX_PIN = 32;     // ESP32 pin that READS the GPS TXD
static const int  GPS_TX_PIN = 33;     // ESP32 pin that drives the GPS RXD
#define PRINT_RAW_NMEA 0               // 1 = mirror raw NMEA sentences (debug)

TinyGPSPlus gps;

// ===================== MODEM: onboard SIM800L (UART1) ========================
// T-Call V1.4 fixed pins (LilyGo/TinyGSM board definitions)
static const int MODEM_PWRKEY   = 4;
static const int MODEM_RST      = 5;
static const int MODEM_POWER_ON = 23;
static const int MODEM_RX       = 26;    // ESP32 RXD1  <- SIM800L TXD
static const int MODEM_TX       = 27;    // ESP32 TXD1  -> SIM800L RXD
static const long MODEM_BAUD    = 115200;

// V1.4 has an IP5306 power-management IC on I2C (0x75). Its boost converter must
// be told to stay ON, else the modem rail drops out during GSM transmit bursts.
static const int I2C_SDA = 21;
static const int I2C_SCL = 22;
#define IP5306_ADDR         0x75
#define IP5306_REG_SYS_CTL0 0x00

#define SerialAT Serial1

// ------------------------------- state --------------------------------------
static bool netRegistered = false;
static bool gprsUp        = false;
static int  lastHttpCode  = 0;           // 0 = nothing sent yet
static uint8_t consecutiveFails = 0;

// ========================= forward declarations ============================
// Harmless in the Arduino IDE (it generates its own); required if this is ever
// compiled as plain C++ (e.g. PlatformIO).
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
bool  postToGeolinker(double lat, double lon, const char* ts, float speedKmh, uint32_t sats);

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println();
  Serial.println("=== T-Call V1.4 GPS Tracker -> GeoLinker ===");

  // GPS on UART2 (identical to the proven sketch)
  Serial2.setRxBufferSize(2048);       // absorb NMEA while the modem is busy (~2 s)
  Serial2.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);

  modemPowerOn();
  modemInit();

  Serial.print("GPS listening on UART2 @ ");
  Serial.print(GPS_BAUD);
  Serial.println(" baud. Cold start can take 30-90 s with a clear sky view.");
}

void loop() {
  pumpGps();

  // Console status once per second (same readout as the original sketch + GSM line)
  static unsigned long lastPrint = 0;
  if (millis() - lastPrint >= 1000) {
    lastPrint = millis();
    printStatus();
  }

  // Upload on a fixed cadence when we have a live fix. (No special first-send
  // case needed: modem init in setup already burns >15 s, so the very first
  // fix passes the interval check immediately.)
  static unsigned long lastAttempt = 0;
  bool fixFresh = gps.location.isValid() && gps.location.age() < 5000;

  if (fixFresh && millis() - lastAttempt >= SEND_INTERVAL_MS) {
    lastAttempt = millis();

    char ts[20];
    if (!makeTimestamp(ts, sizeof(ts))) {
      Serial.println("[GEO] Fix OK but no date/time yet - skipping this point");
      return;
    }

    Serial.printf("[GEO] Uploading %s  lat=%.6f long=%.6f ...\n",
                  ts, gps.location.lat(), gps.location.lng());

    bool ok = false;
    for (uint8_t attempt = 1; attempt <= 3 && !ok; attempt++) {
      if (attempt > 1) {
        Serial.printf("[GEO] Retry %u/3...\n", attempt);
        delay(3000);                    // only retries *failed* posts, so no rate-limit risk
      }
      ok = postToGeolinker(gps.location.lat(), gps.location.lng(), ts,
                           gps.speed.isValid() ? gps.speed.kmph() : 0.0f,
                           gps.satellites.isValid() ? gps.satellites.value() : 0);
    }

    if (ok) {
      consecutiveFails = 0;
      Serial.println(lastHttpCode == 201
        ? "[GEO] Device created (HTTP 201). Open circuitdigest.cloud -> GeoLinker ->"
          " Track, then hit refresh once - first point needs a manual page refresh."
        : "[GEO] Stored.");
    } else {
      consecutiveFails++;
      Serial.printf("[GEO] Upload FAILED (%u in a row)\n", consecutiveFails);
      if (consecutiveFails >= 5) {      // modem is likely wedged - bounce it
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
//                                 GPS
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
    // Serial.print(x, 2) prints BINARY for ints (2 = base) - printf %02u instead.
    Serial.printf("UTC: %04u-%02u-%02u  %02u:%02u:%02u\n",
                  (unsigned)gps.date.year(), (unsigned)gps.date.month(), (unsigned)gps.date.day(),
                  (unsigned)gps.time.hour(), (unsigned)gps.time.minute(), (unsigned)gps.time.second());
  }

  // --- GSM state line ---
  Serial.printf("NET: %s   GPRS: %s   Last HTTP: %d\n",
                netRegistered ? "registered" : "searching...",
                gprsUp ? "up" : "down",
                lastHttpCode);
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

// "YYYY-MM-DD HH:MM:SS" from GPS UTC + TZ_OFFSET_MIN, with correct date rollover
bool makeTimestamp(char* out, size_t n) {
  if (!gps.date.isValid() || !gps.time.isValid()) return false;

  long days  = daysFromCivil((int)gps.date.year(), (int)gps.date.month(), (int)gps.date.day());
  long epoch = days * 86400L + (long)gps.time.hour() * 3600L
             + (long)gps.time.minute() * 60L + (long)gps.time.second()
             + TZ_OFFSET_MIN * 60L;

  long d   = epoch / 86400L;
  long rem = epoch % 86400L;
  if (rem < 0) { rem += 86400L; d -= 1; }       // can only happen with negative offsets

  int y, m, dd;
  civilFromDays(d, y, m, dd);
  snprintf(out, n, "%04d-%02d-%02d %02d:%02d:%02d",
           y, m, dd, (int)(rem / 3600), (int)((rem % 3600) / 60), (int)(rem % 60));
  return true;
}

// Howard Hinnant's days_from_civil / civil_from_days (pub. domain algorithms)
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
//                              SIM800L / GPRS
// ============================================================================
static bool setPowerBoostKeepOn(bool en) {
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.beginTransmission(IP5306_ADDR);
  Wire.write(IP5306_REG_SYS_CTL0);
  Wire.write(en ? 0x37 : 0x35);          // bit1 of reg 0x00: boost keep-on
  return Wire.endTransmission() == 0;
}

void modemPowerOn() {
  Serial.println(setPowerBoostKeepOn(true)
                 ? "[PWR] IP5306 boost keep-on enabled"
                 : "[PWR] IP5306 not found (not a V1.4?) - continuing anyway");

  pinMode(MODEM_PWRKEY,   OUTPUT);
  pinMode(MODEM_RST,      OUTPUT);
  pinMode(MODEM_POWER_ON, OUTPUT);
  digitalWrite(MODEM_PWRKEY,   LOW);
  digitalWrite(MODEM_RST,      HIGH);
  digitalWrite(MODEM_POWER_ON, HIGH);

  SerialAT.setRxBufferSize(1024);
  SerialAT.begin(MODEM_BAUD, SERIAL_8N1, MODEM_RX, MODEM_TX);
  delay(3000);                            // SIM800L boot time

  // Sync baud; if the modem was off, pulse PWRKEY like its button would
  bool ok = false;
  for (uint8_t round = 0; round < 2 && !ok; round++) {
    for (uint8_t i = 0; i < 10 && !ok; i++) {
      ok = sendAT("AT", 1000, "OK").indexOf("OK") != -1;
      delay(300);
    }
    if (!ok) {                            // modem probably powered down
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
  sendAT("ATE0", 1000, "OK");                                // echo off

  String cpin = sendAT("AT+CPIN?", 3000, "CPIN");
  if (cpin.indexOf("READY") == -1) {
    Serial.println("[GSM] !! SIM problem: " + cpin);
    if (cpin.indexOf("SIM PIN") != -1)
      Serial.println("[GSM] !! Remove the SIM PIN using a phone, then retry.");
  }

  // Wait for network registration (stat 1 = home, 5 = roaming)
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
  Serial.println(netRegistered ? "\n[GSM] Registered" : "\n[GSM] !! No network (check SIM/antenna/2G coverage)");

  Serial.println("[GSM] Signal: " + sendAT("AT+CSQ", 1000, "+CSQ"));
  sendAT("AT+CGATT=1", 10000, "OK");                         // attach to GPRS
  gprsUp = false;
}

// Send an AT command and collect the reply; GPS stream keeps being parsed
// while we wait so location parsing never stalls behind the modem.
String sendAT(const String& cmd, uint32_t timeoutMs, const char* expect) {
  while (SerialAT.available()) SerialAT.read();              // flush stale
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

  sendAT("AT+CIPSHUT", 5000, "SHUT OK");                     // clean slate
  String cstt = "AT+CSTT=\"" + String(MODEM_APN) + "\"";
  if (strlen(MODEM_USER) || strlen(MODEM_PASS))
    cstt += ",\"" + String(MODEM_USER) + "\",\"" + String(MODEM_PASS) + "\"";
  sendAT(cstt, 2000, "OK");

  if (sendAT("AT+CIICR", 30000, "OK").indexOf("OK") == -1) { // bring up PDP
    gprsUp = false;
    return false;
  }
  String ip = sendAT("AT+CIFSR", 3000, ".");                 // ask for our IP
  gprsUp = ip.indexOf('.') != -1;
  if (gprsUp) Serial.println("[GPRS] Up");
  return gprsUp;
}

// Keep collecting whatever the modem prints for a while (GPS keeps parsing).
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

bool postToGeolinker(double lat, double lon, const char* ts, float speedKmh, uint32_t sats) {
  if (!netRegistered) { Serial.println("[GEO] No network reg"); return false; }

  sendAT("AT+CIPSHUT", 4000, "SHUT");   // clean slate each cycle; tolerable if it errors
  gprsUp = false;
  if (!ensureGprsUp())  { Serial.println("[GEO] GPRS bring-up failed"); return false; }

  // Open raw TCP to the server (plain HTTP:80 - the GeoLinkerLite-proven path)
  String conn = sendAT("AT+CIPSTART=\"TCP\",\"www.circuitdigest.cloud\",80",
                       20000, "CONNECT");
  conn += collectFor(1500);   // let " OK"/" FAIL" finish printing after "CONNECT"
  bool connected = conn.indexOf("CONNECT") != -1    // covers "CONNECT OK" & "ALREADY CONNECT"
                && conn.indexOf("FAIL")     == -1
                && conn.indexOf("ERROR")    == -1;
  if (!connected) {
    Serial.println("[GEO] TCP connect failed: " + conn);
    gprsUp = false;
    return false;
  }

  // Build the JSON body (must know its length before the headers go out)
  char json[224];
  snprintf(json, sizeof(json),
           "{\"device_id\":\"%s\",\"timestamp\":[\"%s\"],\"lat\":[%.6f],"
           "\"long\":[%.6f],\"payload\":[{\"speed\":%.1f,\"sats\":%u}]}",
           DEVICE_ID, ts, lat, lon, speedKmh, (unsigned)sats);

  if (sendAT("AT+CIPSEND", 5000, ">").indexOf(">") == -1) {
    Serial.println("[GEO] No CIPSEND prompt");
    sendAT("AT+CIPCLOSE", 3000, "CLOSE");
    return false;
  }

  SerialAT.print("POST /api/v1/geolinker HTTP/1.1\r\n");
  SerialAT.print("Host: www.circuitdigest.cloud\r\n");
  SerialAT.print("Authorization: "); SerialAT.print(API_KEY); SerialAT.print("\r\n");
  SerialAT.print("Content-Type: application/json\r\n");
  SerialAT.print("Connection: close\r\n");
  SerialAT.printf("Content-Length: %u\r\n", (unsigned)strlen(json));
  SerialAT.print("\r\n");
  SerialAT.print(json);
  SerialAT.write(0x1A);                                      // Ctrl-Z = send it

  // Wait for the server's HTTP status line
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
      break;                                                 // got status + body sample
    }
    if (buf.indexOf("+CME ERROR") != -1 || buf.indexOf("SEND FAIL") != -1) break;
    delay(5);
  }
  sendAT("AT+CIPCLOSE", 2000, "CLOSE");

  lastHttpCode = code;
  int bodyIdx = buf.indexOf("\r\n\r\n");
  if (bodyIdx != -1) Serial.println("[GEO] Server: " + buf.substring(bodyIdx + 4));

  if (code == 200 || code == 201) return true;
  Serial.printf("[GEO] HTTP status %d\n", code);
  if (code == 401) Serial.println("[GEO] 401 = wrong/missing API key!");
  if (code == 429) Serial.println("[GEO] 429 = rate limit (keep interval >= 10 s)");
  return false;
}
