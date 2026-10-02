#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <TinyGPS++.h>
#include <DFRobotDFPlayerMini.h>

// =====================================================
// WIFI / TELEGRAM CREDENTIALS
// =====================================================
//
// Put the SAME private values from your GPS demo here.
//

const char* WIFI_SSID     = "POCO X2";
const char* WIFI_PASSWORD = "radha9390";

const String BOT_TOKEN =
    "8934570970:AAEht86p_OqLAm0G2DIAWm5Ud9EABfzySXo";

const String CHAT_IDS[] =
{
  "1798782759",
  "946876237"
};

const int NUM_CHATS =
    sizeof(CHAT_IDS) / sizeof(CHAT_IDS[0]);


// =====================================================
// GPS / SOS
// =====================================================

#define GPS_RX_PIN 16
#define GPS_TX_PIN 17

#define SOS_BUTTON 27

TinyGPSPlus gps;

HardwareSerial gpsSerial(2);

WiFiClientSecure secured_client;


// SOS button states

unsigned long buttonPressTime = 0;

bool isButtonPressed = false;

bool alertSent = false;


// =====================================================
// SONAR / AUDIO PINS
// =====================================================

#define S1_TRIG 13
#define S1_ECHO 35

#define S2_TRIG 14
#define S2_ECHO 34

#define S3_TRIG 25
#define S3_ECHO 33

#define BUZZER 26

#define MP3_RX 21
#define MP3_TX 22


// =====================================================
// THRESHOLDS
// =====================================================

// First warning
const float WARNING_DISTANCE = 130.0;

// Below this:
// obstacle vs upward staircase
const float CLASSIFY_DISTANCE = 75.0;

// Close obstacle / step
const float CLOSE_DISTANCE = 20.0;

// After Ebar Utho,
// S1 must move beyond this before next step
const float STEP_REARM_DISTANCE = 25.0;

// Difference = S2 - S1
const float DIFF_THRESHOLD = 72.0;


// S3 thresholds

const float SAFE_GROUND_LIMIT = 10.0;

const float DOWN_STAIR_LIMIT = 25.0;


// Upward stair finish

const int UP_STAIR_END_REQUIRED = 2;


// Sabdhan confirmation

const unsigned long WARNING_CONFIRM_TIME = 500;

const float WARNING_MIN_DECREASE = 1.0;


// Give 0003 some time before first 0006

const unsigned long UP_INTRO_TIME = 1800;


// =====================================================
// DFPLAYER
// =====================================================

HardwareSerial mp3Serial(1);

DFRobotDFPlayerMini player;

bool audioReady = false;

int currentTrack = 0;


// =====================================================
// FRONT STATES
// =====================================================

enum FrontState
{
  FRONT_UNKNOWN,
  FRONT_CLEAR,
  FRONT_APPROACH,
  FRONT_OBSTACLE,
  FRONT_UP_STAIR
};

FrontState frontState =
    FRONT_UNKNOWN;


// =====================================================
// GROUND STATES
// =====================================================

enum GroundState
{
  GROUND_UNKNOWN,
  GROUND_SAFE,
  GROUND_DOWN_STAIR,
  GROUND_DEEP_DROP
};

GroundState groundState =
    GROUND_UNKNOWN;


// =====================================================
// HAZARD STATES
// =====================================================

enum HazardState
{
  HAZARD_NONE,
  HAZARD_OBSTACLE,
  HAZARD_UP_STAIR,
  HAZARD_DOWN_STAIR,
  HAZARD_DEEP_DROP
};

HazardState lastHazard =
    HAZARD_NONE;


// =====================================================
// FIRST WARNING
// =====================================================

bool warningArmed = true;

bool warningCheckActive = false;

unsigned long warningCheckStart = 0;

float warningStartDistance = 0;


// =====================================================
// UPWARD STAIR
// =====================================================

bool upStairActive = false;

bool stepPromptArmed = true;

int upStairEndCount = 0;

unsigned long upStairStartTime = 0;


// =====================================================
// SHORT BEEP
// =====================================================

bool shortBeepActive = false;

unsigned long shortBeepStart = 0;


