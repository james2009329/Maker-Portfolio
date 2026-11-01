#include <SPI.h>
#include <TFT_eSPI.h>
#include <Preferences.h>
#include <QRCodeGenerator.h>

TFT_eSPI tft = TFT_eSPI();
Preferences prefs;
#define BACKLIGHT_PIN 27

// ================= Colors =================
#define COLOR_PAPER    0xEF5D
#define COLOR_INK      0x2104
#define COLOR_GRID     0xC618
#define COLOR_FUNC     0x2A6F
#define COLOR_INTEGRAL 0xB2A9
#define COLOR_BUTTON   0xDEFB
#define COLOR_BUTTON_BORDER 0x8C69
#define COLOR_POPUP_BG 0x738E   // dim backdrop behind the draw popup

// ================= Structs & Prototypes =================
struct Button {
  int x, y, w, h;
  const char* label;
};
void drawButton(Button b, bool pressed = false);
bool touchedButton(int tx, int ty, Button b);
void replayStatsToPanels();
void drawPanelFrame(int left, const char* title, const char* yLabel, const char* xLabel, float yMin, float yMax);

// ================= Button Definitions =================
Button homeButtons[4] = {
  { 30,  70, 200, 90, "Examples" },
  {250,  70, 200, 90, "Calculate a Function" },
  { 30, 180, 200, 90, "Draw Function" },
  {250, 180, 200, 90, "Info Menu" }
};
Button backButton = {10, 10, 90, 30, "< Back"};

// Info Menu
Button howItWorksBtn = {140,  65, 200, 55, "How It Works"};
Button calibMenuBtn  = {140, 130, 200, 55, "Calibration"};
Button aboutBtn      = {140, 195, 200, 55, "About"};
Button scrollUpBtn   = {430, 60, 40, 90, "^"};
Button scrollDownBtn = {430, 170, 40, 90, "v"};

// Example Buttons
Button flatBtn  = {140,  70, 200, 50, "Flat Line"};
Button slopeBtn = {140, 140, 200, 50, "Sloped Line"};
Button curveBtn = {140, 210, 200, 50, "Curved Line"};

// Run Controls
Button runBtn     = { 20, 285, 90, 30, "Run"};
Button stopBtn    = {120, 285, 90, 30, "Stop"};
Button resetBtn   = {220, 285, 90, 30, "Reset"};
Button resultsBtn = {370, 285, 90, 30, "Results"};

// Draw popup controls -- positioned dynamically in showDrawCanvas()
Button canvasClearBtn  = {0, 0, 0, 0, "Clear"};
Button canvasFinishBtn = {0, 0, 0, 0, "Finish"};

// Equation Numpad Buttons
Button numBtns[12] = {
  {250,  70, 50, 40, "1"}, {310,  70, 50, 40, "2"}, {370,  70, 50, 40, "3"},
  {250, 120, 50, 40, "4"}, {310, 120, 50, 40, "5"}, {370, 120, 50, 40, "6"},
  {250, 170, 50, 40, "7"}, {310, 170, 50, 40, "8"}, {370, 170, 50, 40, "9"},
  {250, 220, 50, 40, "-"}, {310, 220, 50, 40, "0"}, {370, 220, 50, 40, "DEL"}
};
Button selABtn = {50, 70, 170, 40, "A: 0"};
Button selBBtn = {50, 120, 170, 40, "B: 50"};
Button selCBtn = {50, 170, 170, 40, "C: 10"};
Button eqGraphBtn = {50, 220, 170, 40, "Graph"};

char lblA[24] = "A: 0";
char lblB[24] = "B: 50";
char lblC[24] = "C: 10";

// ================= Calibration Menu =================
// The machine's real calibration lives here, on the display board. It is saved to flash
// and pushed to the motor board on boot and on every change, overwriting the motor
// board's own values. The defaults below are only used until a value is saved, and the
// belt pitch / steps-per-mm defaults predate the switch to GT3 (3 mm) belts: set them
// from the Calibration screen.
struct CalibParam {
  const char* label;
  const char* unit;
  float value;
  float minVal;
  float maxVal;
  int decimals; // 0 = show as integer, else show 2 decimal places
};

CalibParam calibParams[10] = {
  {"Belt Pitch",       "mm", 2.0,   0.5,  10.0,   2}, // 0
  {"Pulley Teeth",     "",   20,    8,    100,    0}, // 1
  {"Diff. Ratio",      "",   1.0,   0.1,  10.0,   2}, // 2
  {"Disk Radius",      "mm", 100.0, 10.0, 500.0,  2}, // 3
  {"Max Travel",       "mm", 90.0,  10.0, 500.0,  2}, // 4
  {"Steps/mm",         "",   160.0, 1.0,  2000.0, 2}, // 5
  {"Microstepping",    "x",  32,    1,    256,    0}, // 6
  {"Encoder Cnts/Rev", "",   4096,  360,  65536,  0}, // 7
  {"Wheel Radius",     "mm", 25.6,  1.0,  200.0,  2}, // 8
  {"Friction Coeff",   "",   1.0,   0.1,  3.0,    2}  // 9 (optional)
};
const char* calibPrefKeys[10] = {"bp","pt","dr","dk","mt","sm","ms","ec","wr","fr"};

Button calibListBtns[10]; // positioned at runtime in updateCalibListUI()
Button calibNumBtns[12] = {
  {255,  58, 48, 40, "1"}, {308,  58, 48, 40, "2"}, {361,  58, 48, 40, "3"},
  {255, 103, 48, 40, "4"}, {308, 103, 48, 40, "5"}, {361, 103, 48, 40, "6"},
  {255, 148, 48, 40, "7"}, {308, 148, 48, 40, "8"}, {361, 148, 48, 40, "9"},
  {255, 193, 48, 40, "."}, {308, 193, 48, 40, "0"}, {361, 193, 48, 40, "DEL"}
};
Button calibSaveBtn   = {255, 240, 100, 35, "Save"};
Button calibRecalcBtn = {361, 240,  63, 35, "Recalc"};

