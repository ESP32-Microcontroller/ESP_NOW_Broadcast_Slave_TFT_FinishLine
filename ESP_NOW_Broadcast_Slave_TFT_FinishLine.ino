#include "ESP32_NOW.h"
#include "WiFi.h"
#include <esp_mac.h>  // For the MAC2STR and MACSTR macros
#include <vector>
#include <SPI.h>
#include <Wire.h>
#include <ezButton.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>

long startTimeMillis;
bool gateOpen = true;
TFT_eSPI tft = TFT_eSPI();

/* Definitions */
#define ESPNOW_WIFI_CHANNEL 6

#define SCL0_Pin 19
#define SDA0_Pin 20

#define AT_GATE 0
#define RACING 1
#define FINISHED 2
#define RELOADING 3
#define LANES 2

// Touchscreen pins
#define XPT2046_IRQ 48   // T_IRQ
#define XPT2046_MOSI 21  // T_DIN
#define XPT2046_MISO 47  // T_OUT
#define XPT2046_CLK 37   // T_CLK
#define XPT2046_CS 38    // T_CS

SPIClass touchscreenSPI = SPIClass(VSPI);
XPT2046_Touchscreen touchscreen(XPT2046_CS, XPT2046_IRQ);

#define SCREEN_WIDTH 320
#define SCREEN_HEIGHT 240
#define FONT_SIZE 4

int laneStatus[LANES];
long elapsedTime[LANES];
int breakBeamPin[LANES] = {6, 7};
int finishLineLED[LANES] = {9, 10};
int readyLED = 11;
int raceActiveLED = 8;
bool scoresReported = false;
bool allFinished = false;
bool allAtGate = false;
bool commEstablished = false;
ezButton resetSwitch(3);
int finishedRacerCount;
int heatNumber = 0;

// Touchscreen coordinates: (x, y) and pressure (z)
int x, y, z;

// Print Touchscreen info about X, Y and Pressure (Z) on the Serial Monitor
void printTouchToSerial(int touchX, int touchY, int touchZ) {
  Serial.print("X = ");
  Serial.print(touchX);
  Serial.print(" | Y = ");
  Serial.print(touchY);
  Serial.print(" | Pressure = ");
  Serial.print(touchZ);
  Serial.println();
}

// Print Touchscreen info about X, Y and Pressure (Z) on the TFT Display
void printTouchToDisplay(int touchX, int touchY, int touchZ) {
  // Clear TFT screen
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);

  int centerX = SCREEN_WIDTH / 2;
  int textY = 80;
 
  String tempText = "X = " + String(touchX);
  tft.drawCentreString(tempText, centerX, textY, FONT_SIZE);

  textY += 20;
  tempText = "Y = " + String(touchY);
  tft.drawCentreString(tempText, centerX, textY, FONT_SIZE);

  textY += 20;
  tempText = "Pressure = " + String(touchZ);
  tft.drawCentreString(tempText, centerX, textY, FONT_SIZE);
}

void displayToggleGate() {
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawCentreString("Toggle start gate to establish connection. ", 160, 20, 2);
}

void displayReady() {
  tft.setTextColor(TFT_RED, TFT_BLACK);
  tft.drawString("Ready... ", 45, 30, FONT_SIZE);
}

void displaySet() {
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawString("Set...", 150, 30, FONT_SIZE);
}

void displayGo() {
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.drawString("Go!", 225, 30, FONT_SIZE);
}

void displayHeatNumber() {
  tft.setTextColor(TFT_SKYBLUE, TFT_BLACK);
  char buffer[32];
  sprintf(buffer, "Heat: %1d", heatNumber);
  tft.drawString(buffer, 45, 70, FONT_SIZE);
}

void displayLaneTime(int lane) {
  char buffer[32];
  sprintf(buffer, "Lane %1d: %7.3fs", (lane+1), (elapsedTime[lane] / 1000.0));
  tft.setTextColor(TFT_SKYBLUE, TFT_BLACK);
  tft.drawString(buffer, 60, 70 + (finishedRacerCount * 30), FONT_SIZE);
}


// Creating a new class that inherits from the ESP_NOW_Peer class is required.
class ESP_NOW_Peer_Class : public ESP_NOW_Peer {
public:
  // Constructor of the class
  ESP_NOW_Peer_Class(const uint8_t *mac_addr, uint8_t channel, wifi_interface_t iface, const uint8_t *lmk) : ESP_NOW_Peer(mac_addr, channel, iface, lmk) {}

  // Destructor of the class
  ~ESP_NOW_Peer_Class() {}

  // Function to register the master peer
  bool add_peer() {
    if (!add()) {
      log_e("Failed to register the broadcast peer");
      return false;
    }
    return true;
  }

