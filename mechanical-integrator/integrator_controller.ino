/*
  Disk-and-wheel mechanical integrator: main controller (ESP32).

  A wheel rides on a turntable at radius r. With no slip, R * dtheta_w = r * dtheta, so
      wheel rotation = (1/R) * integral of r dtheta.
  Setting r = k * f(theta) makes the wheel's total rotation proportional to the integral of f.
  Negative r (wheel past the disk centre) turns the wheel backwards, giving signed area.

  Two stationary steppers drive the disk through a belt differential:
      motorA = turntable + carriage
      motorB = turntable - carriage
  Same direction spins the disk, opposite directions slide the carriage (sets r).

  The sweep is split into segments of SEG turntable steps. What the radius does inside a
  segment decides which numerical rule the machine computes:
      LEFT  set r = k f(x_i), then turn the disk     -> left Riemann sum   (error ~ h)
      MID   set r = k f(x_i + h/2), then turn        -> midpoint rule      (error ~ h^2)
      TRAP  slide r linearly while the disk turns    -> trapezoidal rule   (error ~ h^2)
  Defaults (TRAP, SEG 5) give the original smooth demo.

  NO ENDSTOPS. Carriage step 0 is wherever the carriage sits at power-on. Jog the wheel over
  the disk centre with SLIDE, then send ZERO, before any run that uses negative radius.

  The display board pushes its own saved calibration (CALIB:...) on every boot, which
  overwrites this board's values. Fix calibration on the display, not only here.

  Serial commands (USB or display):
    RUN [FLAT|SLOPE|CURVE|DRAWN|SQUARE|SINE]  start / resume a sweep
    STOP                                       pause
    RESET                                      return carriage, zero integral
    RULE LEFT|MID|TRAP                         integration rule
    SEG <steps>                                segment size h in turntable steps
    XMAX <steps>                               sweep length in turntable steps
    STATUS                                     print current settings
    NEG ON|OFF                                 allow negative radius (default off)
    TURN <steps>                               rotate disk only (calibration)
    SLIDE <steps>                              move disk carriage only (calibration)
    ZERO                                       set current carriage position as r = 0
    CALIB:<10 comma-separated values>          set calibration
    CALIB_RECALC                               recompute steps/mm
    DRAW_DATA:<v1,v2,...>                      load a drawn curve

  At the end of every sweep it prints (USB serial only):
    RESULT,<rule>,<function>,<segSteps>,<segments>,<ticks>,<wheel_rad>,<clips>
  A non-zero clips count means the radius hit its limit and that run is not a valid integral.
*/

#include <Wire.h>
#include <Preferences.h>
#include <math.h>

// Enums must come before any function. The Arduino IDE generates function
// prototypes near the top of the file, and they need these types to exist.
enum Mode { FLAT, SLOPE, CURVE, DRAWN, SQUARE, SINE };
enum Rule { LEFT, MID, TRAP };

// ================= MOTOR PINS =================
#define STEP_A_PIN 18
#define DIR_A_PIN  5
#define STEP_B_PIN 19
#define DIR_B_PIN  21

// ================= AS5600 (integrator wheel) =================
#define SDA_PIN 15
#define SCL_PIN 4
#define AS5600_ADDRESS 0x36
#define DISPLAY_RX_PIN 14   // connected to Display TX
#define DISPLAY_TX_PIN 13   // connected to Display RX

// ================= RUN CONFIG =================
long xMax     = 10240;   // sweep length in turntable steps (divides evenly by 8, 16, 32, 64)
long segSteps = 5;       // segment size h in turntable steps (5 = smooth demo)

const float FUNC_PEAK   = 4000.0;
const float PEAK_X_FRAC = 0.45;

// Negative radius = disk carriage moves past the point where the wheel sits
// over the disk centre. Needs carriage step 0 to be the exact centre
// (use SLIDE + ZERO to set it). Turn on with NEG ON. Needed for SINE.
bool allowNegativeRadius = false;

const int STEP_PULSE_US = 5;
const int STEP_DELAY_US = 600;
const int I2C_POLL_EVERY_N_STEPS = 20;
int stepCounter = 0;

const float DISPLAY_RADIUS_MAX = 8000.0;   // must match the display's FUNC_SEND_SCALE

// ================= CALIBRATION =================
struct CalibConfig {
  float beltPitch_mm;
  float pulleyTeeth;
  float differentialRatio;
  float diskRadius_mm;
  float maxCarriageTravel_mm;
  float stepsPerMM;
  float microstepping;
  float encoderCountsPerRev;
  float wheelRadius_mm;
  float frictionCoefficient;   // only scales the live display value; RESULT is always raw
};