int selectedCalibIndex = -1;
String calibEditBuf = "";

// ================= Screen State =================
enum Screen { HOME, INFO_MENU, HOW_IT_WORKS, ABOUT, EXAMPLES_MENU, EXAMPLES, DRAW_CANVAS, DRAW_RUN, WRITE_EQ, RESULTS, CALIB_MENU };
Screen currentScreen = HOME;
Screen returnScreen = HOME;

unsigned long lastTouchMillis = 0;
const unsigned long TOUCH_COOLDOWN_MS = 250;

// ================= Dimensions & Variables =================
const int PANEL_W = 200;
const int PANEL_H = 180;
const int PANEL_TOP = 70;
const int PANEL1_LEFT = 30;
const int PANEL2_LEFT = 260;

// Draw popup: same 10:9 aspect ratio as the graph panels, scaled up and centered.
const int DRAW_POPUP_W = 222;
const int DRAW_POPUP_H = 200;
const int DRAW_POPUP_X = (480 - DRAW_POPUP_W) / 2;
const int DRAW_POPUP_Y = 60;

int drawnCurve[DRAW_POPUP_W];
int lastDrawTx = -1, lastDrawTy = -1;

int howItWorksScrollY = 0;
const int HOW_VIEW_X = 20, HOW_VIEW_Y = 60, HOW_VIEW_W = 400, HOW_VIEW_H = 250;

const float X_AXIS_MAX = 15000;

// Physical amplitude for converting a curve into motor targets. Must match the motor
// board's DISPLAY_RADIUS_MAX. Do NOT tie it to the graph's autoscale value below: that
// starts small, so the first run would only use a fraction of the carriage's range.
const float FUNC_SEND_SCALE = 8000;

// Autoscaling axis bounds. Display only, never used for motor commands.
float funcAxisMax = 1000;       // f(x) panel: 0 .. funcAxisMax
float integralAxisMax = 1000;   // Integral panel: -integralAxisMax .. +integralAxisMax

int lastFuncPx = -1, lastFuncPy = -1;
int lastIntPx = -1, lastIntPy = -1;
float lastPlottedX = -1;
String serialBuffer = "";

// Equation variables
String strA = "0", strB = "50", strC = "10";
int eqA = 0, eqB = 50, eqC = 10;
int selectedCoeff = 0; // 0=A, 1=B, 2=C

// Run Handshake Flag
bool waitingForAck = false;

// Results Engine Variables
// The on-screen "Actual Integral" uses MECH_CALIBRATION_CONST, a rough display-only
// estimate, not the true integral. Use the motor board's RESULT line for real data.
float theoIntTotal = 0;
float lastX_val = 0;
const float MECH_CALIBRATION_CONST = 0.0025;

float stat_exp[200];
float stat_theo[200];
float stat_x[200];
float stat_fx[200];
int stat_count = 0;

char currentRunTitle[24] = "Run";

// ================= Touch Calibration =================
uint16_t calData[5];
void touchCalibrate() {
  prefs.begin("touchcal2", false);
  if (prefs.isKey("calData")) {
    prefs.getBytes("calData", calData, sizeof(calData));
    tft.setTouch(calData);
  } else {
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(20, 0);
    tft.println("Touch corners to calibrate...");
    tft.calibrateTouch(calData, TFT_WHITE, TFT_BLACK, 15);
    prefs.putBytes("calData", calData, sizeof(calData));
    tft.setTouch(calData);
  }
  prefs.end();
}

// ================= Calibration Storage & Sync =================
void saveCalibToPrefs() {
  prefs.begin("calib", false);
  for (int i = 0; i < 10; i++) prefs.putFloat(calibPrefKeys[i], calibParams[i].value);
  prefs.end();
}

void loadCalibFromPrefs() {
  prefs.begin("calib", true);
  for (int i = 0; i < 10; i++) {
    if (prefs.isKey(calibPrefKeys[i])) {
      calibParams[i].value = prefs.getFloat(calibPrefKeys[i], calibParams[i].value);
    }
  }
  prefs.end();
}

// On this board, Serial IS the link to the motor board, not a USB debug console.
void sendCalibToMotor() {
  Serial.print("CALIB:");
  for (int i = 0; i < 10; i++) {
    Serial.print(calibParams[i].value, 4);
    if (i < 9) Serial.print(",");
  }
  Serial.println();
}

String formatCalibValue(int idx) {
  char buf[16];
  if (calibParams[idx].decimals == 0) sprintf(buf, "%d", (int)round(calibParams[idx].value));
  else sprintf(buf, "%.2f", calibParams[idx].value);
  return String(buf);
}

// ================= Core Graphics Helpers =================
void drawButton(Button b, bool pressed) {
  tft.fillRoundRect(b.x, b.y, b.w, b.h, 8, pressed ? COLOR_BUTTON_BORDER : COLOR_BUTTON);
  tft.drawRoundRect(b.x, b.y, b.w, b.h, 8, COLOR_INK);
  tft.setTextColor(COLOR_INK, pressed ? COLOR_BUTTON_BORDER : COLOR_BUTTON);
  tft.setTextDatum(MC_DATUM);

  // Drop to text size 1 if the label doesn't fit at size 2.
  tft.setTextSize(2);
  int padding = 10;
  if (tft.textWidth(b.label) > b.w - padding) {
    tft.setTextSize(1);
  }

  tft.drawString(b.label, b.x + b.w / 2, b.y + b.h / 2);
  tft.setTextDatum(TL_DATUM);
}

