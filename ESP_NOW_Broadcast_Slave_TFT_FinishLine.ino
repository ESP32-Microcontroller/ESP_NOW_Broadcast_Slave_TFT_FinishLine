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
#define MAX_HEATS 256

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
#define REPEATED_TOUCH_TOLERANCE 1000

#define LANES 4
int laneStatus[LANES];
long elapsedTime[LANES];
int breakBeamPin[LANES] = {6, 7, 18, 16};
int finishLineLED[LANES] = {9, 10, 46, 17};
int readyLED = 11;
int raceActiveLED = 8;
bool scoresReported = false;
bool allFinished = false;
bool allAtGate = false;
bool commEstablished = false;
ezButton resetSwitch(3);
int finishedRacerCount;
int finishLineCrossingOrder[LANES];
int heatNumber = 0;
int heatData[MAX_HEATS][LANES]; // elapsed millis
long lastTouchMillis = millis() - REPEATED_TOUCH_TOLERANCE;

// set this to true if using racer names, time averaging, and finals
// set this to false if we're just running individual races
bool structuredRace = true;

// Racer registration
#define NUMBER_OF_RACERS 3
#define NUMBER_OF_TIMES 3
// int NUMBER_OF_RACERS_IN_FINALS = min(LANES, NUMBER_OF_RACERS); // this number should be le to LANES and le to NUMBER_OF_RACERS
String racerName[NUMBER_OF_RACERS] = {
    "Happy"
  , "Sleepy"
  , "Sneezy"
  // , "Doc"
  // , "Dopey"
  // , "Bashful"
  // , "Grumpy"
};
int lanesUsedInThisHeat = min(LANES, NUMBER_OF_RACERS);
int regularHeats = ceil((NUMBER_OF_RACERS * NUMBER_OF_TIMES) / (lanesUsedInThisHeat * 1.0));
bool isFinals = false; // change to true when running finals
float racerData[NUMBER_OF_RACERS][NUMBER_OF_TIMES];
int racerDataIndex[NUMBER_OF_RACERS];
float racerAverage[NUMBER_OF_RACERS];
int racerAverageIndex[NUMBER_OF_RACERS];
int laneAssignment[LANES];

int getRacerNumber(int heat, int lane) {
  return ((heat * lanesUsedInThisHeat) + lane) % NUMBER_OF_RACERS;
}

String getRacerName(int heat, int lane) {
  return racerName[getRacerNumber(heat, lane)];
}

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
  tft.setTextColor(TFT_SKYBLUE, TFT_BLACK);
  displayHeatNumber();
  if (structuredRace) { // map lanes to names
    // TODO: how do we know if this is the last heat before finals?
    // TODO: how do we know if there are fewer remaining racers than lanes?

    // how many lanes are we using for this heat?
    lanesUsedInThisHeat = min(LANES, NUMBER_OF_RACERS);
    Serial.print("########## lanesUsedInThisHeat: "); Serial.println(lanesUsedInThisHeat);
    if (heatNumber == regularHeats) {
      if (NUMBER_OF_RACERS < LANES) {
        lanesUsedInThisHeat = NUMBER_OF_RACERS;
      } else {
        lanesUsedInThisHeat = (NUMBER_OF_RACERS * NUMBER_OF_TIMES) - ((regularHeats - 1) * LANES);
      }
      Serial.print("########## (last qualifier) lanesUsedInThisHeat: "); Serial.println(lanesUsedInThisHeat);
      isFinals = false;
    } else if (heatNumber == regularHeats + 1) {
      // this is the finals
      isFinals = true;
    } else {
      isFinals = false;
    }

    Serial.print("##### Heat: "); Serial.print(heatNumber);
    if (isFinals) {
      Serial.print("  Finals!!!  ");
    }
    Serial.println("    Lane assignments:");
    for (int lane=0 ; lane < lanesUsedInThisHeat ; lane++) {
      int racerNumber;
      String name;
      if (isFinals) {
        Serial.println("isFinals: true");
        racerNumber = racerAverageIndex[lane];
        name = racerName[racerAverageIndex[lane]];
      } else {
        racerNumber = getRacerNumber(heatNumber - 1, lane);
        name = getRacerName(heatNumber - 1, lane);
        Serial.print("# isFinals: false");
        Serial.print(", heatNumber: "); Serial.print(heatNumber);
        Serial.print(", racerNumber: "); Serial.print(racerNumber);
        Serial.print(", name: ");Serial.print(name);
        Serial.print(", lane: "); Serial.println(lane);
      }
      Serial.print("  Lane: "); Serial.print(lane+1); Serial.print(", name: "); Serial.print(name);
      if (isFinals) {
        Serial.print(" ["); Serial.print(racerAverage[lane] / 1000.0); Serial.print("]");
      }
      Serial.print(", racerNumber: ");Serial.println(racerNumber);
      laneAssignment[lane] = racerNumber;

      char buffer[32];
      sprintf(buffer, "%1d: %s", lane+1, name);
      tft.setTextColor(TFT_SKYBLUE, TFT_BLACK);
      tft.drawString(buffer, 5, 70 + ((lane+1) * 30), FONT_SIZE);

      if (isFinals) {
        sprintf(buffer, "[%1.3fs]", racerAverage[lane] / 1000.0);
        tft.setTextColor(TFT_ORANGE, TFT_BLACK);
        tft.drawString(buffer, 175, 70 + ((lane+1) * 30), FONT_SIZE);
      }
    }
  }
}