CalibConfig calib = {
  3.0, 20, 1.0, 100.0, 90.0, 106.6667, 32, 4096, 25.6, 1.0   // GT3: 3 mm pitch
};

Preferences prefs;
const char* calibPrefKeys[10] = {"bp","pt","dr","dk","mt","sm","ms","ec","wr","fr"};

float* calibFields[10] = {
  &calib.beltPitch_mm, &calib.pulleyTeeth, &calib.differentialRatio,
  &calib.diskRadius_mm, &calib.maxCarriageTravel_mm, &calib.stepsPerMM,
  &calib.microstepping, &calib.encoderCountsPerRev, &calib.wheelRadius_mm,
  &calib.frictionCoefficient
};

void saveCalibToPrefs() {
  prefs.begin("calib", false);
  for (int i = 0; i < 10; i++) prefs.putFloat(calibPrefKeys[i], *calibFields[i]);
  prefs.end();
}

void loadCalibFromPrefs() {
  prefs.begin("calib", true);
  for (int i = 0; i < 10; i++) {
    if (prefs.isKey(calibPrefKeys[i])) *calibFields[i] = prefs.getFloat(calibPrefKeys[i], *calibFields[i]);
  }
  prefs.end();
}

void parseCalibData(const String &data) {
  float vals[10];
  for (int i = 0; i < 10; i++) vals[i] = *calibFields[i];   // keep old value if a field is missing
  int start = 0;
  for (int i = 0; i < 10 && start < (int)data.length(); i++) {
    int comma = data.indexOf(',', start);
    String piece = (comma == -1) ? data.substring(start) : data.substring(start, comma);
    vals[i] = piece.toFloat();
    start = (comma == -1) ? data.length() : comma + 1;
  }
  for (int i = 0; i < 10; i++) *calibFields[i] = vals[i];
  saveCalibToPrefs();
  Serial.println("Calibration updated.");
}

float computeStepsPerMM() {
  float travelPerRev = calib.pulleyTeeth * calib.beltPitch_mm;
  if (travelPerRev <= 0) return calib.stepsPerMM;
  return (200.0 * calib.microstepping) / travelPerRev;
}

// ================= STATE =================
long xPos = 0;
long lastCarriageSteps = 0;
long integralTicks = 0;
int  lastRawAngle = -1;
bool running = false;
bool runPrepared = false;      // true once a sweep has been positioned and zeroed
long segmentsDone = 0;
long clipCount = 0;
unsigned long lastPrint = 0;
const unsigned long PRINT_INTERVAL_MS = 100;

Mode currentMode = CURVE;
Rule rule = TRAP;

const int MAX_CUSTOM_SAMPLES = 250;
float customCurve[MAX_CUSTOM_SAMPLES];
int customCurveCount = 0;

const char* modeName(Mode m) {
  switch (m) {
    case FLAT: return "FLAT";   case SLOPE: return "SLOPE";
    case CURVE: return "CURVE"; case DRAWN: return "DRAWN";
    case SQUARE: return "SQUARE"; case SINE: return "SINE";
  }
  return "?";
}

const char* ruleName(Rule r) {
  switch (r) {
    case LEFT: return "LEFT"; case MID: return "MID"; case TRAP: return "TRAP";
  }
  return "?";
}

// ================= FUNCTIONS =================
// x is in turntable steps, 0..xMax. Float so the midpoint rule can sample half-steps.
float functionValue(float x) {
  float u = x / (float)xMax;            // 0..1 across the sweep
  if (u < 0) u = 0;
  if (u > 1) u = 1;

  switch (currentMode) {
    case FLAT:  return FUNC_PEAK * 0.5;
    case SLOPE: return FUNC_PEAK * u;
    case CURVE: {
      float xp = xMax * PEAK_X_FRAC;
      if (xp <= 0) return 0;
      float ratio = x / xp;
      return FUNC_PEAK * ratio * exp(1.0 - ratio);
    }
    case SQUARE:                         // theta^2 on [0, 2pi], rescaled: theta = 2*pi*u
      return FUNC_PEAK * u * u;
    case SINE:                           // sin(theta) on [0, 3pi/2]: theta = 1.5*pi*u
      return FUNC_PEAK * sin(1.5 * PI * u);
    case DRAWN: {
      if (customCurveCount <= 0) return 0;
      float idxF = u * (customCurveCount - 1);
      int idx0 = (int)idxF;
      int idx1 = min(idx0 + 1, customCurveCount - 1);
      float frac = idxF - idx0;
      return customCurve[idx0] * (1.0 - frac) + customCurve[idx1] * frac;
    }
  }
  return 0;
}

