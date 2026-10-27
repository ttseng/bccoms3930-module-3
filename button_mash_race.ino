/*
  Module 3: Button Mash Game for the ESP32 LILYGO T-Display
  Race to mash the buttons on the ESP32 the fastest.  The boards broadcast
  their button presses to each other using ESP-NOW.  Fastest wins.

  Supports 2-3 players (must modify TEAM_ID and PLAYER_ID). 
  - TEAM_ID:   should match for all members on a team
  - PLAYER_ID: unique for each member on a team (0, 1, or 2) 
*/

#include <WiFi.h>
#include <esp_now.h>
#include <TFT_eSPI.h>

#define TEAM_ID       1      // TODO: Modify based on your TEAM ID
#define PLAYER_ID     1      // TODO: Change to 0, 1, or 2. Each group member should have a unique ID.
#define MAX_PLAYERS   3
#define TARGET        40     // button presses to win
#define BUTTON_A      0      // left button (either button counts)
#define BUTTON_B      35    // right button (either button counts)
#define HEARTBEAT_MS  250

uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

TFT_eSPI tft = TFT_eSPI();

// counts[i] = presses by player i. Your own slot is updated in loop() while the others are filled in by the receive callback.
volatile uint16_t counts[MAX_PLAYERS] = {0, 0, 0};
volatile bool seen[MAX_PLAYERS]   = {false, false, false};
volatile bool resetRequested = false;
volatile bool needsRedraw = true;

bool     gameOver = false;
int      winnerId = -1;
uint32_t gameOverAt = 0;
uint32_t lastHeartbeat = 0;
int      lastSeenCount = 0;

bool lastA = HIGH, lastB = HIGH;
uint32_t lastPressTime = 0;

const uint16_t playerColors[MAX_PLAYERS] = {TFT_CYAN, TFT_ORANGE, TFT_GREEN};

// message structure between devices
typedef struct {
  uint8_t  teamId;     // which group this message belongs to
  uint8_t  playerId;   // which player in the group sent it
  uint8_t  isReset;    // 1 = "start a new round"
  uint16_t presses;    // sender's total button presses so far
} Message;

// receive callback
void onReceive(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len != sizeof(Message)) return;

  Message msg;
  memcpy(&msg, data, sizeof(msg));
  if (msg.teamId != TEAM_ID) return;                 // msgs from a different team
  if (msg.playerId >= MAX_PLAYERS) return;           // bad player number
  if (msg.playerId == PLAYER_ID) return;             // two boards with the same PLAYER_ID

  if (msg.isReset) {
    resetRequested = true;
  } else {
    counts[msg.playerId] = msg.presses;
    seen[msg.playerId] = true;
  }
  needsRedraw = true;
}

void setupEspNow() {
  WiFi.mode(WIFI_STA);
  if (esp_now_init() != ESP_OK) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_RED);
    tft.drawString("ESP-NOW init failed", 10, 60, 2);
    while (true) delay(1000);
  }
  esp_now_register_recv_cb(onReceive);

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, broadcastAddress, 6);
  peer.channel = 0;
  peer.encrypt = false;
  esp_now_add_peer(&peer);
}

void sendState(bool reset = false) {
  Message msg;
  msg.teamId   = TEAM_ID;
  msg.playerId = PLAYER_ID;
  msg.isReset  = reset ? 1 : 0;
  msg.presses  = counts[PLAYER_ID];
  esp_now_send(broadcastAddress, (uint8_t *)&msg, sizeof(msg)); // where state is broadcasted
}

// set up variables for drawing progress bars for each player
const int LANE_X = 48, LANE_W = 188, LANE_H = 32, LANE_TOP = 8, LANE_STEP = 38;

