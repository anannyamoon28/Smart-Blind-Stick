#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <TinyGPS++.h>
#include <DFRobotDFPlayerMini.h>

// ================= BLIND STICK PINS (unchanged) =================
#define TRIG1 13
#define ECHO1 35
#define TRIG2 14
#define ECHO2 34
#define TRIG3 25
#define ECHO3 33
#define BUZZER 26
#define MP3_RX 21
#define MP3_TX 22

// ================= GPS / SOS PINS =================
#define SOS_BUTTON 27
#define GPS_RX_PIN 16
#define GPS_TX_PIN 17

// Set to 1 to print raw NMEA sentences (clutters the sonar status lines).
#define GPS_RAW_DEBUG 0

// ================= CREDENTIALS =================
const char* WIFI_SSID     = "Chowdhury";
const char* WIFI_PASSWORD = "52446661";
const String BOT_TOKEN    = "8934570970:AAEht86p_OqLAm0G2DIAWm5Ud9EABfzySXo";

const String CHAT_IDS[] = {"1798782759", "7499117589", "8431741319"};
const int NUM_CHATS = 3;

// ================= GLOBAL OBJECTS =================
HardwareSerial mp3Serial(1);
HardwareSerial gpsSerial(2);
DFRobotDFPlayerMini player;
TinyGPSPlus gps;
WiFiClientSecure secured_client;

// ================= AUDIO STATE =================
bool audioReady=false, audioPlaying=false;
int currentTrack=0;

#define QUEUE_SIZE 10
int audioQueue[QUEUE_SIZE];
int queueHead=0, queueTail=0;

// ================= BLIND STICK STATE =================
enum FrontState { FRONT_UNKNOWN, FRONT_CLEAR, FRONT_APPROACH, FRONT_OBSTACLE, FRONT_UP_STAIR };
enum GroundState { GROUND_UNKNOWN, GROUND_SAFE, GROUND_DOWN_STAIR, GROUND_DEEP_DROP };

FrontState frontState=FRONT_UNKNOWN, previousFrontState=FRONT_UNKNOWN;
GroundState groundState=GROUND_UNKNOWN, previousGroundState=GROUND_UNKNOWN;

bool warningArmed=false;
bool upStairActive=false;
bool ebarUthoPlayed=false;
int stairEndCount=0;

bool shortBeepActive=false;
unsigned long shortBeepStart=0;

// ================= SOS STATE =================
unsigned long buttonPressTime=0;
bool isButtonPressed=false;
bool alertSent=false;

// ================= ULTRASONIC =================
float readDistanceRaw(int trigPin,int echoPin){
  digitalWrite(trigPin,LOW); delayMicroseconds(5);
  digitalWrite(trigPin,HIGH); delayMicroseconds(10);
  digitalWrite(trigPin,LOW);
  unsigned long duration=pulseIn(echoPin,HIGH,30000);
  if(duration==0) return -1;
  return duration*0.0343/2.0;
}

// Median-of-3 filter. Valid readings are used; all timeouts => -1.
float readDistanceMedian3(int trigPin,int echoPin){
  float v[3]; int n=0;
  for(int i=0;i<3;i++){
    float d=readDistanceRaw(trigPin,echoPin);
    if(d>=0) v[n++]=d;
    if(i<2) delay(25);
  }
  if(n==0) return -1;
  if(n==1) return v[0];
  if(n==2) return (v[0]+v[1])/2.0;
  if(v[0]>v[1]){float t=v[0];v[0]=v[1];v[1]=t;}
  if(v[1]>v[2]){float t=v[1];v[1]=v[2];v[2]=t;}
  if(v[0]>v[1]){float t=v[0];v[0]=v[1];v[1]=t;}
  return v[1];
}

// ================= AUDIO =================
bool trackAlreadyQueued(int track){
  if(audioPlaying && currentTrack==track) return true;
  int i=queueHead;
  while(i!=queueTail){
    if(audioQueue[i]==track) return true;
    i=(i+1)%QUEUE_SIZE;
  }
  return false;
}

void requestAudio(int track){
  if(!audioReady || trackAlreadyQueued(track)) return;
  int next=(queueTail+1)%QUEUE_SIZE;
  if(next==queueHead){ Serial.println("AUDIO QUEUE FULL"); return; }
  audioQueue[queueTail]=track;
  queueTail=next;
  Serial.print("QUEUED: 000"); Serial.print(track); Serial.println(".mp3");
}

void updateAudio(){
  if(!audioReady) return;
  if(player.available()){
    uint8_t type=player.readType();
    int value=player.read(); (void)value;
    if(type==DFPlayerPlayFinished){
      Serial.print("FINISHED: 000"); Serial.print(currentTrack); Serial.println(".mp3");
      audioPlaying=false; currentTrack=0;
    }
  }
  if(!audioPlaying && queueHead!=queueTail){
    currentTrack=audioQueue[queueHead];
    queueHead=(queueHead+1)%QUEUE_SIZE;
    player.playMp3Folder(currentTrack);
    audioPlaying=true;
    Serial.print("PLAYING: 000"); Serial.print(currentTrack); Serial.println(".mp3");
  }
}