void parseDrawData(const String &data) {
  customCurveCount = 0;
  int start = 0;
  while (start < (int)data.length() && customCurveCount < MAX_CUSTOM_SAMPLES) {
    int comma = data.indexOf(',', start);
    String piece = (comma == -1) ? data.substring(start) : data.substring(start, comma);
    if (piece.length() > 0) customCurve[customCurveCount++] = piece.toFloat();
    if (comma == -1) break;
    start = comma + 1;
  }
}

long radiusToSteps(float rawRadius) {
  float norm = rawRadius / DISPLAY_RADIUS_MAX;
  float lo = allowNegativeRadius ? -1.0 : 0.0;
  bool clipped = false;
  if (norm < lo)  { norm = lo;  clipped = true; }
  if (norm > 1.0) { norm = 1.0; clipped = true; }

  float radius_mm = norm * calib.diskRadius_mm;
  if (radius_mm >  calib.maxCarriageTravel_mm) { radius_mm =  calib.maxCarriageTravel_mm; clipped = true; }
  if (radius_mm < -calib.maxCarriageTravel_mm) { radius_mm = -calib.maxCarriageTravel_mm; clipped = true; }

  if (clipped) clipCount++;
  return (long)round(radius_mm * calib.stepsPerMM);
}

// ================= AS5600 =================
int readAS5600Raw() {
  Wire.beginTransmission(AS5600_ADDRESS);
  Wire.write(0x0C);
  if (Wire.endTransmission(false) != 0) return -1;
  Wire.requestFrom(AS5600_ADDRESS, 2);
  if (Wire.available() == 2) {
    int high = Wire.read(), low = Wire.read();
    return ((high << 8) | low) & 0x0FFF;
  }
  return -1;
}

void updateIntegral() {
  int raw = readAS5600Raw();
  if (raw < 0) return;
  if (lastRawAngle < 0) { lastRawAngle = raw; return; }
  int delta = raw - lastRawAngle;
  int half = (int)calib.encoderCountsPerRev / 2;
  if (delta >  half) delta -= (int)calib.encoderCountsPerRev;
  if (delta < -half) delta += (int)calib.encoderCountsPerRev;
  integralTicks += delta;
  lastRawAngle = raw;
}

void zeroIntegral() {
  integralTicks = 0;
  lastRawAngle = -1;
  updateIntegral();          // takes a fresh baseline reading
}

// ================= MOTOR =================
void stepPulse(int pin) {
  digitalWrite(pin, HIGH);
  delayMicroseconds(STEP_PULSE_US);
  digitalWrite(pin, LOW);
}

// Same direction on both motors = disk rotation, opposite = carriage translation.
void moveDifferential(long turntableSteps, long carriageSteps) {
  long stepsA = turntableSteps + carriageSteps;
  long stepsB = turntableSteps - carriageSteps;

  digitalWrite(DIR_A_PIN, stepsA >= 0 ? HIGH : LOW);
  digitalWrite(DIR_B_PIN, stepsB >= 0 ? HIGH : LOW);

  unsigned long nA = labs(stepsA), nB = labs(stepsB);
  unsigned long maxSteps = max(nA, nB);
  if (maxSteps == 0) return;

  long errA = 0, errB = 0;
  for (unsigned long i = 0; i < maxSteps; i++) {
    errA += nA;
    if (errA >= (long)maxSteps) { errA -= maxSteps; stepPulse(STEP_A_PIN); }
    errB += nB;
    if (errB >= (long)maxSteps) { errB -= maxSteps; stepPulse(STEP_B_PIN); }
    delayMicroseconds(STEP_DELAY_US);
    if (++stepCounter >= I2C_POLL_EVERY_N_STEPS) {
      stepCounter = 0;
      updateIntegral();
    }
  }
  // Always read the wheel at the end of a move so no motion is missed.
  stepCounter = 0;
  updateIntegral();
}

// ================= SWEEP =================
void prepareRun() {
  // Put the carriage at f(0) with the disk still, then zero the wheel.
  long tgt = radiusToSteps(functionValue(0));
  moveDifferential(0, tgt - lastCarriageSteps);
  lastCarriageSteps = tgt;
  xPos = 0;
  segmentsDone = 0;
  clipCount = 0;
  zeroIntegral();
  runPrepared = true;
}

