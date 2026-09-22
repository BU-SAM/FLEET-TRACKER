/*
 * TTGO T-Call V1.4 - WiFi TEST version (no SIM800L needed)
 * Use this to test your local server quickly on same WiFi network
 * This proves map + backend works before dealing with cellular + tunnels
 *
 * Steps:
 * 1. Set WIFI_SSID, WIFI_PASS, LOCAL_SERVER_HOST (your PC's IP)
 * 2. Flash
 * 3. Open Serial Monitor 115200
 * 4. Check http://localhost:8000 - you should see car_tracker_01 moving
 *
 * This uses ESP32 WiFi, not SIM800L. Once this works, switch to tunnel firmware for real field test.
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <TinyGPSPlus.h>

static const char* WIFI_SSID = "YOUR_WIFI_SSID";
static const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";

static const char* LOCAL_SERVER_HOST = "192.168.1.50"; // YOUR PC IP
static const int   LOCAL_SERVER_PORT = 8000;
static const char* LOCAL_API_PATH    = "/api/v1/geolinker";
static const char* DEVICE_ID = "car_tracker_01";

static const long GPS_BAUD   = 9600;
static const int  GPS_RX_PIN = 32;
static const int  GPS_TX_PIN = 33;
static const long TZ_OFFSET_MIN = 60;

TinyGPSPlus gps;

bool makeTimestamp(char* out, size_t n);
static long daysFromCivil(int y, int m, int d);
static void civilFromDays(long z, int &y, int &m, int &d);
void pumpGps();

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=== T-Call WiFi TEST -> Local Server ===");
  Serial.printf("Target: http://%s:%d%s\n", LOCAL_SERVER_HOST, LOCAL_SERVER_PORT, LOCAL_API_PATH);

  Serial2.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  Serial.print("GPS on UART2... ");

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("WiFi connecting");
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 30) {
    delay(500);
    Serial.print(".");
    tries++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nWiFi connected! IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\nWiFi failed! Check SSID/PASS. Will still try GPS.");
  }
}

void loop() {
  pumpGps();

  static unsigned long lastPrint = 0;
  if (millis() - lastPrint >= 1000) {
    lastPrint = millis();
    if (gps.location.isValid()) {
      Serial.printf("Fix: %.6f,%.6f sats=%d\n", gps.location.lat(), gps.location.lng(), gps.satellites.value());
    } else {
      Serial.println("No GPS fix yet... (go outside, wait 30-90s)");
    }
  }

  static unsigned long lastSend = 0;
  if (millis() - lastSend >= 15000) {
    if (!gps.location.isValid() || gps.location.age() > 5000) {
      Serial.println("[WiFi] No fresh fix, skipping");
      return;
    }
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[WiFi] Not connected");
      return;
    }

    char ts[20];
    if (!makeTimestamp(ts, sizeof(ts))) {
      Serial.println("[WiFi] No date/time yet");
      return;
    }

    lastSend = millis();

    // Build JSON
    char json[256];
    snprintf(json, sizeof(json),
      "{\"device_id\":\"%s\",\"timestamp\":[\"%s\"],\"lat\":[%.6f],\"long\":[%.6f],\"payload\":[{\"speed\":%.1f,\"sats\":%u}]}",
      DEVICE_ID, ts, gps.location.lat(), gps.location.lng(),
      gps.speed.isValid() ? gps.speed.kmph() : 0.0f,
      gps.satellites.isValid() ? gps.satellites.value() : 0);

    String url = String("http://") + LOCAL_SERVER_HOST + ":" + LOCAL_SERVER_PORT + LOCAL_API_PATH;
    Serial.printf("[WiFi] POST %s\n%s\n", url.c_str(), json);

    HTTPClient http;
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    int code = http.POST(json);
    String resp = http.getString();
    Serial.printf("[WiFi] HTTP %d: %s\n", code, resp.c_str());
    http.end();
    Serial.println("----------------------------------------");
  }
}

void pumpGps() { while (Serial2.available()) gps.encode(Serial2.read()); }

bool makeTimestamp(char* out, size_t n) {
  if (!gps.date.isValid() || !gps.time.isValid()) return false;
  long days = daysFromCivil(gps.date.year(), gps.date.month(), gps.date.day());
  long epoch = days*86400L + gps.time.hour()*3600L + gps.time.minute()*60L + gps.time.second() + TZ_OFFSET_MIN*60L;
  long d = epoch/86400L; long rem = epoch%86400L; if (rem<0){rem+=86400L; d-=1;}
  int y,m,dd; civilFromDays(d,y,m,dd);
  snprintf(out,n,"%04d-%02d-%02d %02d:%02d:%02d",y,m,dd,(int)(rem/3600),(int)((rem%3600)/60),(int)(rem%60));
  return true;
}
static long daysFromCivil(int y,int m,int d){y-=m<=2; long era=(y>=0?y:y-399)/400; unsigned yoe=y-era*400; unsigned doy=(153*(m+(m>2?-3:9))+2)/5+d-1; unsigned doe=yoe*365+yoe/4-yoe/100+doy; return era*146097L+doe-719468;}
static void civilFromDays(long z,int &y,int &m,int &d){z+=719468; long era=(z>=0?z:z-146096)/146097; unsigned doe=z-era*146097; unsigned yoe=(doe-doe/1460+doe/36524-doe/146096)/365; int yy=yoe+era*400; unsigned doy=doe-(365*yoe+yoe/4-yoe/100); unsigned mp=(5*doy+2)/153; d=doy-(153*mp+2)/5+1; m=mp+(mp<10?3:-9); y=yy+(m<=2);}