// =====================================================
// GPS SERVICE
// =====================================================

void serviceGPS()
{
  while (gpsSerial.available() > 0)
  {
    gps.encode(
        gpsSerial.read()
    );
  }
}


// =====================================================
// AUDIO STATUS
// =====================================================

void updateAudioStatus()
{
  if (!audioReady)
  {
    return;
  }


  if (player.available())
  {
    uint8_t type =
        player.readType();

    int value =
        player.read();

    (void)value;


    if (type == DFPlayerPlayFinished)
    {
      currentTrack = 0;
    }
  }
}


// =====================================================
// BACKGROUND WAIT
// =====================================================
//
// Replaces ordinary delay(100).
//
// During the 100 ms sonar gap,
// GPS data still gets processed.
// =====================================================

void backgroundWait(
    unsigned long waitMs)
{
  unsigned long start =
      millis();


  while (
      millis() - start <
      waitMs)
  {
    serviceGPS();

    updateAudioStatus();

    delay(2);
  }
}


// =====================================================
// WIFI FOR SOS
// =====================================================

bool ensureWiFiForSOS(
    unsigned long timeoutMs)
{
  if (
      WiFi.status() ==
      WL_CONNECTED)
  {
    return true;
  }


  Serial.println(
      "Wi-Fi disconnected. Reconnecting for SOS..."
  );


  // Stop the background attempt first; otherwise ESP-IDF refuses the new
  // config ("sta is connecting, cannot set config").
  WiFi.disconnect();
  delay(100);

  WiFi.begin(
      WIFI_SSID,
      WIFI_PASSWORD
  );


  unsigned long start =
      millis();


  while (
      WiFi.status() != WL_CONNECTED &&
      millis() - start < timeoutMs)
  {
    serviceGPS();

    delay(100);
  }


  if (
      WiFi.status() ==
      WL_CONNECTED)
  {
    Serial.println(
        "Wi-Fi connected for SOS."
    );

    return true;
  }


  Serial.println(
      "Wi-Fi connection failed."
  );

  return false;
}


// =====================================================
// TELEGRAM SOS
// =====================================================

String urlEncode(const String& s)
{
  static const char hex[] = "0123456789ABCDEF";
  String out;
  out.reserve(s.length() * 3);
  for (size_t i = 0; i < s.length(); i++)
  {
    uint8_t c = (uint8_t)s[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
    {
      out += (char)c;
    }
    else
    {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 0x0F];
    }
  }
  return out;
}


// One HTTPS POST. Returns HTTP status (200 = accepted) or a negative
// HTTPClient error code (connection / TLS / timeout failure).
int telegramSendOnce(const String& chatId, const String& text, String& resp)
{
  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(15);

  HTTPClient http;
  http.setConnectTimeout(10000);
  http.setTimeout(10000);

  String url = "https://api.telegram.org/bot" + BOT_TOKEN + "/sendMessage";
  if (!http.begin(client, url))
  {
    resp = "http.begin() failed";
    return -100;
  }
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");

  String body = "chat_id=" + chatId + "&text=" + urlEncode(text);
  int code = http.POST(body);
  if (code > 0) resp = http.getString();
  else          resp = HTTPClient::errorToString(code);
  http.end();
  return code;
}


