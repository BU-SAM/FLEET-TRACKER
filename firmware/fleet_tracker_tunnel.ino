/*
 * TTGO T-Call V1.4 -> LOCAL SERVER via PUBLIC TUNNEL (for CGNAT routers)
 * Use this when you can't port forward. Works with:
 * - localtunnel:  npx localtunnel --port 8000
 * - ngrok:        ngrok http 8000
 * - cloudflared:  cloudflared tunnel --url http://localhost:8000
 *
 * SIM800L can only do PLAIN HTTP port 80, not HTTPS!
 * So set LOCAL_SERVER_PORT = 80 and host = your tunnel domain without https://
 *
 * Example:
 *   localtunnel gives: https://loud-paws-shout.loca.lt
 *   You set: HOST = "loud-paws-shout.loca.lt" PORT = 80
 *
 *   ngrok gives: https://a1b2c3d4.ngrok-free.app
 *   You set: HOST = "a1b2c3d4.ngrok-free.app" PORT = 80
 *
 * This sketch adds extra headers to bypass ngrok/localtunnel warnings.
 */

#include <TinyGPSPlus.h>
#include <Wire.h>

// ============================== TUNNEL CONFIG ================================
static const char* LOCAL_SERVER_HOST = "REPLACE_WITH_YOUR_TUNNEL.loca.lt"; // e.g. loud-paws-shout.loca.lt
static const int   LOCAL_SERVER_PORT = 80;  // ALWAYS 80 for tunnels (plain HTTP)
static const char* LOCAL_API_PATH    = "/api/v1/geolinker";

static const char* API_KEY   = ""; // leave empty if server has no FLEET_API_KEY set
static const char* DEVICE_ID = "car_tracker_01"; // change per car

// --- APN ---
static const char* MODEM_APN  = "internet.ng.airtel.com";
static const char* MODEM_USER = "";
static const char* MODEM_PASS = "";

static const uint32_t SEND_INTERVAL_MS = 15000;
static const long     TZ_OFFSET_MIN    = 60;

// ============================== GPS ==================================
static const long GPS_BAUD   = 9600;
static const int  GPS_RX_PIN = 32;
static const int  GPS_TX_PIN = 33;
TinyGPSPlus gps;

// ===================== MODEM ========================
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
static bool gprsUp = false;
static int  lastHttpCode = 0;
static uint8_t consecutiveFails = 0;

void printStatus();
void pumpGps();
bool makeTimestamp(char* out, size_t n);
static long daysFromCivil(int y, int m, int d);
static void civilFromDays(long z, int &y, int &m, int &d);
static bool setPowerBoostKeepOn(bool en);
void modemPowerOn();
void modemInit();
String sendAT(const String& cmd, uint32_t timeoutMs, const char* expect);
bool ensureGprsUp();
String collectFor(uint32_t ms);
bool postToTunnel(double lat, double lon, const char* ts, float speedKmh, uint32_t sats);

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println();
  Serial.println("=== T-Call V1.4 -> TUNNEL TEST ===");
  Serial.printf("Tunnel: http://%s:%d%s\n", LOCAL_SERVER_HOST, LOCAL_SERVER_PORT, LOCAL_API_PATH);
  Serial.printf("Device: %s\n", DEVICE_ID);
  Serial2.setRxBufferSize(2048);
  Serial2.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  modemPowerOn();
  modemInit();
}

void loop() {
  pumpGps();
  static unsigned long lastPrint = 0;
  if (millis() - lastPrint >= 1000) { lastPrint = millis(); printStatus(); }

  static unsigned long lastAttempt = 0;
  bool fixFresh = gps.location.isValid() && gps.location.age() < 5000;
  if (fixFresh && millis() - lastAttempt >= SEND_INTERVAL_MS) {
    lastAttempt = millis();
    char ts[20];
    if (!makeTimestamp(ts, sizeof(ts))) { Serial.println("[TUNNEL] No date yet"); return; }
    Serial.printf("[TUNNEL] Uploading %s %.6f,%.6f to %s\n", ts, gps.location.lat(), gps.location.lng(), LOCAL_SERVER_HOST);
    bool ok = false;
    for (uint8_t attempt = 1; attempt <= 3 && !ok; attempt++) {
      if (attempt > 1) { Serial.printf("[TUNNEL] Retry %u/3...\n", attempt); delay(3000); }
      ok = postToTunnel(gps.location.lat(), gps.location.lng(), ts, gps.speed.isValid() ? gps.speed.kmph() : 0.0f, gps.satellites.isValid() ? gps.satellites.value() : 0);
    }
    if (ok) { consecutiveFails = 0; Serial.println("[TUNNEL] Stored!"); }
    else {
      consecutiveFails++;
      Serial.printf("[TUNNEL] FAILED %u in a row\n", consecutiveFails);
      if (consecutiveFails >= 5) { modemPowerOn(); modemInit(); consecutiveFails = 0; }
    }
    Serial.println("----------------------------------------");
  }
}