bool touchedButton(int tx, int ty, Button b) {
  return (tx >= b.x && tx <= b.x + b.w && ty >= b.y && ty <= b.y + b.h);
}

// Titles start at x=110 so the back button (x=10-100) never covers them.
void drawTitle(const char* title) {
  tft.fillRect(0, 0, 480, 50, COLOR_PAPER);
  tft.setTextColor(COLOR_INK, COLOR_PAPER);
  tft.setTextSize(3);
  tft.setCursor(110, 12);
  tft.print(title);
  tft.drawFastHLine(0, 50, 480, COLOR_INK);
}

// Stacked one letter per row: TFT_eSPI can't reliably rotate plain text 90 degrees
// without a sprite.
void drawVerticalLabel(int x, int y, const char* label, int maxHeight) {
  tft.setTextColor(COLOR_INK, COLOR_PAPER);
  tft.setTextSize(1);
  tft.setTextDatum(TL_DATUM);
  int len = strlen(label);
  int rowH = 10;
  int totalH = len * rowH;
  int startY = y + max(0, (maxHeight - totalH) / 2);
  for (int i = 0; i < len; i++) {
    char buf[2] = { label[i], '\0' };
    tft.drawString(buf, x, startY + i * rowH);
  }
}

// ================= Screens =================
void showHome() {
  currentScreen = HOME;
  tft.fillScreen(COLOR_PAPER);
  drawTitle("Integrator Menu");
  for (int i = 0; i < 4; i++) drawButton(homeButtons[i]);
}

void showInfoMenu() {
  currentScreen = INFO_MENU;
  tft.fillScreen(COLOR_PAPER);
  drawTitle("Info Menu");
  drawButton(backButton);
  drawButton(howItWorksBtn);
  drawButton(calibMenuBtn);
  drawButton(aboutBtn);
}

void showAbout() {
  currentScreen = ABOUT;
  tft.fillScreen(COLOR_PAPER);
  drawTitle("About");
  drawButton(backButton);

  tft.setTextColor(COLOR_INK, COLOR_PAPER);
  tft.setTextSize(2);
  tft.setCursor(20, 62);  tft.println("This model was created by");
  tft.setCursor(20, 88);  tft.println("James Lin in the summer of");
  tft.setCursor(20, 114); tft.println("2026. It was inspired by");
  tft.setCursor(20, 140); tft.println("Jack Monaco's design.");

  tft.setTextSize(1);
  tft.setCursor(20, 168);
  tft.println("Scan the QR code below to see more of my work:");
  tft.setCursor(20, 180);
  tft.println("https://james2009329.github.io/portfolio/");

  QRCode qrcode;
  uint8_t qrcodeData[qrcode_getBufferSize(3)];
  qrcode_initText(&qrcode, qrcodeData, 3, 0, "https://james2009329.github.io/portfolio/");

  int pixelSize = 4;
  int offsetX = 160 - (qrcode.size * pixelSize) / 2;
  int offsetY = 196;

  for (uint8_t y = 0; y < qrcode.size; y++) {
    for (uint8_t x = 0; x < qrcode.size; x++) {
      uint16_t c = qrcode_getModule(&qrcode, x, y) ? COLOR_INK : COLOR_PAPER;
      tft.fillRect(offsetX + (x * pixelSize), offsetY + (y * pixelSize), pixelSize, pixelSize, c);
    }
  }
}

// Uses a hardware viewport so scrolled text is clipped and can't overlap the header.
void updateHowItWorksText() {
  tft.setViewport(HOW_VIEW_X, HOW_VIEW_Y, HOW_VIEW_W, HOW_VIEW_H);
  tft.fillScreen(COLOR_PAPER); // clears only the viewport, not the whole screen
  tft.setTextColor(COLOR_INK, COLOR_PAPER);
  tft.setTextSize(2);

  String body = "A carriage sets the radius where a small wheel touches a spinning turntable. As the turntable rotates, the wheel is dragged along at that radius, so its own rotation accumulates radius times angle, the integral of the function you set with the carriage.\n\nAn AS5600 magnetic sensor reads the wheel's rotation to give the running total, and two stepper motors drive the turntable and carriage via a belt differential.";

  int cursorX = 0, cursorY = howItWorksScrollY;
  String word = "";
  for (int i = 0; i <= (int)body.length(); i++) {
    char c = (i < (int)body.length()) ? body[i] : ' ';
    if (c == ' ' || c == '\n') {
      int wWidth = tft.textWidth(word);
      if (cursorX + wWidth > HOW_VIEW_W - 10) { cursorX = 0; cursorY += 26; }
      tft.setCursor(cursorX, cursorY);
      tft.print(word);
      cursorX += wWidth + tft.textWidth(" ");
      word = "";
      if (c == '\n') { cursorX = 0; cursorY += 26; }
    } else { word += c; }
  }
  tft.resetViewport();
}

void showHowItWorks() {
  currentScreen = HOW_IT_WORKS;
  howItWorksScrollY = 0;
  tft.fillScreen(COLOR_PAPER);
  drawTitle("How It Works");
  drawButton(backButton);
  drawButton(scrollUpBtn);
  drawButton(scrollDownBtn);
  updateHowItWorksText();
}

void showExamplesMenu() {
  currentScreen = EXAMPLES_MENU;
  tft.fillScreen(COLOR_PAPER);
  drawTitle("Select Example");
  drawButton(backButton);
  drawButton(flatBtn);
  drawButton(slopeBtn);
  drawButton(curveBtn);
}

void updateNumpadUI() {
  sprintf(lblA, "A: %s", strA.c_str()); selABtn.label = lblA;
  sprintf(lblB, "B: %s", strB.c_str()); selBBtn.label = lblB;
  sprintf(lblC, "C: %s", strC.c_str()); selCBtn.label = lblC;

  drawButton(selABtn, selectedCoeff == 0);
  drawButton(selBBtn, selectedCoeff == 1);
  drawButton(selCBtn, selectedCoeff == 2);
}