void sendTelegramSOS(
    double lat,
    double lng,
    bool hasLock)
{
  if (!ensureWiFiForSOS(15000))
  {
    Serial.println(
        "SOS NOT SENT: Wi-Fi unavailable."
    );

    return;
  }

  Serial.printf("[WiFi] RSSI %d dBm\n", WiFi.RSSI());

  String message =
      "EMERGENCY ALERT!\n"
      "Blind stick user needs help!\n\n";

  if (hasLock)
  {
    message += "Live Location: https://maps.google.com/?q=";
    message += String(lat, 6);
    message += ",";
    message += String(lng, 6);
  }
  else
  {
    message += "GPS satellite lock pending...";
  }

  int delivered = 0;

  for (int i = 0; i < NUM_CHATS; i++)
  {
    for (int attempt = 1; attempt <= 3; attempt++)
    {
      serviceGPS();

      String resp;
      int code = telegramSendOnce(CHAT_IDS[i], message, resp);
      bool ok = (code == 200) && (resp.indexOf("\"ok\":true") >= 0);

      if (ok)
      {
        Serial.printf("SOS DELIVERED to contact %d (attempt %d)\n", i + 1, attempt);
        delivered++;
        break;
      }

      Serial.printf("SOS FAILED for contact %d, attempt %d - HTTP %d: %s\n",
                    i + 1, attempt, code, resp.c_str());

      // 400/401/403 = wrong chat ID / token / bot not started: retry won't help
      bool retryable = (code < 0) || (code == 429) || (code >= 500);
      if (!retryable || attempt == 3) break;
      if (!ensureWiFiForSOS(8000)) break;
      delay(1000UL * attempt);
    }
  }

  Serial.printf("SOS delivered to %d of %d contacts\n", delivered, NUM_CHATS);
}


// =====================================================
// SOS BUTTON
// =====================================================
//
// GPIO27 -> push button -> GND
//
// INPUT_PULLUP
//
// Hold 2 seconds to send SOS.
// =====================================================

void updateSOSButton()
{
  bool buttonState =
      (
          digitalRead(
              SOS_BUTTON) ==
          LOW
      );


  // Button just pressed

  if (
      buttonState &&
      !isButtonPressed)
  {
    isButtonPressed = true;

    buttonPressTime =
        millis();


    Serial.println(
        "SOS button pressed. Hold for 2 seconds..."
    );
  }


  // Button being held

  else if (
      buttonState &&
      isButtonPressed)
  {
    if (
        !alertSent &&
        millis() -
            buttonPressTime >=
            2000)
    {
      Serial.println(
          "SOS confirmed. Sending Telegram alert..."
      );


      serviceGPS();


      if (
          gps.location.isValid())
      {
        sendTelegramSOS(
            gps.location.lat(),
            gps.location.lng(),
            true
        );
      }

      else
      {
        sendTelegramSOS(
            0.0,
            0.0,
            false
        );
      }


      alertSent = true;
    }
  }


  // Button released

  else
  {
    if (isButtonPressed)
    {
      Serial.println(
          "SOS button released."
      );
    }


    isButtonPressed = false;

    alertSent = false;
  }
}


// =====================================================
// SONAR
// =====================================================

float readDistance(
    int trigPin,
    int echoPin)
{
  digitalWrite(
      trigPin,
      LOW
  );

  delayMicroseconds(2);


  digitalWrite(
      trigPin,
      HIGH
  );

  delayMicroseconds(10);


  digitalWrite(
      trigPin,
      LOW
  );


  unsigned long duration =
      pulseIn(
          echoPin,
          HIGH,
          30000
      );


  if (duration == 0)
  {
    return -1;
  }


  return
      duration *
      0.0343 /
      2.0;
}


// =====================================================
// PLAY AUDIO IMMEDIATELY
// =====================================================

void playNow(int track)
{
  if (!audioReady)
  {
    Serial.println(
        "AUDIO NOT READY"
    );

    return;
  }


  // No old-message queue

  player.stop();

  delay(40);


  player.playMp3Folder(
      track
  );


  currentTrack =
      track;


  Serial.print(
      ">>> PLAYING 000"
  );

  Serial.print(
      track
  );

  Serial.println(
      ".mp3"
  );
}


// =====================================================
// SHORT BEEP
// =====================================================

void startShortBeep()
{
  shortBeepActive =
      true;

  shortBeepStart =
      millis();
}


// =====================================================
// BUZZER
// =====================================================

void updateBuzzer(
    bool closeObstacle,
    bool deepDrop)
{
  /*
     BUZZER ONLY:

     1. 130 cm Sabdhan
        -> short beep

     2. obstacle <=20 cm
        -> continuous

     3. deep drop
        -> continuous

     SOS DOES NOT use buzzer.
  */


  if (
      closeObstacle ||
      deepDrop)
  {
    digitalWrite(
        BUZZER,
        HIGH
    );

    return;
  }


  // Sabdhan short beep

  if (shortBeepActive)
  {
    if (
        millis() -
            shortBeepStart <
        200)
    {
      digitalWrite(
          BUZZER,
          HIGH
      );
    }

    else
    {
      shortBeepActive =
          false;

      digitalWrite(
          BUZZER,
          LOW
      );
    }


    return;
  }


  digitalWrite(
      BUZZER,
      LOW
  );
}