void runSegment() {
  long seg = min(segSteps, xMax - xPos);           // last segment may be shorter
  long turn = (long)(seg * calib.differentialRatio);

  if (rule == TRAP) {
    // Radius ramps from f(x_i) to f(x_i + h) while the disk turns: trapezoidal rule.
    long tgt = radiusToSteps(functionValue(xPos + seg));
    moveDifferential(turn, tgt - lastCarriageSteps);
    lastCarriageSteps = tgt;
  } else {
    // Set the radius with the disk still, then turn the disk at fixed radius.
    float sampleX = (rule == LEFT) ? (float)xPos : xPos + seg / 2.0;
    long tgt = radiusToSteps(functionValue(sampleX));
    moveDifferential(0, tgt - lastCarriageSteps);
    lastCarriageSteps = tgt;
    moveDifferential(turn, 0);
  }

  xPos += seg;
  segmentsDone++;
}

void finishRun() {
  running = false;
  runPrepared = false;
  updateIntegral();
  float wheelRad = integralTicks * 2.0 * PI / calib.encoderCountsPerRev;
  Serial.println("Sweep complete.");
  Serial.printf("RESULT,%s,%s,%ld,%ld,%ld,%.5f,%ld\n",
                ruleName(rule), modeName(currentMode), segSteps,
                segmentsDone, integralTicks, wheelRad, clipCount);
  if (clipCount > 0) {
    Serial.println("WARNING: radius was clipped; this result is not a valid integral.");
  }
}

void printStatus() {
  Serial.printf("STATUS rule=%s mode=%s seg=%ld xmax=%ld segments=%ld negRadius=%s\n",
                ruleName(rule), modeName(currentMode), segSteps, xMax,
                (xMax + segSteps - 1) / segSteps, allowNegativeRadius ? "on" : "off");
  Serial.printf("CALIB pitch=%.2f teeth=%.0f diffRatio=%.3f disk=%.1f travel=%.1f steps/mm=%.3f wheelR=%.2f\n",
                calib.beltPitch_mm, calib.pulleyTeeth, calib.differentialRatio,
                calib.diskRadius_mm, calib.maxCarriageTravel_mm, calib.stepsPerMM,
                calib.wheelRadius_mm);
}

