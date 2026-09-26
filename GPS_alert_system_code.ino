#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <TinyGPS++.h>

// --- PIN DEFINITIONS ---
const int PIN_TRIG_FRONT = 32;
const int PIN_ECHO_FRONT = 33;
const int PIN_TRIG_STAIR = 25; 
const int PIN_ECHO_STAIR = 26;
const int PIN_MOISTURE   = 34; 
const int PIN_BUZZER     = 2;
const int PIN_MOTOR      = 21;
const int PIN_BUTTON     = 27;

// GPS Pins (Hardware Serial 2)
#define GPS_RX_PIN 16
#define GPS_TX_PIN 17

// --- CREDENTIALS ---
const char* WIFI_SSID     = "Chowdhury";
const char* WIFI_PASSWORD = "52446661";
const String BOT_TOKEN    = "8934570970:AAEht86p_OqLAm0G2DIAWm5Ud9EABfzySXo";

// Array of individual Family Chat IDs
const String CHAT_IDS[] = {"1798782759", "7499117589", "8431741319" };
const int NUM_CHATS = 3;
// --- GLOBAL OBJECTS ---
TinyGPSPlus gps;
HardwareSerial gpsSerial(2);
WiFiClientSecure secured_client;

// --- STATE VARIABLES ---
unsigned long lastSensorCheck = 0;
unsigned long buttonPressTime = 0;
bool isButtonPressed = false;
bool alertSent = false;
unsigned long lastAlertToggle = 0;
bool alertState = false;
int currentBeepInterval = 0; 

// --- ULTRASONIC SENSOR FUNCTION ---
long readDistance(int trigPin, int echoPin) {
  digitalWrite(trigPin, LOW);
  delayMicroseconds(2);
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);

  long duration = pulseIn(echoPin, HIGH, 25000); // 25ms timeout
  if (duration == 0) return 400; // Return max distance if no ping
  return (duration * 0.0343) / 2;
}

// --- TELEGRAM SOS FUNCTION ---
// --- TELEGRAM SOS FUNCTION ---
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

  // Loop through the array and open a fresh connection for each person
  for (int i = 0; i < NUM_CHATS; i++) {
    secured_client.setInsecure(); // Bypass TLS validation
    
    if (!secured_client.connect("api.telegram.org", 443)) {
      Serial.println("Failed to connect to Telegram for Member " + String(i + 1));
      continue; // If it fails, skip to the next family member
    }

    String url = "/bot" + BOT_TOKEN + "/sendMessage?chat_id=" + CHAT_IDS[i] + "&text=" + message;
    url.replace(" ", "%20");
    url.replace("\n", "%0A");

    // Send the message and tell the server to close this specific connection
    secured_client.print(String("GET ") + url + " HTTP/1.1\r\n" +
                         "Host: api.telegram.org\r\n" +
                         "Connection: close\r\n\r\n");
                         
    Serial.println("SOS Message Sent to Family Member " + String(i + 1));
    
    delay(500); // Give the server half a second to process it
    secured_client.stop(); // Fully close the connection to prepare for the next loop
  }
}
  
  // Wait 2 seconds for the Serial Monitor to catch up
  void setup() {
  Serial.begin(115200);
  delay(2000); 
  Serial.println("\n\n====================================");
  Serial.println("   SMART BLIND STICK BOOTING UP!    ");
  Serial.println("====================================\n");

  gpsSerial.begin(9600, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);

  pinMode(PIN_TRIG_FRONT, OUTPUT);
  pinMode(PIN_ECHO_FRONT, INPUT);
  pinMode(PIN_TRIG_STAIR, OUTPUT);
  pinMode(PIN_ECHO_STAIR, INPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_MOTOR, OUTPUT);
  pinMode(PIN_BUTTON, INPUT_PULLUP);

  digitalWrite(PIN_BUZZER, LOW);
  digitalWrite(PIN_MOTOR, LOW);

  // Connect to Wi-Fi
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to Wi-Fi Hotspot");
  
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWi-Fi Connected Successfully!");
}

void loop() {
 // 1. Process GPS Data
  while (gpsSerial.available() > 0) {
    char c = gpsSerial.read();
    Serial.write(c); // <--- This line prints the raw GPS data to your screen
    gps.encode(c);
  }

 // 2. Read Emergency Button (Hold for 2 seconds to avoid accidental presses)
  bool buttonState = (digitalRead(PIN_BUTTON) == LOW);
  
  if (buttonState && !isButtonPressed) {
    Serial.println("Wire connected! Timer started..."); // <--- DEBUG LINE
    isButtonPressed = true;
    buttonPressTime = millis();
  } else if (buttonState && isButtonPressed) {
    if (!alertSent && (millis() - buttonPressTime >= 2000)) {
      Serial.println("2 seconds reached! Sending to Telegram..."); // <--- DEBUG LINE
      digitalWrite(PIN_BUZZER, HIGH); 
      delay(300);
      digitalWrite(PIN_BUZZER, LOW);

      if (gps.location.isValid()) {
        sendTelegramSOS(gps.location.lat(), gps.location.lng(), true);
      } else {
        sendTelegramSOS(0.0, 0.0, false);
      }
      alertSent = true;
    }
  } else if (!buttonState) {
    if (isButtonPressed) {
      Serial.println("Wire disconnected."); // <--- DEBUG LINE
    }
    isButtonPressed = false;
    alertSent = false;
  }

  // 3. Scan Sensors every 100ms
  if (millis() - lastSensorCheck >= 100) {
    lastSensorCheck = millis();

    long distFront = readDistance(PIN_TRIG_FRONT, PIN_ECHO_FRONT);
    long distStair = readDistance(PIN_TRIG_STAIR, PIN_ECHO_STAIR);
    int moistureRaw = analogRead(PIN_MOISTURE);

    // Hazard Logic Evaluation
    if (distStair > 85) {
      currentBeepInterval = 80;  // Drop-off/Down-stair: Fast double pulse
    } else if (distStair < 25 && distFront > 60) {
      currentBeepInterval = 150; // Step Up / Curb: Medium pulse
    } else if (moistureRaw < 2500) {
      currentBeepInterval = 250; // Mud/Water: Slower rhythm
    } else if (distFront < 30) {
      currentBeepInterval = -1;  // Direct obstacle: Continuous beep
    } else if (distFront < 80) {
      currentBeepInterval = map(distFront, 30, 80, 100, 500); // Proportional warning
    } else {
      currentBeepInterval = 0;   // Clear path
    }
  }

  // 4. Non-Blocking Feedback Output
  if (currentBeepInterval == -1) {
    digitalWrite(PIN_BUZZER, HIGH);
    digitalWrite(PIN_MOTOR, HIGH);
  } else if (currentBeepInterval > 0) {
    if (millis() - lastAlertToggle >= (unsigned long)currentBeepInterval) {
      lastAlertToggle = millis();
      alertState = !alertState;
      digitalWrite(PIN_BUZZER, alertState ? HIGH : LOW);
      digitalWrite(PIN_MOTOR, alertState ? HIGH : LOW);
    }
  } else {
    digitalWrite(PIN_BUZZER, LOW);
    digitalWrite(PIN_MOTOR, LOW);
  }
}