// =====================================================
// SETUP
// =====================================================

void setup()
{
  Serial.begin(115200);

  delay(300);


  // =================================================
  // GPS
  // =================================================

  gpsSerial.begin(
      9600,
      SERIAL_8N1,
      GPS_RX_PIN,
      GPS_TX_PIN
  );


  // =================================================
  // SOS BUTTON
  // =================================================

  pinMode(
      SOS_BUTTON,
      INPUT_PULLUP
  );


  // =================================================
  // WIFI
  // =================================================
  //
  // Starts connection in background.
  //
  // Sonar system does NOT wait forever
  // if hotspot is unavailable.
  // =================================================

  WiFi.mode(
      WIFI_STA
  );

  WiFi.setSleep(false);         // modem sleep makes TLS flaky
  WiFi.setAutoReconnect(true);


  WiFi.begin(
      WIFI_SSID,
      WIFI_PASSWORD
  );


  // =================================================
  // SONARS
  // =================================================

  pinMode(
      S1_TRIG,
      OUTPUT
  );

  pinMode(
      S1_ECHO,
      INPUT
  );


  pinMode(
      S2_TRIG,
      OUTPUT
  );

  pinMode(
      S2_ECHO,
      INPUT
  );


  pinMode(
      S3_TRIG,
      OUTPUT
  );

  pinMode(
      S3_ECHO,
      INPUT
  );


  digitalWrite(
      S1_TRIG,
      LOW
  );

  digitalWrite(
      S2_TRIG,
      LOW
  );

  digitalWrite(
      S3_TRIG,
      LOW
  );


  // =================================================
  // BUZZER
  // =================================================

  pinMode(
      BUZZER,
      OUTPUT
  );


  digitalWrite(
      BUZZER,
      LOW
  );


  // =================================================
  // DFPLAYER
  // =================================================

  mp3Serial.begin(
      9600,
      SERIAL_8N1,
      MP3_RX,
      MP3_TX
  );


  delay(1200);


  if (
      player.begin(
          mp3Serial))
  {
    audioReady =
        true;


    player.volume(
        25
    );


    Serial.println(
        "DFPLAYER READY"
    );
  }

  else
  {
    audioReady =
        false;


    Serial.println(
        "DFPLAYER NOT FOUND"
    );
  }


  Serial.println();

  Serial.println(
      "SMART BLIND STICK STARTED"
  );

  Serial.println(
      "GPS RX=16 TX=17"
  );

  Serial.println(
      "SOS BUTTON=GPIO27"
  );

  Serial.println(
      "Hold SOS for 2 seconds"
  );


  if (
      WiFi.status() ==
      WL_CONNECTED)
  {
    Serial.println(
        "Wi-Fi CONNECTED"
    );
  }

  else
  {
    Serial.println(
        "Wi-Fi connecting in background"
    );
  }


  Serial.println(
      "-----------------------------------------"
  );
}


// =====================================================
// LOOP
// =====================================================