void showWriteEq() {
  currentScreen = WRITE_EQ;
  tft.fillScreen(COLOR_PAPER);
  drawTitle("f(x) = Ax^2 + Bx + C");
  drawButton(backButton);

  for (int i = 0; i < 12; i++) drawButton(numBtns[i]);
  drawButton(eqGraphBtn);
  updateNumpadUI();
}

void showResults() {
  currentScreen = RESULTS;
  tft.fillScreen(COLOR_PAPER);
  drawTitle("Analytics & Results");
  drawButton(backButton);

  float rSquared = 0;
  if (stat_count > 1) {
    float sumExp = 0;
    for (int i = 0; i < stat_count; i++) sumExp += stat_exp[i];
    float meanExp = sumExp / stat_count;

    float ssTot = 0, ssRes = 0;
    for (int i = 0; i < stat_count; i++) {
      ssTot += pow(stat_exp[i] - meanExp, 2);
      ssRes += pow(stat_exp[i] - stat_theo[i], 2);
    }
    if (ssTot > 0) rSquared = 1.0 - (ssRes / ssTot);
    else rSquared = 1.0;
  }

  // Percent error compares the final experimental (measured) integral
  // against the final actual (calculated) integral.
  float experimentalFinal = (stat_count > 0 ? stat_exp[stat_count - 1] : 0);
  bool percentErrorValid = (stat_count > 0 && theoIntTotal != 0);
  float percentError = 0;
  if (percentErrorValid) {
    percentError = fabs(theoIntTotal - experimentalFinal) / fabs(theoIntTotal) * 100.0;
  }

  tft.setTextColor(COLOR_INK, COLOR_PAPER);
  tft.setTextSize(2);

  tft.setCursor(40, 100); tft.printf("Actual Integral:        %.0f", theoIntTotal);
  tft.setCursor(40, 140); tft.printf("Experimental Integral:  %.0f", experimentalFinal);
  tft.setCursor(40, 180);

  if (stat_count > 1) tft.printf("R-Squared:              %.4f", rSquared);
  else tft.print("R-Squared:              N/A (Run First)");

  tft.setCursor(40, 220);
  if (percentErrorValid) tft.printf("Percent Error:          %.2f%%", percentError);
  else tft.print("Percent Error:          N/A (Run First)");
}

// ================= Calibration Screen =================
// Left: all 10 params. Right: keypad editing the selected row, which shows the live buffer.
void updateCalibListUI() {
  for (int i = 0; i < 10; i++) {
    Button b = {15, 58 + i * 21, 225, 19, ""};
    calibListBtns[i] = b;

    bool selected = (i == selectedCalibIndex);
    char buf[48];
    String valStr = selected ? (calibEditBuf + "_") : formatCalibValue(i);
    snprintf(buf, sizeof(buf), "%s: %s%s", calibParams[i].label, valStr.c_str(), calibParams[i].unit);

    tft.fillRoundRect(b.x, b.y, b.w, b.h, 4, selected ? COLOR_BUTTON_BORDER : COLOR_BUTTON);
    tft.drawRoundRect(b.x, b.y, b.w, b.h, 4, COLOR_INK);
    tft.setTextColor(COLOR_INK, selected ? COLOR_BUTTON_BORDER : COLOR_BUTTON);
    tft.setTextSize(1);
    tft.setTextDatum(ML_DATUM);
    tft.drawString(buf, b.x + 5, b.y + b.h / 2);
    tft.setTextDatum(TL_DATUM);
  }
}

void showCalibMenu() {
  currentScreen = CALIB_MENU;
  selectedCalibIndex = -1;
  calibEditBuf = "";
  tft.fillScreen(COLOR_PAPER);
  drawTitle("Calibration");
  drawButton(backButton);
  for (int i = 0; i < 12; i++) drawButton(calibNumBtns[i]);
  drawButton(calibSaveBtn);
  drawButton(calibRecalcBtn);
  updateCalibListUI();
}

void handleCalibNumKey(String key) {
  if (selectedCalibIndex < 0) return;

  if (key == "DEL") {
    if (calibEditBuf.length() > 0) calibEditBuf.remove(calibEditBuf.length() - 1);
  } else if (key == ".") {
    if (calibEditBuf.indexOf('.') == -1) {
      if (calibEditBuf.length() == 0) calibEditBuf = "0";
      calibEditBuf += ".";
    }
  } else {
    if (calibEditBuf == "0") calibEditBuf = key;
    else if (calibEditBuf.length() < 8) calibEditBuf += key;
  }
  updateCalibListUI();
}

// Clamps to range, saves to flash, and pushes the full set to the motor board.
void saveCalibField() {
  if (selectedCalibIndex < 0 || calibEditBuf.length() == 0) return;

  float v = calibEditBuf.toFloat();
  if (v < calibParams[selectedCalibIndex].minVal) v = calibParams[selectedCalibIndex].minVal;
  if (v > calibParams[selectedCalibIndex].maxVal) v = calibParams[selectedCalibIndex].maxVal;
  calibParams[selectedCalibIndex].value = v;

  saveCalibToPrefs();
  sendCalibToMotor();

  selectedCalibIndex = -1;
  calibEditBuf = "";
  updateCalibListUI();
}