void pumpGps() { while (Serial2.available()) gps.encode(Serial2.read()); }

void printStatus() {
  if (gps.location.isValid()) {
    Serial.printf("LAT: %.6f LON: %.6f SAT: %d HDOP: %.1f\n", gps.location.lat(), gps.location.lng(), gps.satellites.isValid()?gps.satellites.value():0, gps.hdop.isValid()?gps.hdop.hdop():0);
  } else Serial.print("No fix yet... ");
  Serial.printf("NET: %s GPRS: %s HTTP: %d\n", netRegistered?"reg":"search", gprsUp?"up":"down", lastHttpCode);
}

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

static bool setPowerBoostKeepOn(bool en){Wire.begin(I2C_SDA,I2C_SCL); Wire.beginTransmission(IP5306_ADDR); Wire.write(IP5306_REG_SYS_CTL0); Wire.write(en?0x37:0x35); return Wire.endTransmission()==0;}
void modemPowerOn(){
  Serial.println(setPowerBoostKeepOn(true)?"[PWR] IP5306 keep-on OK":"[PWR] IP5306 not found");
  pinMode(MODEM_PWRKEY,OUTPUT); pinMode(MODEM_RST,OUTPUT); pinMode(MODEM_POWER_ON,OUTPUT);
  digitalWrite(MODEM_PWRKEY,LOW); digitalWrite(MODEM_RST,HIGH); digitalWrite(MODEM_POWER_ON,HIGH);
  SerialAT.setRxBufferSize(1024); SerialAT.begin(MODEM_BAUD,SERIAL_8N1,MODEM_RX,MODEM_TX); delay(3000);
  bool ok=false; for(uint8_t r=0;r<2&&!ok;r++){for(uint8_t i=0;i<10&&!ok;i++){ok=sendAT("AT",1000,"OK").indexOf("OK")!=-1; delay(300);} if(!ok){Serial.println("[GSM] Pulsing PWRKEY"); digitalWrite(MODEM_PWRKEY,HIGH); delay(1200); digitalWrite(MODEM_PWRKEY,LOW); delay(5000);}}
  Serial.println(ok?"[GSM] SIM800L OK":"[GSM] No answer");
}
void modemInit(){
  sendAT("ATE0",1000,"OK");
  String cpin=sendAT("AT+CPIN?",3000,"CPIN"); if(cpin.indexOf("READY")==-1) Serial.println("[GSM] SIM issue: "+cpin);
  netRegistered=false; Serial.print("[GSM] Registering"); unsigned long t0=millis();
  while(millis()-t0<90000){String r=sendAT("AT+CREG?",2000,"+CREG:"); int c=r.indexOf(','); int stat=(c!=-1)?r.substring(c+1,c+2).toInt():-1; if(stat==1||stat==5){netRegistered=true; break;} Serial.print('.'); delay(1500);}
  Serial.println(netRegistered?"\n[GSM] Registered":"\n[GSM] No network");
  Serial.println("[GSM] "+sendAT("AT+CSQ",1000,"+CSQ")); sendAT("AT+CGATT=1",10000,"OK"); gprsUp=false;
}
String sendAT(const String& cmd,uint32_t timeoutMs,const char* expect){
  while(SerialAT.available()) SerialAT.read(); if(cmd.length()) SerialAT.println(cmd);
  String resp; unsigned long t0=millis(); while(millis()-t0<timeoutMs){pumpGps(); while(SerialAT.available()){resp+=(char)SerialAT.read(); if(resp.length()>384) resp=resp.substring(resp.length()-256);} if(expect&&resp.indexOf(expect)!=-1) break; delay(5);} return resp;
}
bool ensureGprsUp(){
  if(gprsUp&&sendAT("AT+CIFSR",2000,".").indexOf('.')!=-1) return true;
  sendAT("AT+CIPSHUT",5000,"SHUT OK"); String cstt="AT+CSTT=\""+String(MODEM_APN)+"\""; if(strlen(MODEM_USER)||strlen(MODEM_PASS)) cstt+=",\""+String(MODEM_USER)+"\",\""+String(MODEM_PASS)+"\""; sendAT(cstt,2000,"OK");
  if(sendAT("AT+CIICR",30000,"OK").indexOf("OK")==-1){gprsUp=false; return false;} String ip=sendAT("AT+CIFSR",3000,"."); gprsUp=ip.indexOf('.')!=-1; if(gprsUp) Serial.println("[GPRS] Up "+ip); return gprsUp;
}
String collectFor(uint32_t ms){String s; unsigned long t0=millis(); while(millis()-t0<ms){pumpGps(); while(SerialAT.available()){s+=(char)SerialAT.read(); if(s.length()>384) s=s.substring(s.length()-256);} delay(5);} return s;}