// ================= COMMANDS =================
void handleCommand(String cmd) {
  cmd.trim();
  if (cmd.length() == 0) return;
  cmd.toUpperCase();

  if (cmd.startsWith("CALIB:")) {
    parseCalibData(cmd.substring(6));
  }
  else if (cmd == "CALIB_RECALC") {
    calib.stepsPerMM = computeStepsPerMM();
    saveCalibToPrefs();
    Serial2.println("STEPSMM:" + String(calib.stepsPerMM, 4));
    Serial.println("Recalculated steps/mm: " + String(calib.stepsPerMM, 4));
  }
  else if (cmd.startsWith("DRAW_DATA:")) {
    parseDrawData(cmd.substring(10));
    currentMode = DRAWN;
    Serial.println("Received curve: " + String(customCurveCount) + " samples");
    Serial2.println("ACK_DATA");
  }
  else if (cmd.startsWith("RULE")) {
    if (running) { Serial.println("Stop the sweep before changing RULE."); return; }
    String r = cmd.substring(4); r.trim();
    if (r == "LEFT") rule = LEFT;
    else if (r == "MID") rule = MID;
    else if (r == "TRAP") rule = TRAP;
    else { Serial.println("Use RULE LEFT, RULE MID or RULE TRAP."); return; }
    Serial.println(String("Rule: ") + ruleName(rule));
  }
  else if (cmd.startsWith("SEG")) {
    if (running) { Serial.println("Stop the sweep before changing SEG."); return; }
    long v = cmd.substring(3).toInt();
    if (v <= 0) { Serial.println("SEG must be a positive number of steps."); return; }
    segSteps = v;
    Serial.printf("Segment size: %ld steps (%ld segments)\n", segSteps, (xMax + segSteps - 1) / segSteps);
  }
  else if (cmd.startsWith("XMAX")) {
    if (running) { Serial.println("Stop the sweep before changing XMAX."); return; }
    long v = cmd.substring(4).toInt();
    if (v <= 0) { Serial.println("XMAX must be a positive number of steps."); return; }
    xMax = v;
    Serial.printf("Sweep length: %ld steps\n", xMax);
  }
  else if (cmd == "STATUS") {
    printStatus();
  }
  else if (cmd.startsWith("NEG")) {
    if (running) { Serial.println("Stop the sweep before changing NEG."); return; }
    String v = cmd.substring(3); v.trim();
    if (v == "ON") allowNegativeRadius = true;
    else if (v == "OFF") allowNegativeRadius = false;
    else { Serial.println("Use NEG ON or NEG OFF."); return; }
    Serial.println(String("Negative radius: ") + (allowNegativeRadius ? "on" : "off"));
  }
  // ---- Calibration / setup moves (only while stopped) ----
  else if (cmd.startsWith("TURN")) {          // TURN <steps>: rotate disk only
    if (running) { Serial.println("Stop the sweep first."); return; }
    long v = cmd.substring(4).toInt();
    moveDifferential(v, 0);
    Serial.printf("Turned disk %ld steps. Wheel ticks so far: %ld\n", v, integralTicks);
  }
  else if (cmd.startsWith("SLIDE")) {         // SLIDE <carriage steps>: move disk carriage only
    if (running) { Serial.println("Stop the sweep first."); return; }
    long v = cmd.substring(5).toInt();
    moveDifferential(0, v);
    lastCarriageSteps += v;
    Serial.printf("Carriage at %ld steps (%.2f mm from zero)\n",
                  lastCarriageSteps, lastCarriageSteps / calib.stepsPerMM);
  }
  else if (cmd == "ZERO") {                   // current carriage position becomes r = 0
    if (running) { Serial.println("Stop the sweep first."); return; }
    lastCarriageSteps = 0;
    runPrepared = false;
    Serial.println("Carriage position set as r = 0 (wheel over disk centre).");
  }
  else if (cmd.startsWith("RUN")) {
    if (cmd == "RUN FLAT") currentMode = FLAT;
    else if (cmd == "RUN SLOPE") currentMode = SLOPE;
    else if (cmd == "RUN CURVE") currentMode = CURVE;
    else if (cmd == "RUN DRAWN") currentMode = DRAWN;
    else if (cmd == "RUN SQUARE") currentMode = SQUARE;
    else if (cmd == "RUN SINE") currentMode = SINE;
    if (!runPrepared) prepareRun();     // fresh sweep: position carriage and zero wheel
    running = true;
    Serial.println("Started: " + cmd + " rule=" + ruleName(rule) + " seg=" + String(segSteps));
  }
  else if (cmd == "STOP") {
    running = false;
    Serial.println("Stopped. Send RUN to resume or RESET to start over.");
  }
  else if (cmd == "RESET") {
    running = false;
    runPrepared = false;
    moveDifferential(0, -lastCarriageSteps);
    xPos = 0;
    lastCarriageSteps = 0;
    segmentsDone = 0;
    clipCount = 0;
    integralTicks = 0;
    lastRawAngle = -1;
    Serial.println("Reset. Carriage returned to start.");
  }
  else {
    Serial.println("Unknown command: " + cmd);
  }
}

void pollPort(Stream &port) {
  while (port.available()) {
    handleCommand(port.readStringUntil('\n'));
  }
}

// ================= SETUP =================
void setup() {
  Serial.begin(115200);
  pinMode(STEP_A_PIN, OUTPUT); pinMode(DIR_A_PIN, OUTPUT);
  pinMode(STEP_B_PIN, OUTPUT); pinMode(DIR_B_PIN, OUTPUT);
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(10000);
  Wire.setTimeOut(20);
  Serial2.begin(115200, SERIAL_8N1, DISPLAY_RX_PIN, DISPLAY_TX_PIN);
  loadCalibFromPrefs();
  delay(300);
  Serial.println("Mechanical Integrator ready.");
  printStatus();
}

// ================= LOOP =================
void loop() {
  pollPort(Serial);
  pollPort(Serial2);

  if (running) {
    if (xPos < xMax) runSegment();
    if (xPos >= xMax) finishRun();
  }

  unsigned long now = millis();
  if (now - lastPrint >= PRINT_INTERVAL_MS) {
    lastPrint = now;
    float fx = functionValue(xPos);
    long reportedIntegral = (long)round(integralTicks * calib.frictionCoefficient);
    Serial.printf("%ld,%.2f,%ld\n", xPos, fx, reportedIntegral);
    Serial2.printf("%ld,%.2f,%ld\n", xPos, fx, reportedIntegral);
  }
}