void redrawCanvasOnly() {
  tft.fillRect(DRAW_POPUP_X + 1, DRAW_POPUP_Y + 1, DRAW_POPUP_W - 2, DRAW_POPUP_H - 2, COLOR_PAPER);
  for (int i = 0; i < DRAW_POPUP_W - 1; i++) {
    if (drawnCurve[i] >= 0 && drawnCurve[i + 1] >= 0) {
      tft.drawLine(DRAW_POPUP_X + i, DRAW_POPUP_Y + drawnCurve[i],
                   DRAW_POPUP_X + i + 1, DRAW_POPUP_Y + drawnCurve[i + 1], COLOR_FUNC);
    } else if (drawnCurve[i] >= 0) {
      tft.drawPixel(DRAW_POPUP_X + i, DRAW_POPUP_Y + drawnCurve[i], COLOR_FUNC);
    }
  }
}

void showDrawCanvas() {
  currentScreen = DRAW_CANVAS;

  tft.fillScreen(COLOR_POPUP_BG);
  drawTitle("Draw a Function");
  drawButton(backButton);

  tft.fillRoundRect(DRAW_POPUP_X - 10, DRAW_POPUP_Y - 10, DRAW_POPUP_W + 20, DRAW_POPUP_H + 20, 10, COLOR_PAPER);
  tft.drawRoundRect(DRAW_POPUP_X - 10, DRAW_POPUP_Y - 10, DRAW_POPUP_W + 20, DRAW_POPUP_H + 20, 10, COLOR_INK);
  tft.drawRect(DRAW_POPUP_X, DRAW_POPUP_Y, DRAW_POPUP_W, DRAW_POPUP_H, COLOR_INK);

  drawVerticalLabel(DRAW_POPUP_X - 26, DRAW_POPUP_Y, "Carriage", DRAW_POPUP_H);

  tft.setTextColor(COLOR_INK, COLOR_PAPER);
  tft.setTextSize(1);
  tft.setTextDatum(TR_DATUM);
  tft.drawString("Turntable", DRAW_POPUP_X + DRAW_POPUP_W, DRAW_POPUP_Y + DRAW_POPUP_H + 4, 1);
  tft.setTextDatum(TL_DATUM);

  canvasClearBtn  = { DRAW_POPUP_X,       DRAW_POPUP_Y + DRAW_POPUP_H + 18, 105, 30, "Clear"};
  canvasFinishBtn = { DRAW_POPUP_X + 117, DRAW_POPUP_Y + DRAW_POPUP_H + 18, 105, 30, "Finish"};
  drawButton(canvasClearBtn);
  drawButton(canvasFinishBtn);

  redrawCanvasOnly();
  lastDrawTx = -1; lastDrawTy = -1;
}

// yMin/yMax are passed in so a panel can autoscale and (Integral panel) go negative.
void drawPanelFrame(int left, const char* title, const char* yLabel, const char* xLabel, float yMin, float yMax) {
  tft.drawRect(left, PANEL_TOP, PANEL_W, PANEL_H, COLOR_INK);
  for (int i = 0; i <= 4; i++) tft.drawFastHLine(left, PANEL_TOP + (PANEL_H * i / 4), PANEL_W, COLOR_GRID);
  for (int i = 0; i <= 4; i++) tft.drawFastVLine(left + (PANEL_W * i / 4), PANEL_TOP, PANEL_H, COLOR_GRID);

  tft.setTextColor(COLOR_INK, COLOR_PAPER);
  tft.setTextSize(1);

  tft.setCursor(left, PANEL_TOP - 14);
  tft.print(title);

  // Sideways y-axis label, in the gutter to the left of the panel
  drawVerticalLabel(left - 16, PANEL_TOP, yLabel, PANEL_H);

  // Current axis bounds in the panel corners
  tft.setTextColor(COLOR_INK, COLOR_PAPER);
  tft.setCursor(left + 2, PANEL_TOP + 1);
  tft.print((int)yMax);
  tft.setCursor(left + 2, PANEL_TOP + PANEL_H - 9);
  tft.print((int)yMin);

  // Zero line, only when the axis spans zero
  if (yMin < 0 && yMax > 0) {
    float ratio = (0 - yMin) / (yMax - yMin);
    int zeroY = (PANEL_TOP + PANEL_H) - (int)(ratio * PANEL_H);
    tft.drawFastHLine(left, zeroY, PANEL_W, COLOR_INK);
    tft.drawFastHLine(left, zeroY + 1, PANEL_W, COLOR_INK);
  }

  tft.setTextDatum(TR_DATUM);
  tft.drawString(xLabel, left + PANEL_W, PANEL_TOP + PANEL_H + 4, 1);
  tft.setTextDatum(TL_DATUM);
}

void showRunScreen(const char* title) {
  strncpy(currentRunTitle, title, sizeof(currentRunTitle) - 1);
  currentRunTitle[sizeof(currentRunTitle) - 1] = '\0';

  lastFuncPx = -1; lastFuncPy = -1;
  lastIntPx = -1; lastIntPy = -1;
  lastPlottedX = -1;
  theoIntTotal = 0;
  lastX_val = 0;
  stat_count = 0;
  waitingForAck = false;

  // Reset autoscaling axes for a fresh run
  funcAxisMax = 1000;
  integralAxisMax = 1000;

  tft.fillScreen(COLOR_PAPER);
  drawTitle(title);
  drawButton(backButton);
  drawButton(runBtn);
  drawButton(stopBtn);
  drawButton(resetBtn);
  drawButton(resultsBtn);

  drawPanelFrame(PANEL1_LEFT, "Function f(x)", "Radius", "Turntable", 0, funcAxisMax);
  drawPanelFrame(PANEL2_LEFT, "Integral", "Rotation", "Turntable", -integralAxisMax, integralAxisMax);
}

// Redraws the run screen WITHOUT resetting progress, so a run resumes after viewing Results.
void resumeRunScreen(const char* title) {
  tft.fillScreen(COLOR_PAPER);
  drawTitle(title);
  drawButton(backButton);
  drawButton(runBtn);
  drawButton(stopBtn);
  drawButton(resetBtn);
  drawButton(resultsBtn);

  drawPanelFrame(PANEL1_LEFT, "Function f(x)", "Radius", "Turntable", 0, funcAxisMax);
  drawPanelFrame(PANEL2_LEFT, "Integral", "Rotation", "Turntable", -integralAxisMax, integralAxisMax);

  replayStatsToPanels();
}