  // Function to print the received messages from the master
  void onReceive(const uint8_t *data, size_t len, bool broadcast) {
    Serial.printf("Received a message from master " MACSTR " (%s)\n", MAC2STR(addr()), broadcast ? "broadcast" : "unicast");
    Serial.printf("  Message: %s\n", (char *)data);

    char *buffer = (char*)data;

    if ((strcmp(buffer, "START_GATE_OPENED") == 0) && (allFinished == false)) {
      digitalWrite(readyLED, LOW);
      startTimeMillis = millis();
      gateOpen = true;
      finishedRacerCount = 0;
      scoresReported = false;
      for (int i=0 ; i < LANES ; i++) {
        laneStatus[i] = RACING;
        digitalWrite(finishLineLED[i], LOW);
      }
      Serial.println("race START");
      heatNumber++;
      digitalWrite(raceActiveLED, HIGH);
      displayGo();
      displayHeatNumber();
    } else if (strcmp(buffer, "START_GATE_CLOSED") == 0) {
      gateOpen = false;
      if (allFinished) {
        for (int i=0 ; i < LANES ; i++) {
          digitalWrite(finishLineLED[i], LOW);
          laneStatus[i] = AT_GATE;
        }
        tft.fillScreen(TFT_BLACK);
        displayReady();
        displaySet();
        digitalWrite(readyLED, HIGH);
      }
      if (allAtGate) {
        tft.fillScreen(TFT_BLACK);
        displayReady();
        displaySet();
        digitalWrite(readyLED, HIGH);
      }
    }
  }
};



// List of all the masters. It will be populated when a new master is registered
std::vector<ESP_NOW_Peer_Class> masters;

/* Callbacks */

// Callback called when an unknown peer sends a message
void register_new_master(const esp_now_recv_info_t *info, const uint8_t *data, int len, void *arg) {
  if (memcmp(info->des_addr, ESP_NOW.BROADCAST_ADDR, 6) == 0) {
    Serial.printf("Unknown peer " MACSTR " sent a broadcast message\n", MAC2STR(info->src_addr));
    Serial.println("Registering the peer as a master");
    commEstablished = true;
    tft.fillScreen(TFT_BLACK);
    displayReady();
    ESP_NOW_Peer_Class new_master(info->src_addr, ESPNOW_WIFI_CHANNEL, WIFI_IF_STA, NULL);

    masters.push_back(new_master);
    if (!masters.back().add_peer()) {
      Serial.println("Failed to register the new master");
      return;
    }
  } else {
    // The slave will only receive broadcast messages
    log_v("Received a unicast message from " MACSTR, MAC2STR(info->src_addr));
    log_v("Igorning the message");
  }
}

String getDefaultMacAddress() {

  String mac = "";

  unsigned char mac_base[6] = {0};

  if (esp_efuse_mac_get_default(mac_base) == ESP_OK) {
    char buffer[18];  // 6*2 characters for hex + 5 characters for colons + 1 character for null terminator
    sprintf(buffer, "%02X:%02X:%02X:%02X:%02X:%02X", mac_base[0], mac_base[1], mac_base[2], mac_base[3], mac_base[4], mac_base[5]);
    mac = buffer;
  }

  return mac;
}