void displayGo() {
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.drawString("Go!", 225, 30, FONT_SIZE);
}

void displayHeatNumber() {
  tft.setTextColor(TFT_SKYBLUE, TFT_BLACK);
  char buffer[32];
  sprintf(buffer, "Heat: %1d", heatNumber);
  tft.drawString(buffer, 5, 70, FONT_SIZE);
  if (heatNumber == regularHeats) {
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.drawString("Last Qualifier", 140, 70, FONT_SIZE);
    tft.setTextColor(TFT_SKYBLUE, TFT_BLACK);
  } else if (heatNumber == regularHeats + 1) {
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.drawString("Finals!!!", 140, 70, FONT_SIZE);
    tft.setTextColor(TFT_SKYBLUE, TFT_BLACK);
  } else {

  }
}

void displayLaneTime_old(int lane) {
  char buffer[32];
  sprintf(buffer, "Lane %1d: %7.3fs", (lane+1), (elapsedTime[lane] / 1000.0));
  tft.setTextColor(TFT_SKYBLUE, TFT_BLACK);
  tft.drawString(buffer, 60, 70 + (finishedRacerCount * 30), FONT_SIZE);
}

void displayLaneTime(int lane) {
  String place;
  tft.setTextColor(TFT_SILVER, TFT_BLACK);
  if (finishedRacerCount == 1) {
    place = "1st    ";
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
  } else if (finishedRacerCount == 2) {
    place = "2nd    ";
  } else if (finishedRacerCount == 3) {
    place = "3rd    ";
  } else {
    place = "4th    ";
  }
  char buffer[32];
  sprintf(buffer, "%s", place);
  tft.drawString(buffer, 175, 70 + ((lane+1) * 30), FONT_SIZE);
  sprintf(buffer, "%7.3fs", (elapsedTime[lane] / 1000.0));
  tft.drawString(buffer, 225, 70 + ((lane+1) * 30), FONT_SIZE);
}