int mapToPanelX(int left, float x) {
  float ratio = x / X_AXIS_MAX;
  if (ratio > 1) ratio = 1; if (ratio < 0) ratio = 0;
  return left + (int)(ratio * PANEL_W);
}

int mapToPanelY(float y, float yMin, float yMax) {
  float range = yMax - yMin;
  if (range == 0) range = 1;
  float ratio = (y - yMin) / range;
  if (ratio > 1) ratio = 1; if (ratio < 0) ratio = 0;
  return (PANEL_TOP + PANEL_H) - (int)(ratio * PANEL_H);
}

// Snaps a value up to a "clean" round number (1/2/2.5/5/10 x a power of
// ten) so the axis grows in sensible steps instead of jittering to an odd
// number after every single sample.
float niceAxisCeiling(float value) {
  if (value <= 0) return 1000;
  float mag = pow(10, floor(log10(value)));
  float steps[] = {1, 2, 2.5, 5, 10};
  for (int i = 0; i < 5; i++) {
    float candidate = steps[i] * mag;
    if (candidate >= value) return candidate;
  }
  return ceil(value / mag) * mag;
}

// Grows (never shrinks) the axes. Returns true if the caller must redraw and replay.
bool expandAxesIfNeeded(float fx, float integral) {
  bool changed = false;
  if (fx > funcAxisMax) {
    funcAxisMax = niceAxisCeiling(fx);
    changed = true;
  }
  float absInt = fabs(integral);
  if (absInt > integralAxisMax) {
    integralAxisMax = niceAxisCeiling(absInt);
    changed = true;
  }
  return changed;
}

// Replays stored samples at the current scale without touching stat_count/theoIntTotal.
void replayStatsToPanels() {
  lastFuncPx = -1; lastFuncPy = -1;
  lastIntPx = -1; lastIntPy = -1;

  for (int i = 0; i < stat_count; i++) {
    float x = stat_x[i];
    float fx = stat_fx[i];
    float integral = stat_exp[i];

    int fxPx = mapToPanelX(PANEL1_LEFT, x);
    int fxPy = mapToPanelY(fx, 0, funcAxisMax);
    if (lastFuncPx >= 0) tft.drawLine(lastFuncPx, lastFuncPy, fxPx, fxPy, COLOR_FUNC);
    lastFuncPx = fxPx; lastFuncPy = fxPy;

    int intPx = mapToPanelX(PANEL2_LEFT, x);
    int intPy = mapToPanelY(integral, -integralAxisMax, integralAxisMax);
    if (lastIntPx >= 0) tft.drawLine(lastIntPx, lastIntPy, intPx, intPy, COLOR_INTEGRAL);
    lastIntPx = intPx; lastIntPy = intPy;
  }

  if (stat_count > 0) {
    tft.fillRect(0, 262, 480, 20, COLOR_PAPER);
    tft.setTextColor(COLOR_INK, COLOR_PAPER);
    tft.setTextSize(1);
    tft.setCursor(20, 268);
    tft.printf("Turntable=%.0f  Radius=%.0f  Rotation=%.0f",
                stat_x[stat_count - 1], stat_fx[stat_count - 1], stat_exp[stat_count - 1]);
  }
}

void plotPoint(float x, float fx, float integral) {
  if (x < lastPlottedX) {
    tft.fillRect(PANEL1_LEFT + 1, PANEL_TOP + 1, PANEL_W - 2, PANEL_H - 2, COLOR_PAPER);
    tft.fillRect(PANEL2_LEFT + 1, PANEL_TOP + 1, PANEL_W - 2, PANEL_H - 2, COLOR_PAPER);
    lastFuncPx = -1; lastIntPx = -1;
  }

  if (x > lastX_val) {
    float dx = x - lastX_val;
    theoIntTotal += (fx * dx * MECH_CALIBRATION_CONST);
  }
  lastX_val = x;

  if (stat_count < 200) {
    stat_x[stat_count] = x;
    stat_fx[stat_count] = fx;
    stat_exp[stat_count] = integral;
    stat_theo[stat_count] = theoIntTotal;
    stat_count++;
  }
  lastPlottedX = x;

  // If the axes grew, replayStatsToPanels() has already drawn this point.
  if (expandAxesIfNeeded(fx, integral)) {
    tft.fillRect(PANEL1_LEFT + 1, PANEL_TOP + 1, PANEL_W - 2, PANEL_H - 2, COLOR_PAPER);
    tft.fillRect(PANEL2_LEFT + 1, PANEL_TOP + 1, PANEL_W - 2, PANEL_H - 2, COLOR_PAPER);
    drawPanelFrame(PANEL1_LEFT, "Function f(x)", "Radius", "Turntable", 0, funcAxisMax);
    drawPanelFrame(PANEL2_LEFT, "Integral", "Rotation", "Turntable", -integralAxisMax, integralAxisMax);
    replayStatsToPanels();
    return;
  }

  int fxPx = mapToPanelX(PANEL1_LEFT, x);
  int fxPy = mapToPanelY(fx, 0, funcAxisMax);
  if (lastFuncPx >= 0) tft.drawLine(lastFuncPx, lastFuncPy, fxPx, fxPy, COLOR_FUNC);
  lastFuncPx = fxPx; lastFuncPy = fxPy;

  int intPx = mapToPanelX(PANEL2_LEFT, x);
  int intPy = mapToPanelY(integral, -integralAxisMax, integralAxisMax);
  if (lastIntPx >= 0) tft.drawLine(lastIntPx, lastIntPy, intPx, intPy, COLOR_INTEGRAL);
  lastIntPx = intPx; lastIntPy = intPy;

  tft.fillRect(0, 262, 480, 20, COLOR_PAPER);
  tft.setTextColor(COLOR_INK, COLOR_PAPER);
  tft.setTextSize(1);
  tft.setCursor(20, 268);
  tft.printf("Turntable=%.0f  Radius=%.0f  Rotation=%.0f", x, fx, integral);
}