// ================= BUZZER =================
void startShortBeep(){ shortBeepActive=true; shortBeepStart=millis(); }

void updateBuzzer(bool veryClose,bool deepDrop){
  if(veryClose || deepDrop){ digitalWrite(BUZZER,HIGH); return; }
  if(shortBeepActive){
    if(millis()-shortBeepStart<200) digitalWrite(BUZZER,HIGH);
    else { shortBeepActive=false; digitalWrite(BUZZER,LOW); }
    return;
  }
  digitalWrite(BUZZER,LOW);
}

// ================= TELEGRAM SOS =================
void sendTelegramSOS(double lat, double lng, bool hasLock) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Telegram failed: Wi-Fi disconnected!");
    return;
  }

  String message = "EMERGENCY ALERT!\nBlind stick user needs help!\n\n";
  if (hasLock) {
    message += "Live Location: https://maps.google.com/?q=" + String(lat, 6) + "," + String(lng, 6);
  } else {
    message += "GPS satellite lock pending...";
  }

  for (int i = 0; i < NUM_CHATS; i++) {
    secured_client.setInsecure();

    if (!secured_client.connect("api.telegram.org", 443)) {
      Serial.println("Failed to connect to Telegram for Member " + String(i + 1));
      continue;
    }

    String url = "/bot" + BOT_TOKEN + "/sendMessage?chat_id=" + CHAT_IDS[i] + "&text=" + message;
    url.replace(" ", "%20");
    url.replace("\n", "%0A");

    secured_client.print(String("GET ") + url + " HTTP/1.1\r\n" +
                         "Host: api.telegram.org\r\n" +
                         "Connection: close\r\n\r\n");

    Serial.println("SOS Message Sent to Family Member " + String(i + 1));

    delay(500);
    secured_client.stop();
  }
}

// ================= GPS + SOS BUTTON =================
void updateGPS(){
  while (gpsSerial.available() > 0) {
    char c = gpsSerial.read();
#if GPS_RAW_DEBUG
    Serial.write(c);
#endif
    gps.encode(c);
  }
}

// Hold the button for 2 s to send the SOS (same logic as the original GPS code).
void updateSOSButton(){
  bool buttonState = (digitalRead(SOS_BUTTON) == LOW);

  if (buttonState && !isButtonPressed) {
    Serial.println("Wire connected! Timer started...");
    isButtonPressed = true;
    buttonPressTime = millis();
  } else if (buttonState && isButtonPressed) {
    if (!alertSent && (millis() - buttonPressTime >= 2000)) {
      Serial.println("2 seconds reached! Sending to Telegram...");
      digitalWrite(BUZZER, HIGH);
      delay(300);
      digitalWrite(BUZZER, LOW);

      if (gps.location.isValid()) {
        sendTelegramSOS(gps.location.lat(), gps.location.lng(), true);
      } else {
        sendTelegramSOS(0.0, 0.0, false);
      }
      alertSent = true;
    }
  } else if (!buttonState) {
    if (isButtonPressed) {
      Serial.println("Wire disconnected.");
    }
    isButtonPressed = false;
    alertSent = false;
  }
}

// Everything that must keep running while the sonar loop is waiting.
void serviceBackground(){
  updateGPS();
  updateSOSButton();
  updateAudio();
}

// Drop-in replacement for delay(): same wait time, but GPS/button/audio keep running.
void serviceDelay(unsigned long ms){
  unsigned long t0 = millis();
  while (millis() - t0 < ms) {
    serviceBackground();
    delay(1);
  }
}

// ================= SETUP =================
void setup(){
  Serial.begin(115200);
  delay(2000);
  Serial.println("\n\n====================================");
  Serial.println("   SMART BLIND STICK BOOTING UP!    ");
  Serial.println("====================================\n");

  pinMode(TRIG1,OUTPUT); pinMode(ECHO1,INPUT);
  pinMode(TRIG2,OUTPUT); pinMode(ECHO2,INPUT);
  pinMode(TRIG3,OUTPUT); pinMode(ECHO3,INPUT);
  digitalWrite(TRIG1,LOW); digitalWrite(TRIG2,LOW); digitalWrite(TRIG3,LOW);
  pinMode(BUZZER,OUTPUT); digitalWrite(BUZZER,LOW);
  pinMode(SOS_BUTTON, INPUT_PULLUP);

  // MP3 on UART1
  mp3Serial.begin(9600,SERIAL_8N1,MP3_RX,MP3_TX);
  delay(1000);
  if(player.begin(mp3Serial)){
    audioReady=true; player.volume(25); Serial.println("MP3 READY");
  } else {
    audioReady=false; Serial.println("MP3 NOT FOUND");
  }

  // GPS on UART2. Larger RX buffer so NMEA data isn't lost during sonar reads.
  gpsSerial.setRxBufferSize(1024);   // must be called before begin()
  gpsSerial.begin(9600, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);

  // Wi-Fi: wait at most 15 s so the stick still works without a hotspot.
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to Wi-Fi Hotspot");
  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 15000) {
    delay(500);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) Serial.println("\nWi-Fi Connected Successfully!");
  else Serial.println("\nWi-Fi not connected yet - will keep retrying in background.");

  Serial.println("SMART BLIND STICK STARTED");
}