bool postToTunnel(double lat,double lon,const char* ts,float speedKmh,uint32_t sats){
  if(!netRegistered) return false;
  sendAT("AT+CIPSHUT",4000,"SHUT"); gprsUp=false; if(!ensureGprsUp()) return false;
  String cmd="AT+CIPSTART=\"TCP\",\""+String(LOCAL_SERVER_HOST)+"\","+String(LOCAL_SERVER_PORT);
  String conn=sendAT(cmd,20000,"CONNECT")+collectFor(1500);
  if(conn.indexOf("CONNECT")==-1||conn.indexOf("FAIL")!=-1) {Serial.println("[TUNNEL] TCP fail "+conn); gprsUp=false; return false;}

  char json[256];
  snprintf(json,sizeof(json),"{\"device_id\":\"%s\",\"timestamp\":[\"%s\"],\"lat\":[%.6f],\"long\":[%.6f],\"payload\":[{\"speed\":%.1f,\"sats\":%u}]}",DEVICE_ID,ts,lat,lon,speedKmh,(unsigned)sats);
  if(sendAT("AT+CIPSEND",5000,">").indexOf(">")==-1){sendAT("AT+CIPCLOSE",3000,"CLOSE"); return false;}

  // HTTP with bypass headers for ngrok/localtunnel
  SerialAT.print("POST "); SerialAT.print(LOCAL_API_PATH); SerialAT.print(" HTTP/1.1\r\n");
  SerialAT.print("Host: "); SerialAT.print(LOCAL_SERVER_HOST); SerialAT.print("\r\n");
  if(strlen(API_KEY)) {SerialAT.print("Authorization: "); SerialAT.print(API_KEY); SerialAT.print("\r\n");}
  SerialAT.print("Content-Type: application/json\r\n");
  SerialAT.print("ngrok-skip-browser-warning: true\r\n"); // bypass ngrok warning
  SerialAT.print("Bypass-Tunnel-Reminder: true\r\n"); // bypass localtunnel reminder
  SerialAT.print("User-Agent: TTGO-Tracker/1.0\r\n");
  SerialAT.print("Connection: close\r\n");
  SerialAT.printf("Content-Length: %u\r\n\r\n", (unsigned)strlen(json));
  SerialAT.print(json);
  SerialAT.write(0x1A);

  String buf; int code=0; unsigned long t0=millis(), gotCodeAt=0;
  while(millis()-t0<25000){
    pumpGps(); while(SerialAT.available()){buf+=(char)SerialAT.read(); if(buf.length()>1024) buf=buf.substring(buf.length()-512);}
    if(!code){int i=buf.indexOf("HTTP/1.1 "); if(i==-1) i=buf.indexOf("HTTP/1.0 "); if(i!=-1){code=buf.substring(i+9,i+12).toInt(); gotCodeAt=millis();}}
    else if(buf.indexOf("CLOSED")!=-1||millis()-gotCodeAt>2000) break;
    if(buf.indexOf("+CME ERROR")!=-1||buf.indexOf("SEND FAIL")!=-1) break; delay(5);
  }
  sendAT("AT+CIPCLOSE",2000,"CLOSE");
  lastHttpCode=code;
  int bodyIdx=buf.indexOf("\r\n\r\n"); if(bodyIdx!=-1) Serial.println("[TUNNEL] Resp: "+buf.substring(bodyIdx+4, bodyIdx+200));
  Serial.printf("[TUNNEL] HTTP %d\n", code);
  return code==200||code==201;
}