void sendCustomArray() {
  int lastValid = DRAW_POPUP_H;
  for (int i = 0; i < DRAW_POPUP_W; i++) {
    if (drawnCurve[i] >= 0) lastValid = drawnCurve[i];
    else drawnCurve[i] = lastValid;
  }
  Serial.print("DRAW_DATA:");
  for (int i = 0; i < DRAW_POPUP_W; i++) {
    float mappedVal = ((float)(DRAW_POPUP_H - drawnCurve[i]) / DRAW_POPUP_H) * FUNC_SEND_SCALE;
    Serial.print((int)mappedVal);
    if (i < DRAW_POPUP_W - 1) Serial.print(",");
  }
  Serial.println();
}

void generateCurve(int type) {
  for (int i = 0; i < DRAW_POPUP_W; i++) {
    if (type == 0) { // Flat
      drawnCurve[i] = DRAW_POPUP_H / 2;
    } else if (type == 1) { // Slope
      drawnCurve[i] = DRAW_POPUP_H - (i * DRAW_POPUP_H / DRAW_POPUP_W);
    } else if (type == 2) { // Curve
      float x = (float)i / DRAW_POPUP_W * 3.14159;
      drawnCurve[i] = DRAW_POPUP_H - (int)((sin(x) * 0.8 + 0.1) * DRAW_POPUP_H);
    } else if (type == 3) { // Equation
      float x_norm = (float)i / DRAW_POPUP_W;
      float val = (eqA * x_norm * x_norm) + (eqB * x_norm) + eqC;
      if (val > 100) val = 100;
      if (val < 0) val = 0;
      drawnCurve[i] = DRAW_POPUP_H - (int)((val / 100.0) * DRAW_POPUP_H);
    }
  }
  returnScreen = DRAW_RUN;
  currentScreen = DRAW_RUN;
  showRunScreen(type == 3 ? "Run Equation" : "Run Example");
}

void handleNumpadKey(String key) {
  String *target = (selectedCoeff == 0) ? &strA : ((selectedCoeff == 1) ? &strB : &strC);

  if (key == "DEL") {
    if (target->length() > 0) *target = target->substring(0, target->length() - 1);
    if (target->length() == 0 || *target == "-") *target = "0";
  }
  else if (key == "-") {
    if (!target->startsWith("-")) *target = "-" + *target;
    else *target = target->substring(1);
  }
  else {
    if (*target == "0") *target = key;
    else if (*target == "-0") *target = "-" + key;
    else if (target->length() < 5) *target += key;
  }

  eqA = strA.toInt();
  eqB = strB.toInt();
  eqC = strC.toInt();
  updateNumpadUI();
}

// ================= Setup / Loop =================
void setup() {
  Serial.begin(115200);
  pinMode(BACKLIGHT_PIN, OUTPUT);
  digitalWrite(BACKLIGHT_PIN, HIGH);

  for (int i = 0; i < DRAW_POPUP_W; i++) drawnCurve[i] = -1;

  tft.init();
  tft.setRotation(1);
  touchCalibrate();

  loadCalibFromPrefs();
  delay(500); // give the motor board time to finish its own boot/setup()
  sendCalibToMotor();

  showHome();
}