/* Main */
 void setup() {
  
  Serial.begin(115200);
  touchscreenSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  touchscreen.begin(touchscreenSPI);
  touchscreen.setRotation(1);
  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  int centerX = SCREEN_WIDTH / 2;
  int centerY = SCREEN_HEIGHT / 2;
  // tft.drawCentreString("Ready, set, go!", centerX, 30, FONT_SIZE);
  // tft.drawCentreString("Touch screen to test", centerX, centerY, FONT_SIZE);

  Serial.print("TFT_MISO: "); Serial.println(TFT_MISO);
  Serial.print("TFT_MOSI: "); Serial.println(TFT_MOSI);
  Serial.print("TFT_SCLK: "); Serial.println(TFT_SCLK);
  Serial.print("TFT_CS: "); Serial.println(TFT_CS);
  Serial.print("TFT_DC: "); Serial.println(TFT_DC);
  Serial.print("TFT_RST: "); Serial.println(TFT_RST);
  Serial.print("TFT_BL: "); Serial.println(TFT_BL);

  Serial.print("XPT2046_IRQ: "); Serial.println(XPT2046_IRQ);
  Serial.print("XPT2046_MOSI: "); Serial.println(XPT2046_MOSI);
  Serial.print("XPT2046_MISO: "); Serial.println(XPT2046_MISO);
  Serial.print("XPT2046_CLK: "); Serial.println(XPT2046_CLK);
  Serial.print("XPT2046_CS: "); Serial.println(XPT2046_CS);

  Wire.begin(SDA0_Pin, SCL0_Pin);
  Serial.println(F("SSD1306 allocation OK"));
  while (!Serial) {
    delay(10);
  }

  // Initialize the Wi-Fi module
  WiFi.mode(WIFI_STA);
  WiFi.setChannel(ESPNOW_WIFI_CHANNEL);
  while (!WiFi.STA.started()) {
    delay(100);
  }
  
  char buffer[256];
  String text = getDefaultMacAddress();
  text.toCharArray(buffer, text.length()+1);
  Serial.println(buffer);
  delay(2000);

  Serial.println("ESP-NOW Example - Broadcast Slave");
  Serial.println("Wi-Fi parameters:");
  Serial.println("  Mode: STA");
  Serial.println("  MAC Address: " + WiFi.macAddress());
  Serial.printf("  Channel: %d\n", ESPNOW_WIFI_CHANNEL);

  // Initialize the ESP-NOW protocol
  if (!ESP_NOW.begin()) {
    Serial.println("Failed to initialize ESP-NOW");
    Serial.println("Reeboting in 5 seconds...");
    delay(5000);
    ESP.restart();
  }

  // Register the new peer callback
  ESP_NOW.onNewPeer(register_new_master, NULL);

  Serial.println("Setup complete. Waiting for a master to broadcast a message...");

  for (int i=0 ; i < LANES ; i++) {
    laneStatus[i] = AT_GATE;
    pinMode(finishLineLED[i], OUTPUT);
    digitalWrite(finishLineLED[i], LOW);
  }
  pinMode(readyLED, OUTPUT);
  pinMode(raceActiveLED, OUTPUT);
  digitalWrite(raceActiveLED, LOW);
  resetSwitch.setDebounceTime(50);
  if (commEstablished == false) {
    displayToggleGate();
  }
}

void loop() {
  // check the break beam pins for cars crossing the finish line
  for (int i=0 ; i < LANES ; i++) {
    if ((analogRead(breakBeamPin[i]) < 1000) && (laneStatus[i] == RACING)) {
      laneStatus[i] = FINISHED;
      // Serial.print("Lane "); Serial.print(i); Serial.println(" FINISHED");
      long now = millis();
      elapsedTime[i] = now - startTimeMillis;
      // Serial.print("Time: "); Serial.println(elapsedTime[i] / 1000.0);
      // Serial.println();
      digitalWrite(finishLineLED[i], HIGH);
      finishedRacerCount++;
      displayLaneTime(i);
    }
  }

  resetSwitch.loop();
  if ((resetSwitch.isPressed()) && (commEstablished)) {
    digitalWrite(readyLED, HIGH);
    for (int i=0 ; i < LANES ; i++) {
      digitalWrite(finishLineLED[i], LOW);
      laneStatus[i] = AT_GATE;
    }
    tft.fillScreen(TFT_BLACK);
    displayReady();
    displaySet();
    digitalWrite(readyLED, HIGH);
  }


  // check to see if all cars crossed the finish line
  allFinished = false;
  int finishedLanes = 0;
  for (int i=0 ; i < LANES ; i++) {
    if (laneStatus[i] == FINISHED) {
      finishedLanes++;
    }
  }
  if (finishedLanes == LANES) { // all lanes are in FINISHED state
    allFinished = true;
  }

  allAtGate = false;
  int atGateLanes = 0;
  for (int i=0 ; i < LANES ; i++) {
    if (laneStatus[i] == AT_GATE) {
      atGateLanes++;
    }
  }
  if (atGateLanes == LANES) { // all racers are at the START gate
    allAtGate = true;
  }

  if ((allFinished) && (scoresReported == false)) { // report scores
    Serial.println("race FINISHED");
    digitalWrite(raceActiveLED, LOW);
    Serial.print("Heat: "); Serial.println(heatNumber);
    for (int i=0 ; i < LANES ; i++) {
      Serial.print("Lane: "); Serial.print(i+1); Serial.print(", Time: "); Serial.print(elapsedTime[i] / 1000.0); Serial.println("s");
    }
    scoresReported = true;
  }

  // Checks if Touchscreen was touched, and prints X, Y and Pressure (Z) info on the TFT display and Serial Monitor
  if (touchscreen.tirqTouched() && touchscreen.touched()) {
    TS_Point p = touchscreen.getPoint();
    x = map(p.x, 200, 3700, 1, SCREEN_WIDTH);
    y = map(p.y, 240, 3800, 1, SCREEN_HEIGHT);
    z = p.z;
    printTouchToSerial(x, y, z);
    // printTouchToDisplay(x, y, z);
    // delay(100);
  }


}