// drawScreen handles everything that is drawn to the built-in display
void drawScreen() {
  seen[PLAYER_ID] = true;                            

  int numSeen = 0;
  for (int i = 0; i < MAX_PLAYERS; i++) if (seen[i]) numSeen++;

  // Only wipe the whole screen when a lane appears; otherwise redraw in place
  // (this avoids flicker while people are mashing).
  if (numSeen != lastSeenCount) {
    tft.fillScreen(TFT_BLACK);
    lastSeenCount = numSeen;
  }

  int lane = 0;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (!seen[i]) continue;
    int y = LANE_TOP + lane * LANE_STEP;
    lane++;

    int count = counts[i];
    int fillW = constrain((count * LANE_W) / TARGET, 0, LANE_W);

    // Player label and count (opaque text with padding, so old text is wiped)
    tft.setTextPadding(44);
    tft.setTextColor(playerColors[i], TFT_BLACK);
    tft.drawString(i == PLAYER_ID ? "YOU" : "P" + String(i + 1), 2, y, 2);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString(String(count), 2, y + 16, 2);
    tft.setTextPadding(0);

    // Progress bar: filled part, empty part, outline (thicker for your own lane)
    tft.fillRect(LANE_X, y, fillW, LANE_H, playerColors[i]);
    tft.fillRect(LANE_X + fillW, y, LANE_W - fillW, LANE_H, TFT_BLACK);
    tft.drawRect(LANE_X - 1, y - 1, LANE_W + 2, LANE_H + 2,
                 i == PLAYER_ID ? TFT_WHITE : TFT_DARKGREY);
  }

  // Banner strip for notifications at the bottom
  tft.fillRect(0, 118, 240, 17, TFT_BLACK);
  if (gameOver) {
    tft.setTextDatum(MC_DATUM);
    if (winnerId == PLAYER_ID) {
      tft.setTextColor(TFT_GREEN, TFT_BLACK);
      tft.drawString("YOU WIN!  Press for rematch", 120, 126, 2);
    } else {
      tft.setTextColor(TFT_RED, TFT_BLACK);
      tft.drawString("P" + String(winnerId + 1) + " wins.  Press for rematch", 120, 126, 2);
    }
    tft.setTextDatum(TL_DATUM);
  }
}

// GAME LOGIC
void startNewRound() {
  for (int i = 0; i < MAX_PLAYERS; i++) counts[i] = 0;
  gameOver = false;
  winnerId = -1;
  needsRedraw = true;
}

void checkForWinner() {
  if (gameOver) return;
  int best = -1;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (seen[i] && counts[i] >= TARGET && (best < 0 || counts[i] > counts[best])) best = i;
  }
  if (best >= 0) {
    gameOver = true;
    winnerId = best;
    gameOverAt = millis();
    needsRedraw = true;
  }
}

bool buttonPressed(uint8_t pin, bool &last) {
  bool now = digitalRead(pin);
  bool pressed = (last == HIGH && now == LOW);
  last = now;
  return pressed;
}

void setup() {
  Serial.begin(115200);

  pinMode(BUTTON_A, INPUT_PULLUP);
  pinMode(BUTTON_B, INPUT);          // GPIO 35 has an external pull-up on the board

  tft.init();
  tft.setRotation(1);                
  tft.fillScreen(TFT_BLACK);

  setupEspNow();
  drawScreen();
}

void loop() {
  if (resetRequested) {
    resetRequested = false;
    startNewRound();
  }

  bool a = buttonPressed(BUTTON_A, lastA);
  bool b = buttonPressed(BUTTON_B, lastB);
  bool pressed = (a || b) && (millis() - lastPressTime > 20); // small debounce built in

  if (pressed) {
    lastPressTime = millis();

    if (!gameOver) {
      counts[PLAYER_ID] = counts[PLAYER_ID] + 1;
      sendState();
      needsRedraw = true;
    } else if (millis() - gameOverAt > 1500) {
      startNewRound();
      sendState(true);               // reset for everyone
    }
  }

  checkForWinner();

  // resend our total so a lost packet gets corrected
  if (!gameOver && millis() - lastHeartbeat > HEARTBEAT_MS) {
    lastHeartbeat = millis();
    sendState();
  }

  // only redraw only when something changed
  if (needsRedraw) {
    needsRedraw = false;
    drawScreen();
  }
}