void loop()
{
  // =================================================
  // GPS + SOS
  // =================================================

  serviceGPS();

  updateSOSButton();

  updateAudioStatus();


  // =================================================
  // READ S1
  // =================================================

  float s1 =
      readDistance(
          S1_TRIG,
          S1_ECHO
      );


  updateAudioStatus();

  serviceGPS();

  updateSOSButton();

  backgroundWait(100);


  // =================================================
  // READ S2
  // =================================================

  float s2 =
      readDistance(
          S2_TRIG,
          S2_ECHO
      );


  updateAudioStatus();

  serviceGPS();

  updateSOSButton();

  backgroundWait(100);


  // =================================================
  // READ S3
  // =================================================

  float s3 =
      readDistance(
          S3_TRIG,
          S3_ECHO
      );


  updateAudioStatus();

  serviceGPS();

  updateSOSButton();

  backgroundWait(100);


  // =================================================
  // 130 CM SABDHAN
  // =================================================

  bool warningTriggeredNow =
      false;


  if (!upStairActive)
  {
    // Outside warning distance

    if (
        s1 >=
        WARNING_DISTANCE)
    {
      warningArmed =
          true;

      warningCheckActive =
          false;
    }


    // Inside warning zone

    else if (
        s1 >= 0 &&
        s1 <
            WARNING_DISTANCE &&
        warningArmed)
    {
      // Start 0.5 sec observation

      if (
          !warningCheckActive)
      {
        warningCheckActive =
            true;


        warningCheckStart =
            millis();


        warningStartDistance =
            s1;


        Serial.print(
            "WARNING CHECK START: "
        );

        Serial.println(
            warningStartDistance
        );
      }


      // After 0.5 sec

      else if (
          millis() -
              warningCheckStart >=
          WARNING_CONFIRM_TIME)
      {
        float decrease =
            warningStartDistance -
            s1;


        // Distance decreasing

        if (
            s1 <
                WARNING_DISTANCE &&
            decrease >=
                WARNING_MIN_DECREASE)
        {
          // 0001 = সাবধান

          playNow(1);


          startShortBeep();


          warningArmed =
              false;

          warningCheckActive =
              false;

          warningTriggeredNow =
              true;


          Serial.println(
              "SABDHAN"
          );
        }


        // Not approaching yet

        else
        {
          warningCheckStart =
              millis();


          warningStartDistance =
              s1;
        }
      }
    }


    else if (
        s1 < 0)
    {
      warningCheckActive =
          false;
    }
  }


  else
  {
    warningCheckActive =
        false;
  }


  // =================================================
  // FRONT CLASSIFICATION
  // =================================================

  float difference =
      -999;


  /*
     130 cm:
       warning only

     75 - 130:
       approach only

     S1 <75:
       obstacle vs upward stair
  */


  if (
      s1 >= 0 &&
      s1 <
          CLASSIFY_DISTANCE &&
      s2 >= 0)
  {
    difference =
        s2 - s1;


    // Upward stair

    if (
        difference >=
        DIFF_THRESHOLD)
    {
      frontState =
          FRONT_UP_STAIR;
    }


    // Obstacle

    else
    {
      frontState =
          FRONT_OBSTACLE;
    }
  }


  // 75 - 130 cm

  else if (
      s1 >=
          CLASSIFY_DISTANCE &&
      s1 <
          WARNING_DISTANCE)
  {
    frontState =
        FRONT_APPROACH;
  }


  // >=130 cm

  else if (
      s1 >=
      WARNING_DISTANCE)
  {
    frontState =
        FRONT_CLEAR;
  }


  else
  {
    frontState =
        FRONT_UNKNOWN;
  }


  // =================================================
  // UPWARD STAIR ACTIVE
  // =================================================

  if (upStairActive)
  {
    // Ignore S3 during upward stair

    groundState =
        GROUND_SAFE;


    // =================================================
    // NEXT STEP RE-ARM
    // =================================================

    if (
        !stepPromptArmed &&
        s1 >= 0 &&
        s1 >
            STEP_REARM_DISTANCE)
    {
      stepPromptArmed =
          true;


      Serial.println(
          "NEXT STEP ARMED"
      );
    }


    // =================================================
    // EBAR UTHO
    // =================================================

    /*
       At <=20 cm:

       Difference >=72
       -> still upward stair
       -> Ebar Utho

       Difference <72
       -> obstacle
       -> NO Ebar Utho
       -> obstacle buzzer
    */


    if (
        stepPromptArmed &&
        s1 >= 0 &&
        s1 <=
            CLOSE_DISTANCE &&
        s2 >= 0 &&
        millis() -
            upStairStartTime >=
            UP_INTRO_TIME)
    {
      float currentDifference =
          s2 - s1;


      if (
          currentDifference >=
          DIFF_THRESHOLD)
      {
        // 0006 = এবার উঠো

        playNow(6);


        stepPromptArmed =
            false;


        Serial.println(
            "EBAR UTHO"
        );
      }


      else
      {
        Serial.println(
            "OBSTACLE AT 20 CM - NO EBAR UTHO"
        );
      }
    }


    // =================================================
    // FAST UPWARD STAIR FINISH
    // =================================================

    if (
        s1 >=
        CLASSIFY_DISTANCE)
    {
      upStairEndCount++;
    }

    else
    {
      upStairEndCount =
          0;
    }


    if (
        upStairEndCount >=
        UP_STAIR_END_REQUIRED)
    {
      // 0007 = সিঁড়ি শেষ

      playNow(7);


      Serial.println(
          "UPWARD SHIRI SHESH"
      );


      upStairActive =
          false;

      stepPromptArmed =
          true;

      upStairEndCount =
          0;

      lastHazard =
          HAZARD_NONE;


      warningArmed =
          false;

      warningCheckActive =
          false;
    }
  }


  // =================================================
  // NORMAL MODE
  // =================================================

  else
  {
    // =================================================
    // S3 CLASSIFICATION
    // =================================================

    if (
        s3 < 0)
    {
      groundState =
          GROUND_UNKNOWN;
    }


    else if (
        s3 <=
        SAFE_GROUND_LIMIT)
    {
      groundState =
          GROUND_SAFE;
    }


    else if (
        s3 <=
        DOWN_STAIR_LIMIT)
    {
      groundState =
          GROUND_DOWN_STAIR;
    }


    else
    {
      groundState =
          GROUND_DEEP_DROP;
    }


    // =================================================
    // CURRENT HAZARD
    // =================================================

    HazardState newHazard =
        HAZARD_NONE;


    // Priority:
    // 1 Up stair
    // 2 Deep drop
    // 3 Obstacle
    // 4 Down stair


    if (
        frontState ==
        FRONT_UP_STAIR)
    {
      newHazard =
          HAZARD_UP_STAIR;
    }


    else if (
        groundState ==
        GROUND_DEEP_DROP)
    {
      newHazard =
          HAZARD_DEEP_DROP;
    }


    else if (
        frontState ==
        FRONT_OBSTACLE)
    {
      newHazard =
          HAZARD_OBSTACLE;
    }


    else if (
        groundState ==
        GROUND_DOWN_STAIR)
    {
      newHazard =
          HAZARD_DOWN_STAIR;
    }


    else
    {
      newHazard =
          HAZARD_NONE;
    }


    // =================================================
    // NEW HAZARD AUDIO
    // =================================================

    if (
        newHazard !=
            lastHazard &&
        !warningTriggeredNow)
    {
      // -----------------------------------------------
      // UPWARD STAIR
      // -----------------------------------------------

      if (
          newHazard ==
          HAZARD_UP_STAIR)
      {
        // 0003

        playNow(3);


        upStairActive =
            true;

        stepPromptArmed =
            true;

        upStairEndCount =
            0;

        upStairStartTime =
            millis();


        Serial.println(
            "UP STAIR STARTED"
        );
      }


      // -----------------------------------------------
      // DEEP DROP
      // -----------------------------------------------

      else if (
          newHazard ==
          HAZARD_DEEP_DROP)
      {
        // 0005

        playNow(5);
      }


      // -----------------------------------------------
      // OBSTACLE
      // -----------------------------------------------

      else if (
          newHazard ==
          HAZARD_OBSTACLE)
      {
        // 0002

        playNow(2);
      }


      // -----------------------------------------------
      // DOWNWARD STAIR
      // -----------------------------------------------

      else if (
          newHazard ==
          HAZARD_DOWN_STAIR)
      {
        // 0004

        playNow(4);

        // NO BUZZER
        // NO Shiri Shesh
      }


      lastHazard =
          newHazard;
    }
  }


  // =================================================
  // BUZZER
  // =================================================

  /*
     BUZZER CASE 1:
     Sabdhan short beep.


     BUZZER CASE 2:
     obstacle <=20 cm.
  */


  bool closeObstacle =
      (
          frontState ==
              FRONT_OBSTACLE &&
          s1 >= 0 &&
          s1 <=
              CLOSE_DISTANCE
      );


  /*
     BUZZER CASE 3:
     deep drop.
  */


  bool deepDrop =
      (
          !upStairActive &&
          groundState ==
              GROUND_DEEP_DROP
      );


  updateBuzzer(
      closeObstacle,
      deepDrop
  );


  // =================================================
  // SERIAL MONITOR
  // =================================================

  Serial.print(
      "S1: "
  );


  if (
      s1 < 0)
  {
    Serial.print(
        "TIMEOUT"
    );
  }

  else
  {
    Serial.print(
        s1,
        1
    );

    Serial.print(
        " cm"
    );
  }


  Serial.print(
      " | S2: "
  );


  if (
      s2 < 0)
  {
    Serial.print(
        "TIMEOUT"
    );
  }

  else
  {
    Serial.print(
        s2,
        1
    );

    Serial.print(
        " cm"
    );
  }


  Serial.print(
      " | S3: "
  );


  if (
      s3 < 0)
  {
    Serial.print(
        "TIMEOUT"
    );
  }

  else
  {
    Serial.print(
        s3,
        1
    );

    Serial.print(
        " cm"
    );
  }


  // Difference

  Serial.print(
      " | Diff: "
  );


  if (
      s1 >= 0 &&
      s2 >= 0)
  {
    Serial.print(
        s2 - s1,
        1
    );

    Serial.print(
        " cm"
    );
  }

  else
  {
    Serial.print(
        "---"
    );
  }


  // Front

  Serial.print(
      " | FRONT: "
  );


  if (
      frontState ==
      FRONT_CLEAR)
  {
    Serial.print(
        "CLEAR"
    );
  }


  else if (
      frontState ==
      FRONT_APPROACH)
  {
    Serial.print(
        "APPROACH"
    );
  }


  else if (
      frontState ==
      FRONT_OBSTACLE)
  {
    Serial.print(
        "OBSTACLE"
    );
  }


  else if (
      frontState ==
      FRONT_UP_STAIR)
  {
    Serial.print(
        "UP STAIR"
    );
  }


  else
  {
    Serial.print(
        "UNKNOWN"
    );
  }


  // Up stair

  Serial.print(
      " | UP: "
  );


  if (upStairActive)
  {
    Serial.print(
        "YES"
    );
  }

  else
  {
    Serial.print(
        "NO"
    );
  }


  // Step

  Serial.print(
      " | STEP: "
  );


  if (stepPromptArmed)
  {
    Serial.print(
        "ARMED"
    );
  }

  else
  {
    Serial.print(
        "WAIT"
    );
  }


  // Ground

  Serial.print(
      " | GROUND: "
  );


  if (upStairActive)
  {
    Serial.print(
        "IGNORED"
    );
  }


  else if (
      groundState ==
      GROUND_SAFE)
  {
    Serial.print(
        "SAFE"
    );
  }


  else if (
      groundState ==
      GROUND_DOWN_STAIR)
  {
    Serial.print(
        "DOWN STAIR"
    );
  }


  else if (
      groundState ==
      GROUND_DEEP_DROP)
  {
    Serial.print(
        "DEEP DROP"
    );
  }


  else
  {
    Serial.print(
        "UNKNOWN"
    );
  }


  // =================================================
  // GPS STATUS
  // =================================================

  Serial.print(
      " | GPS: "
  );


  if (
      gps.location.isValid())
  {
    Serial.print(
        gps.location.lat(),
        6
    );

    Serial.print(
        ","
    );

    Serial.print(
        gps.location.lng(),
        6
    );
  }

  else
  {
    Serial.print(
        "NO FIX"
    );
  }


  // =================================================
  // WIFI STATUS
  // =================================================

  Serial.print(
      " | WIFI: "
  );


  if (
      WiFi.status() ==
      WL_CONNECTED)
  {
    Serial.println(
        "CONNECTED"
    );
  }

  else
  {
    Serial.println(
        "OFFLINE"
    );
  }


  // Keep GPS/SOS active

  serviceGPS();

  updateSOSButton();
}