// ================= LOOP =================
void loop(){
  serviceBackground();

  // Median-of-3 for all three sonars.
  float s2=readDistanceMedian3(TRIG2,ECHO2); serviceBackground(); serviceDelay(100);
  float s1=readDistanceMedian3(TRIG1,ECHO1); serviceBackground(); serviceDelay(100);
  float s3=readDistanceMedian3(TRIG3,ECHO3); serviceBackground(); serviceDelay(100);

  bool crossedBelow130=false;
  if(s1>=130) warningArmed=true;
  else if(s1>=0 && s1<130 && warningArmed){
    crossedBelow130=true; warningArmed=false;
    requestAudio(1); startShortBeep();
  }
  (void)crossedBelow130;

  // Front logic: 75 cm S1 classification, 250 cm S2 threshold.
  if(s1>=0 && s1<75){
    if(s2>=0 && s2<250) frontState=FRONT_OBSTACLE;
    else frontState=FRONT_UP_STAIR;
  } else if((s1>=130 || s1<0) && s2>=0 && s2<250){
    frontState=FRONT_OBSTACLE;
  } else if(s1>=130 && (s2>=250 || s2<0)){
    frontState=FRONT_CLEAR;
  } else if(s1>=75 && s1<130){
    frontState=FRONT_APPROACH;
  } else frontState=FRONT_UNKNOWN;

  // Ground: <=6 normal, >6 to <=16 down stair, >16 deep drop.
  if(s3<0) groundState=GROUND_UNKNOWN;
  else if(s3<=6) groundState=GROUND_SAFE;
  else if(s3<=16) groundState=GROUND_DOWN_STAIR;
  else groundState=GROUND_DEEP_DROP;

  if(frontState==FRONT_OBSTACLE && previousFrontState!=FRONT_OBSTACLE) requestAudio(2);

  if(frontState==FRONT_UP_STAIR && previousFrontState!=FRONT_UP_STAIR){
    requestAudio(3);
    upStairActive=true; ebarUthoPlayed=false; stairEndCount=0;
  }

  if(groundState==GROUND_DOWN_STAIR && previousGroundState!=GROUND_DOWN_STAIR) requestAudio(4);
  if(groundState==GROUND_DEEP_DROP && previousGroundState!=GROUND_DEEP_DROP) requestAudio(5);

  if(upStairActive && !ebarUthoPlayed && s1>=0 && s1<=20){
    requestAudio(6);
    ebarUthoPlayed=true; stairEndCount=0;
  }

  bool clearFrontNow=(s1>=130 && (s2>=250 || s2<0));
  if(upStairActive && ebarUthoPlayed){
    if(clearFrontNow) stairEndCount++;
    else stairEndCount=0;
    if(stairEndCount>=5){
      requestAudio(7);
      upStairActive=false; ebarUthoPlayed=false; stairEndCount=0;
    }
  }

  // Buzzer only: 130 crossing short beep, S1<=20 continuous, S3>16 continuous.
  bool veryClose=(s1>=0 && s1<=20);
  bool deepDrop=(groundState==GROUND_DEEP_DROP);
  updateBuzzer(veryClose,deepDrop);

  Serial.print("S1: ");
  if(s1<0) Serial.print("TIMEOUT"); else {Serial.print(s1,1);Serial.print(" cm");}
  Serial.print(" | S2: ");
  if(s2<0) Serial.print("TIMEOUT"); else {Serial.print(s2,1);Serial.print(" cm");}
  Serial.print(" | S3: ");
  if(s3<0) Serial.print("TIMEOUT"); else {Serial.print(s3,1);Serial.print(" cm");}

  Serial.print(" | FRONT: ");
  if(frontState==FRONT_CLEAR) Serial.print("CLEAR");
  else if(frontState==FRONT_APPROACH) Serial.print("APPROACH");
  else if(frontState==FRONT_OBSTACLE) Serial.print("OBSTACLE");
  else if(frontState==FRONT_UP_STAIR) Serial.print("UP STAIR");
  else Serial.print("UNKNOWN");

  Serial.print(" | GROUND: ");
  if(groundState==GROUND_SAFE) Serial.print("SAFE");
  else if(groundState==GROUND_DOWN_STAIR) Serial.print("DOWN STAIR");
  else if(groundState==GROUND_DEEP_DROP) Serial.print("DEEP DROP");
  else Serial.print("UNKNOWN");

  Serial.print(" | GPS: ");
  if(gps.location.isValid()){
    Serial.print(gps.location.lat(),6); Serial.print(","); Serial.print(gps.location.lng(),6);
  } else Serial.print("NO FIX");

  Serial.print(" | WiFi: ");
  Serial.println(WiFi.status()==WL_CONNECTED ? "OK" : "DOWN");

  previousFrontState=frontState;
  previousGroundState=groundState;
  serviceBackground();
}