void loop() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      serialBuffer.trim();

      if (serialBuffer == "ACK_DATA") {
        waitingForAck = false;
        Serial.println("RUN");
      }
      else if (serialBuffer.startsWith("STEPSMM:")) {
        // Reply to CALIB_RECALC: adopt and save the motor board's steps/mm.
        calibParams[5].value = serialBuffer.substring(8).toFloat();
        saveCalibToPrefs();
        if (currentScreen == CALIB_MENU) updateCalibListUI();
      }
      else {
        int firstComma = serialBuffer.indexOf(',');
        int secondComma = serialBuffer.indexOf(',', firstComma + 1);
        if (firstComma > 0 && secondComma > firstComma) {
          float x = serialBuffer.substring(0, firstComma).toFloat();
          float fx = serialBuffer.substring(firstComma + 1, secondComma).toFloat();
          float integral = serialBuffer.substring(secondComma + 1).toFloat();
          if (currentScreen == DRAW_RUN) {
            plotPoint(x, fx, integral);
          }
        }
      }
      serialBuffer = "";
    } else if (c != '\r') {
      serialBuffer += c;
    }
  }

  uint16_t tx, ty;
  if (tft.getTouch(&tx, &ty)) {
    unsigned long now = millis();
    bool isClick = (now - lastTouchMillis > TOUCH_COOLDOWN_MS);

    if (currentScreen == DRAW_CANVAS) {
      if (tx >= DRAW_POPUP_X && tx < DRAW_POPUP_X + DRAW_POPUP_W && ty >= DRAW_POPUP_Y && ty < DRAW_POPUP_Y + DRAW_POPUP_H) {
        if (lastDrawTx >= 0) {
          int x1 = lastDrawTx - DRAW_POPUP_X; int y1 = lastDrawTy - DRAW_POPUP_Y;
          int x2 = tx - DRAW_POPUP_X; int y2 = ty - DRAW_POPUP_Y;
          if (x1 == x2) drawnCurve[x1] = y2;
          else {
            for (int x = min(x1, x2); x <= max(x1, x2); x++) {
              if (x >= 0 && x < DRAW_POPUP_W) drawnCurve[x] = y1 + (y2 - y1) * (x - x1) / (x2 - x1);
            }
          }
        } else { drawnCurve[tx - DRAW_POPUP_X] = ty - DRAW_POPUP_Y; }
        lastDrawTx = tx; lastDrawTy = ty;
        redrawCanvasOnly();
      }
      if (isClick && touchedButton(tx, ty, backButton)) { lastTouchMillis = now; showHome(); }
      else if (isClick && touchedButton(tx, ty, canvasClearBtn)) {
        lastTouchMillis = now;
        for (int i = 0; i < DRAW_POPUP_W; i++) drawnCurve[i] = -1;
        redrawCanvasOnly();
      }
      else if (isClick && touchedButton(tx, ty, canvasFinishBtn)) {
        lastTouchMillis = now;
        returnScreen = DRAW_RUN;
        currentScreen = DRAW_RUN;
        showRunScreen("Run Drawn Curve");
      }
    }
    else if (currentScreen == WRITE_EQ) {
      if (isClick) {
        lastTouchMillis = now;
        if (touchedButton(tx, ty, backButton)) showHome();
        else if (touchedButton(tx, ty, selABtn)) { selectedCoeff = 0; updateNumpadUI(); }
        else if (touchedButton(tx, ty, selBBtn)) { selectedCoeff = 1; updateNumpadUI(); }
        else if (touchedButton(tx, ty, selCBtn)) { selectedCoeff = 2; updateNumpadUI(); }
        else if (touchedButton(tx, ty, eqGraphBtn)) { generateCurve(3); }
        else {
          for (int i = 0; i < 12; i++) {
            if (touchedButton(tx, ty, numBtns[i])) {
              handleNumpadKey(numBtns[i].label);
              break;
            }
          }
        }
      }
    }
    else if (currentScreen == CALIB_MENU) {
      if (isClick) {
        lastTouchMillis = now;
        if (touchedButton(tx, ty, backButton)) showInfoMenu();
        else if (touchedButton(tx, ty, calibSaveBtn)) saveCalibField();
        else if (touchedButton(tx, ty, calibRecalcBtn)) Serial.println("CALIB_RECALC");
        else {
          bool handled = false;
          for (int i = 0; i < 12; i++) {
            if (touchedButton(tx, ty, calibNumBtns[i])) {
              handleCalibNumKey(calibNumBtns[i].label);
              handled = true;
              break;
            }
          }
          if (!handled) {
            for (int i = 0; i < 10; i++) {
              if (touchedButton(tx, ty, calibListBtns[i])) {
                selectedCalibIndex = i;
                calibEditBuf = formatCalibValue(i);
                updateCalibListUI();
                break;
              }
            }
          }
        }
      }
    }
    else if (currentScreen == EXAMPLES_MENU) {
      if (isClick) {
        lastTouchMillis = now;
        if (touchedButton(tx, ty, backButton)) showHome();
        else if (touchedButton(tx, ty, flatBtn)) generateCurve(0);
        else if (touchedButton(tx, ty, slopeBtn)) generateCurve(1);
        else if (touchedButton(tx, ty, curveBtn)) generateCurve(2);
      }
    }
    else if (currentScreen == DRAW_RUN) {
      if (isClick) {
        lastTouchMillis = now;
        if (touchedButton(tx, ty, backButton)) { Serial.println("STOP"); showHome(); }
        else if (touchedButton(tx, ty, runBtn)) {
          waitingForAck = true;
          sendCustomArray();
        }
        else if (touchedButton(tx, ty, stopBtn)) { Serial.println("STOP"); }
        else if (touchedButton(tx, ty, resetBtn)) {
          Serial.println("RESET");
          showRunScreen("Run");
        }
        else if (touchedButton(tx, ty, resultsBtn)) {
          Serial.println("STOP");
          showResults();
        }
      }
    }
    else if (currentScreen == HOW_IT_WORKS) {
      if (isClick && touchedButton(tx, ty, backButton)) { lastTouchMillis = now; showInfoMenu(); }
      else if (touchedButton(tx, ty, scrollUpBtn)) {
        howItWorksScrollY += 15;
        if (howItWorksScrollY > 0) howItWorksScrollY = 0;
        updateHowItWorksText(); delay(15);
      }
      else if (touchedButton(tx, ty, scrollDownBtn)) {
        howItWorksScrollY -= 15;
        if (howItWorksScrollY < -260) howItWorksScrollY = -260;
        updateHowItWorksText(); delay(15);
      }
    }
    else if (currentScreen == RESULTS) {
      if (isClick && touchedButton(tx, ty, backButton)) {
        lastTouchMillis = now;
        currentScreen = returnScreen;
        resumeRunScreen(currentRunTitle);
      }
    }
    else if (isClick) {
      lastTouchMillis = now;
      if (currentScreen == HOME) {
        if (touchedButton(tx, ty, homeButtons[0])) showExamplesMenu();
        else if (touchedButton(tx, ty, homeButtons[1])) showWriteEq();
        else if (touchedButton(tx, ty, homeButtons[2])) showDrawCanvas();
        else if (touchedButton(tx, ty, homeButtons[3])) showInfoMenu();
      }
      else if (currentScreen == INFO_MENU) {
        if (touchedButton(tx, ty, backButton)) showHome();
        else if (touchedButton(tx, ty, howItWorksBtn)) showHowItWorks();
        else if (touchedButton(tx, ty, calibMenuBtn)) showCalibMenu();
        else if (touchedButton(tx, ty, aboutBtn)) showAbout();
      }
      else if (currentScreen == ABOUT) {
        if (touchedButton(tx, ty, backButton)) showInfoMenu();
      }
    }
  } else {
    lastDrawTx = -1; lastDrawTy = -1;
  }
}