void displayMaxHeatsReached() {
  tft.fillRect(0, 0, 320, 65, TFT_BLACK);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawString("Heat data limit reached.", 80, 5, 2);
  tft.drawString("Resetting to heat 0.", 80, 25, 2);
  tft.drawString("Press reset to continue.", 80, 45, 2);
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
        finishLineCrossingOrder[i] = 0;
      }
      Serial.println("RACE START!!!");
      digitalWrite(raceActiveLED, HIGH);
      displayGo();
    } else if (strcmp(buffer, "START_GATE_CLOSED") == 0) {
      gateOpen = false;
      if (allFinished) {
        for (int i=0 ; i < LANES ; i++) {
          digitalWrite(finishLineLED[i], LOW);
          laneStatus[i] = AT_GATE;
        }
        tft.fillScreen(TFT_BLACK);
        displayReady();
        // displaySet();
        digitalWrite(readyLED, HIGH);
      }
      if (allAtGate) {
        tft.fillScreen(TFT_BLACK);
        displayReady();
        Serial.println("######################## New Heat ########################");
        // Serial.print("allAtGate: Incrementing heatNumber from ");Serial.print(heatNumber);
        heatNumber++;
        // Serial.print(" to ");Serial.println(heatNumber);
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

void dumpHeatLog() {
  Serial.print("# Heat log, total heats: "); Serial.println(heatNumber+1);
  for (int heat = 0 ; heat < heatNumber ; heat++ ) {
    Serial.print("#   Heat: "); Serial.println(heat+1);
    for (int lane = 0 ; lane < LANES ; lane++) {
      char timeSeconds[16];
      sprintf(timeSeconds, "%1.3f", heatData[heat][lane] / 1000.0);
      Serial.print("#     Lane: "); Serial.print(lane+1); Serial.print(", Time: "); Serial.print(timeSeconds); Serial.println("s");
    }
  }
  Serial.println("# End of heat log");
}

void calculateAveragesAndSort() {
  Serial.println("\n##### Sorted Average Times of Racers:");
  for (int racer = 0 ; racer < NUMBER_OF_RACERS ; racer++) {
    float sum = 0;
    for (int i=0 ; i < NUMBER_OF_TIMES ; i++) {
      sum += racerData[racer][i];
    }
    racerAverage[racer] = sum / NUMBER_OF_TIMES;
    racerAverageIndex[racer] = racer; // initialize this
  }
  for (int i=0 ; i < NUMBER_OF_RACERS - 1 ; i++) {
    for (int j=0 ; j < NUMBER_OF_RACERS - i - 1 ; j++) {
      if (racerAverage[j] > racerAverage[j+1]) {
        float tempTime = racerAverage[j];
        racerAverage[j] = racerAverage[j+1];
        racerAverage[j+1] = tempTime;
        int tempIndex = racerAverageIndex[j];
        racerAverageIndex[j] = racerAverageIndex[j+1];
        racerAverageIndex[j+1] = tempIndex;
      }
    }
  }
  for (int i=0 ; i < NUMBER_OF_RACERS ; i++) {
    Serial.print("  Place: ");Serial.print(i+1);Serial.print(", name: ");
    Serial.print(racerName[racerAverageIndex[i]]);
    char buffer[16];
    sprintf(buffer, "%1.3fs", racerAverage[i] / 1000.0);
    Serial.print(", averageTime: ");Serial.println(buffer);
  }
}

void dumpRacerData() {
  Serial.println("# Racer log");
  for (int racer = 0 ; racer < NUMBER_OF_RACERS ; racer++) {
    float sum = 0;
    Serial.print(racer);Serial.print(":");Serial.println(racerName[racer]);
    for (int i=0 ; i < NUMBER_OF_TIMES ; i++) {
      char buffer[16];
      sprintf(buffer, "%1.3f", racerData[racer][i] / 1000.0);
      Serial.print("#\t");Serial.print(i);Serial.print(": ");Serial.println(buffer);
      sum += racerData[racer][i];
    }
    char buffer[16];
    sprintf(buffer, "%1.3f", (sum / NUMBER_OF_TIMES) / 1000.0);
    Serial.print("#\tAverage: ");Serial.println(buffer);
  }
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

  // Serial.print("TFT_MISO: "); Serial.println(TFT_MISO);
  // Serial.print("TFT_MOSI: "); Serial.println(TFT_MOSI);
  // Serial.print("TFT_SCLK: "); Serial.println(TFT_SCLK);
  // Serial.print("TFT_CS: "); Serial.println(TFT_CS);
  // Serial.print("TFT_DC: "); Serial.println(TFT_DC);
  // Serial.print("TFT_RST: "); Serial.println(TFT_RST);
  // Serial.print("TFT_BL: "); Serial.println(TFT_BL);

  // Serial.print("XPT2046_IRQ: "); Serial.println(XPT2046_IRQ);
  // Serial.print("XPT2046_MOSI: "); Serial.println(XPT2046_MOSI);
  // Serial.print("XPT2046_MISO: "); Serial.println(XPT2046_MISO);
  // Serial.print("XPT2046_CLK: "); Serial.println(XPT2046_CLK);
  // Serial.print("XPT2046_CS: "); Serial.println(XPT2046_CS);

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
  for (int racer=0 ; racer < NUMBER_OF_RACERS ; racer++) {
    racerDataIndex[racer] = 0;
    for (int i=0 ; i < NUMBER_OF_TIMES ; i++) {
      racerData[racer][i] = 0;
    }
  }
  
}

void loop() {
  // check the break beam pins for cars crossing the finish line
  for (int i=0 ; i < lanesUsedInThisHeat ; i++) {
    if ((analogRead(breakBeamPin[i]) < 1000) && (laneStatus[i] == RACING)) {
      laneStatus[i] = FINISHED;
      long now = millis();
      elapsedTime[i] = now - startTimeMillis;
      heatData[heatNumber-1][i] = elapsedTime[i];
      int racerNumber = laneAssignment[i];
      racerData[racerNumber][racerDataIndex[racerNumber]] = elapsedTime[i];
      racerDataIndex[racerNumber]++;
      digitalWrite(finishLineLED[i], HIGH);
      finishedRacerCount++;
      finishLineCrossingOrder[i] = finishedRacerCount;
      displayLaneTime(i);
    }
  }

  resetSwitch.loop();
  if ((resetSwitch.isPressed()) && (commEstablished)) {
    digitalWrite(readyLED, HIGH); // TODO: Can we remove one of these?  Seems like a duplicate.
    for (int i=0 ; i < LANES ; i++) {
      digitalWrite(finishLineLED[i], LOW);
      laneStatus[i] = AT_GATE;
    }
    tft.fillScreen(TFT_BLACK);
    displayReady();
    Serial.print("resetSwitch: Incrementing heatNumber from ");Serial.print(heatNumber);
    heatNumber++;
    Serial.print(" to ");Serial.println(heatNumber);
    displaySet();
    digitalWrite(readyLED, HIGH);
  }


  // check to see if all cars crossed the finish line
  allFinished = false;
  int finishedLanes = 0;
  for (int i=0 ; i < lanesUsedInThisHeat ; i++) {
    if (laneStatus[i] == FINISHED) {
      finishedLanes++;
    }
  }
  if (finishedLanes == lanesUsedInThisHeat) { // all lanes are in FINISHED state
    allFinished = true;
  }

  allAtGate = false;
  int atGateLanes = 0;
  for (int i=0 ; i < lanesUsedInThisHeat ; i++) {
    if (laneStatus[i] == AT_GATE) {
      atGateLanes++;
    }
  }
  if (atGateLanes == lanesUsedInThisHeat) { // all racers are at the START gate
    allAtGate = true;
  }

  if ((allFinished) && (scoresReported == false)) { // report scores
    Serial.println("RACE FINISHED!!!");
    digitalWrite(raceActiveLED, LOW);
    Serial.print("##### Heat: "); Serial.print(heatNumber); Serial.println("     Finish Times:");
    for (int i=0 ; i < lanesUsedInThisHeat ; i++) {
      String name = racerName[laneAssignment[i]];
      char timeSeconds[16];
      sprintf(timeSeconds, "%1.3f", elapsedTime[i] / 1000.0);
      Serial.print("  Lane: "); Serial.print(i+1); Serial.print(", Place: "); Serial.print(finishLineCrossingOrder[i]); Serial.print(", Name: "); Serial.print(name); Serial.print(", Time: "); Serial.print(timeSeconds); Serial.println("s");
    }
    scoresReported = true;
    if (heatNumber + 1 == MAX_HEATS) {
      dumpHeatLog();
      displayMaxHeatsReached();
      heatNumber = -1;
    }
    if (heatNumber == regularHeats) {
      calculateAveragesAndSort();
    }
  }

  // Checks if Touchscreen was touched, and prints X, Y and Pressure (Z) info on the TFT display and Serial Monitor
  if (touchscreen.tirqTouched() && touchscreen.touched()) {
    TS_Point p = touchscreen.getPoint();
    x = map(p.x, 200, 3700, 1, SCREEN_WIDTH);
    y = map(p.y, 240, 3800, 1, SCREEN_HEIGHT);
    z = p.z;
    long now = millis();
    if (now - lastTouchMillis > REPEATED_TOUCH_TOLERANCE) {
      dumpHeatLog();
      dumpRacerData();
      // calculateAveragesAndSort();
      lastTouchMillis = now;
    }

    // printTouchToSerial(x, y, z);
    // printTouchToDisplay(x, y, z);
    // delay(100);
  }


}
