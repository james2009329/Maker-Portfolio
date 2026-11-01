/*
  Holonomic differential swerve drivetrain — 8x NEMA17 steppers, 4 modules, ESP32.

  Each module is driven by two steppers through a differential:
      motorA = drive + steer
      motorB = drive - steer
  Equal speeds drive the wheel; opposite speeds rotate the module.

  The robot takes a velocity vector (vx, vy) and a rotation rate (omega). Each module
  solves for its own speed and heading:
      module_vx   = vx - omega * y
      module_vy   = vy + omega * x
      wheel speed = hypot(module_vx, module_vy)
      wheel angle = atan2(module_vy, module_vx)

  NO AZIMUTH FEEDBACK. There are no encoders on the module pivots. Headings are tracked by
  dead reckoning — the firmware integrates the steer rate it commands and assumes the motors
  complied. Consequences:
    1. All four modules must be physically pointed forward at power-on, and again after any
       calibration spin test, since the tests move them.
    2. A skipped step or a bumped module puts the estimate permanently out of sync with
       reality. Re-zero from the menu. An AS5600 per module is the real fix.

  Gear ratios: drive 1.969:1 (0.166292 m of travel per motor revolution),
  steering 15:128 (8.533 motor revolutions per module revolution).

  JavaScript in the embedded page must use arrow functions, const f = () => {}, and never
  the older declaration keyword. The Arduino IDE's prototype scanner does not know the HTML
  is inside a string, and reads such a declaration as C++, which breaks the build.

  Libraries: FastAccelStepper, AsyncTCP, ESPAsyncWebServer, ArduinoJson.
*/

#include "FastAccelStepper.h"
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <math.h>

#define FULL_STEPS_PER_REV   200
#define MICROSTEPPING        32
#define STEPS_PER_REV        ((long)FULL_STEPS_PER_REV * MICROSTEPPING)

const float DRIVE_GEAR_RATIO      = 1.969f;
const float WHEEL_CIRCUMFERENCE_M = 0.327429f;
const float DIST_PER_MOTOR_REV_M  = 0.166292f;
const float STEER_GEAR_RATIO      = 128.0f / 15.0f;

const float WHEELBASE_M   = 0.290f;
const float TRACK_WIDTH_M = 0.290f;

#define NUM_MODULES 4

const float MODULE_X[NUM_MODULES] = { +WHEELBASE_M/2, +WHEELBASE_M/2, -WHEELBASE_M/2, -WHEELBASE_M/2 };
const float MODULE_Y[NUM_MODULES] = { +TRACK_WIDTH_M/2, -TRACK_WIDTH_M/2, +TRACK_WIDTH_M/2, -TRACK_WIDTH_M/2 };

// Stated in motor revolutions/s^2 so it scales with MICROSTEPPING. A hardcoded steps/s^2
// value silently becomes 32x too weak at 1/32 microstepping, and modules then cannot reach
// their commanded steer rate before the command decays.
const float ACCEL_REV_PER_S2      = 100.0f;
const uint32_t ACCEL_STEPS_PER_S2 = (uint32_t)(ACCEL_REV_PER_S2 * STEPS_PER_REV);

const float MAX_STEP_RATE_HZ      = 66000.0f;
const float MAX_DRIVE_SPEED_MPS   = 1.0f;
const float MAX_ANGULAR_RATE_DPS  = 180.0f;
const float MAX_MODULE_STEER_DPS  = 60.0f;

const float CAL_TEST_SPEED_MPS    = 0.12f;
const float CAL_TEST_STEER_DPS    = 10.0f;
const unsigned long CAL_TEST_MS   = 1200;

const float STEER_KP = 1.0f;

const unsigned long COMMAND_TIMEOUT_MS = 500;

const char* AP_SSID     = "Differential-Swerve";
const char* AP_PASSWORD = "swerve123";

struct MotorPins { uint8_t step; uint8_t dir; };

MotorPins modulePins[NUM_MODULES][2] = {
  { {13, 2},  {14, 5}  },
  { {27, 12}, {26, 15} },
  { {25, 19}, {33, 21} },
  { {4,  22}, {18, 23} },
};

// Two index spaces, deliberately kept separate:
//   hardware index (hw)   0-3 = a row of modulePins = one physical pair of motors/drivers.
//                         motorA[], motorB[], driveInvert[], steerInvert[] use this.
//   logical position (m)  0-3 = front-left, front-right, back-left, back-right.
//                         MODULE_X[], MODULE_Y[], estimatedAngle[] use this.
// moduleMap[m] = hw is the bridge, set by calibration. driveSwerve() is the only place
// the conversion happens.
uint8_t moduleMap[NUM_MODULES]   = { 0, 1, 2, 3 };
int8_t  driveInvert[NUM_MODULES] = { 1, 1, 1, 1 };
int8_t  steerInvert[NUM_MODULES] = { 1, 1, 1, 1 };
bool    calibrated = false;

Preferences prefs;

void setModuleVelocity(int hw, float wheelSpeed_mps, float steerRate_dps);
void runMotorAtRps(FastAccelStepper* motor, float rps);
void stopAllModules();
void setupWebServer();
void fullStop();
void zeroModuleAngles();
void driveSwerve(float vx, float vy, float omega_dps, float dt);
float wrapAngleDeg(float a);
void loadCalibration();
void saveCalibration();
void sendCalState(AsyncWebSocketClient* client);
void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type,
               void *arg, uint8_t *data, size_t len);

FastAccelStepperEngine engine = FastAccelStepperEngine();
FastAccelStepper* motorA[NUM_MODULES];
FastAccelStepper* motorB[NUM_MODULES];

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

enum ControlMode { MODE_MENU, MODE_JOYSTICK, MODE_ARROWS, MODE_NUMERIC, MODE_CAL };

volatile float g_vx    = 0.0f;
volatile float g_vy    = 0.0f;
volatile float g_omega = 0.0f;

float estimatedAngle[NUM_MODULES] = { 0.0f, 0.0f, 0.0f, 0.0f };

volatile bool  g_stopped = true;
volatile unsigned long g_lastCommandMillis = 0;
volatile ControlMode g_mode = MODE_MENU;
volatile float g_speedScale = 1.0f;

volatile bool g_numericRunning = false;
volatile unsigned long g_numericStopAt = 0;

volatile bool g_testActive = false;
volatile int  g_testHw = -1;
volatile bool g_testIsSteer = false;
volatile unsigned long g_testStopAt = 0;

unsigned long g_lastLoopMicros = 0;

void fullStop() {
  g_stopped = true;
  g_vx = 0.0f;
  g_vy = 0.0f;
  g_omega = 0.0f;
  g_numericRunning = false;
  g_testActive = false;
  g_testHw = -1;
}

void zeroModuleAngles() {
  for (int m = 0; m < NUM_MODULES; m++) estimatedAngle[m] = 0.0f;
}

void loadCalibration() {
  prefs.begin("swerve", true);
  calibrated = prefs.getBool("caldone", false);
  if (calibrated) {
    for (int i = 0; i < NUM_MODULES; i++) {
      char k[8];
      snprintf(k, sizeof(k), "map%d", i);  moduleMap[i]   = prefs.getUChar(k, i);
      snprintf(k, sizeof(k), "dinv%d", i); driveInvert[i] = prefs.getChar(k, 1);
      snprintf(k, sizeof(k), "sinv%d", i); steerInvert[i] = prefs.getChar(k, 1);
    }
  }
  prefs.end();

  bool seen[NUM_MODULES] = { false, false, false, false };
  bool ok = true;
  for (int i = 0; i < NUM_MODULES; i++) {
    if (moduleMap[i] >= NUM_MODULES || seen[moduleMap[i]]) { ok = false; break; }
    seen[moduleMap[i]] = true;
  }
  if (!ok) {
    Serial.println("Stored module map was invalid — reverting to 1,2,3,4.");
    for (int i = 0; i < NUM_MODULES; i++) moduleMap[i] = i;
    calibrated = false;
  }
}

void saveCalibration() {
  prefs.begin("swerve", false);
  for (int i = 0; i < NUM_MODULES; i++) {
    char k[8];
    snprintf(k, sizeof(k), "map%d", i);  prefs.putUChar(k, moduleMap[i]);
    snprintf(k, sizeof(k), "dinv%d", i); prefs.putChar(k, driveInvert[i]);
    snprintf(k, sizeof(k), "sinv%d", i); prefs.putChar(k, steerInvert[i]);
  }
  prefs.putBool("caldone", true);
  prefs.end();
  calibrated = true;
  Serial.println("Calibration saved to NVS.");
}

void sendCalState(AsyncWebSocketClient* client) {
  if (!client) return;
  StaticJsonDocument<256> doc;
  doc["type"] = "calstate";
  doc["calibrated"] = calibrated;
  JsonArray mp = doc.createNestedArray("map");
  JsonArray di = doc.createNestedArray("dinv");
  JsonArray si = doc.createNestedArray("sinv");
  for (int i = 0; i < NUM_MODULES; i++) {
    mp.add(moduleMap[i]);
    di.add(driveInvert[i]);
    si.add(steerInvert[i]);
  }
  String out;
  serializeJson(doc, out);
  client->text(out);
}

const char index_html[] PROGMEM = R"HTMLPAGE(
<!DOCTYPE html>
<html>
<head>
<meta name="viewport" content="width=device-width, initial-scale=1, maximum-scale=1, user-scalable=no">
<title>Swerve Control</title>
<style>
  * { box-sizing: border-box; -webkit-tap-highlight-color: transparent; user-select: none; }
  body { font-family: -apple-system, sans-serif; background:#111; color:#eee; margin:0;
         padding:0 12px 24px; }

  #topbar { position: sticky; top:0; background:#111; padding:10px 0; border-bottom:1px solid #333;
            z-index: 10; }
  #titleRow { display:flex; justify-content:space-between; align-items:center; }
  #titleRow h1 { font-size:18px; margin:0; }
  #connStatus { font-size:12px; color:#888; }
  #connStatus.connected { color:#4caf50; }
  #connStatus.disconnected { color:#e74c3c; }
  #infoRow { display:flex; justify-content:space-between; font-size:13px; color:#aaa; margin-top:4px; }
  #stopBtn { width:100%; background:#c0392b; color:white; border:none; padding:14px;
             font-size:18px; font-weight:bold; border-radius:8px; margin-top:8px; }

  .screen { padding-top: 14px; }
  h3 { font-size:14px; color:#999; margin: 18px 0 8px; text-transform:uppercase; letter-spacing:0.05em; }

  .speedRow, .modeCol { display:flex; gap:8px; }
  .modeCol { flex-direction:column; }
  .speedBtn, .modeBtn, .backBtn { flex:1; background:#222; color:#eee; border:1px solid #333;
             padding:14px; font-size:16px; border-radius:8px; }
  .speedBtn.selected { background:#2d6cdf; border-color:#2d6cdf; }
  .modeBtn { padding:20px; font-size:18px; text-align:left; }
  .backBtn { width:100%; margin-bottom:12px; text-align:left; }

  #zeroBtn, #recalBtn { width:100%; background:#222; color:#eee; border:1px solid #444;
             padding:14px; font-size:15px; border-radius:8px; margin-top:8px; }
  .warn { font-size:12px; color:#8a8a8a; line-height:1.4; margin-top:8px; }
  .warnStrong { font-size:13px; color:#e0a030; line-height:1.4; margin-top:8px; }

  .joyRow { display:flex; justify-content:space-around; margin-top:20px; }
  .joyBox { text-align:center; }
  .joyLabel { color:#999; font-size:13px; margin-bottom:8px; }
  .joystickZone { width:150px; height:150px; border-radius:50%; background:#222;
                  position:relative; touch-action:none; }
  .stick { width:50px; height:50px; border-radius:50%; background:#4caf50; position:absolute;
           left:50px; top:50px; }

  .arrowGrid { display:grid; grid-template-columns: 1fr 1fr 1fr; gap:8px; margin-top:20px; }
  .arrowBtn { background:#222; color:#eee; border:1px solid #333; border-radius:8px;
              font-size:28px; padding:22px 0; }
  .arrowCenter { display:flex; align-items:center; justify-content:center; color:#555; font-size:20px; }
  .rotRow { display:flex; gap:8px; margin-top:12px; }
  .arrowBtn.wide { font-size:16px; padding:18px 0; }

  .numRow { background:#1a1a1a; border-radius:10px; padding:14px; margin-top:14px; }
  .numRow label { display:block; color:#999; font-size:13px; margin-bottom:6px; }
  .numRow input { width:100%; background:#000; color:#eee; border:1px solid #333; border-radius:6px;
                  padding:10px; font-size:16px; margin-bottom:10px; }
  .btnRow { display:flex; gap:8px; }
  .btnRow button { flex:1; background:#2d6cdf; color:white; border:none; border-radius:8px;
                   padding:12px; font-size:15px; }

  /* calibration */
  #chassis { display:block; margin: 6px auto 0; }
  .posCard { background:#1a1a1a; border-radius:10px; padding:12px; margin-top:10px; }
  .posHead { display:flex; justify-content:space-between; align-items:center; margin-bottom:10px; }
  .posName { font-size:16px; font-weight:bold; }
  .posSel { background:#000; color:#eee; border:1px solid #444; border-radius:6px;
            padding:8px; font-size:15px; }
  .calBtnRow { display:flex; gap:8px; }
  .calBtnRow button { flex:1; background:#222; color:#eee; border:1px solid #444;
                      border-radius:8px; padding:12px; font-size:14px; }
  .calBtnRow button.spin { background:#2d6cdf; border-color:#2d6cdf; color:#fff; }
  .calBtnRow button.rev { background:#8e44ad; border-color:#8e44ad; color:#fff; }
  #saveCalBtn { width:100%; background:#27ae60; color:white; border:none; padding:16px;
                font-size:17px; font-weight:bold; border-radius:8px; margin-top:16px; }
  #mapWarn { color:#e74c3c; font-size:13px; margin-top:8px; min-height:18px; }
</style>
</head>
<body>

<div id="topbar">
  <div id="titleRow">
    <h1>Swerve Control</h1>
    <span id="connStatus">connecting...</span>
  </div>
  <div id="infoRow">
    <span id="speedLabel">Speed: 100%</span>
    <span id="modeLabel">Menu</span>
  </div>
  <button id="stopBtn">STOP</button>
</div>

<!-- ===================== CALIBRATION ===================== -->
<div id="screen-cal" class="screen" style="display:none">
  <h3>Calibration</h3>

  <svg id="chassis" width="230" height="230" viewBox="0 0 230 230">
    <rect x="55" y="45" width="120" height="140" rx="10" fill="#1d1d1d" stroke="#3a3a3a"/>
    <polygon points="115,18 106,36 124,36" fill="#4caf50"/>
    <text x="115" y="14" fill="#4caf50" font-size="11" text-anchor="middle">FRONT</text>

    <circle cx="55"  cy="70"  r="24" fill="#262626" stroke="#4a4a4a"/>
    <text id="lblFL" x="55"  y="75"  fill="#eee" font-size="16" text-anchor="middle">1</text>
    <text x="55"  y="36"  fill="#888" font-size="11" text-anchor="middle">FL</text>

    <circle cx="175" cy="70"  r="24" fill="#262626" stroke="#4a4a4a"/>
    <text id="lblFR" x="175" y="75"  fill="#eee" font-size="16" text-anchor="middle">2</text>
    <text x="175" y="36"  fill="#888" font-size="11" text-anchor="middle">FR</text>

    <circle cx="55"  cy="160" r="24" fill="#262626" stroke="#4a4a4a"/>
    <text id="lblBL" x="55"  y="165" fill="#eee" font-size="16" text-anchor="middle">3</text>
    <text x="55"  y="200" fill="#888" font-size="11" text-anchor="middle">BL</text>

    <circle cx="175" cy="160" r="24" fill="#262626" stroke="#4a4a4a"/>
    <text id="lblBR" x="175" y="165" fill="#eee" font-size="16" text-anchor="middle">4</text>
    <text x="175" y="200" fill="#888" font-size="11" text-anchor="middle">BR</text>
  </svg>

  <div class="warn">
    Press SPIN on a corner and watch which module actually moves. If the wrong one moves,
    change that corner's driver pair until it matches. Then use the REV buttons so every
    module drives forward and steers the same way.
  </div>

  <div id="calCards"></div>

  <div id="mapWarn"></div>
  <button id="saveCalBtn">Save calibration</button>

  <div class="warnStrong">
    The spin tests move the modules. Before driving, physically point all four straight
    forward again and press Re-zero in the menu — there are no encoders, so the firmware
    cannot tell where they ended up.
  </div>
</div>

<!-- ===================== MENU ===================== -->
<div id="screen-menu" class="screen" style="display:none">
  <h3>Speed</h3>
  <div class="speedRow">
    <button class="speedBtn" data-v="0.25">25%</button>
    <button class="speedBtn" data-v="0.5">50%</button>
    <button class="speedBtn" data-v="0.75">75%</button>
    <button class="speedBtn selected" data-v="1.0">100%</button>
  </div>

  <h3>Control Mode</h3>
  <div class="modeCol">
    <button class="modeBtn" data-mode="joystick">Joystick</button>
    <button class="modeBtn" data-mode="arrows">Arrow Controls</button>
    <button class="modeBtn" data-mode="numeric">Numerical Control</button>
  </div>

  <h3>Setup</h3>
  <button id="zeroBtn">Re-zero module angles</button>
  <button id="recalBtn">Recalibrate modules</button>
  <div class="warn">
    There are no azimuth encoders. The robot tracks module headings by counting commanded
    steps, so they drift. Physically point all four modules straight forward, then re-zero.
  </div>
</div>

<!-- ===================== JOYSTICK ===================== -->
<div id="screen-joystick" class="screen" style="display:none">
  <button class="backBtn">&larr; Menu</button>
  <div class="joyRow">
    <div class="joyBox">
      <div class="joyLabel">Translate</div>
      <div class="joystickZone" id="leftZone"><div class="stick" id="leftStick"></div></div>
      <div class="warn">up/down = drive<br>left/right = strafe</div>
    </div>
    <div class="joyBox">
      <div class="joyLabel">Rotate</div>
      <div class="joystickZone" id="rightZone"><div class="stick" id="rightStick"></div></div>
      <div class="warn">left/right = spin</div>
    </div>
  </div>
</div>

<!-- ===================== ARROWS ===================== -->
<div id="screen-arrows" class="screen" style="display:none">
  <button class="backBtn">&larr; Menu</button>
  <div class="arrowGrid">
    <div></div><button class="arrowBtn" data-dir="fwd">&uarr;</button><div></div>
    <button class="arrowBtn" data-dir="left">&larr;</button>
    <div class="arrowCenter">&#9679;</div>
    <button class="arrowBtn" data-dir="right">&rarr;</button>
    <div></div><button class="arrowBtn" data-dir="back">&darr;</button><div></div>
  </div>
  <div class="rotRow">
    <button class="arrowBtn wide" data-dir="rotL">Rotate L</button>
    <button class="arrowBtn wide" data-dir="rotR">Rotate R</button>
  </div>
  <div class="warn">Left/right arrows strafe sideways. Use the rotate buttons to spin.</div>
</div>

<!-- ===================== NUMERIC ===================== -->
<div id="screen-numeric" class="screen" style="display:none">
  <button class="backBtn">&larr; Menu</button>

  <div class="numRow">
    <label>Forward / Backward (feet)</label>
    <input type="number" id="distInput" value="1" step="0.5" min="0">
    <div class="btnRow">
      <button id="fwdBtn">Forward</button>
      <button id="backBtn2">Backward</button>
    </div>
  </div>

  <div class="numRow">
    <label>Strafe (feet, negative = right)</label>
    <input type="number" id="strafeInput" value="1" step="0.5">
    <div class="btnRow">
      <button id="strafeBtn">Execute Strafe</button>
    </div>
  </div>

  <div class="numRow">
    <label>Rotate (degrees, negative = clockwise)</label>
    <input type="number" id="rotInput" value="90" step="15">
    <div class="btnRow">
      <button id="rotBtn">Execute Rotation</button>
    </div>
  </div>

  <div class="warn">
    Open-loop timing, not measured distance. Distance = speed x time, so results drift,
    and short moves land shortest because acceleration ramp time isn't counted.
  </div>
</div>

<script>
  let ws = new WebSocket("ws://" + location.hostname + "/ws");
  let connStatus = document.getElementById("connStatus");
  let speedLabel = document.getElementById("speedLabel");
  let modeLabel  = document.getElementById("modeLabel");
  let currentMode = "menu";

  const POS_NAMES = ["Front Left", "Front Right", "Back Left", "Back Right"];
  const POS_IDS   = ["lblFL", "lblFR", "lblBL", "lblBR"];

  // local working copy of calibration; pushed to the ESP32 on Save
  let calMap  = [0, 1, 2, 3];
  let calDrv  = [1, 1, 1, 1];
  let calStr  = [1, 1, 1, 1];

  const send = (obj) => {
    if (ws.readyState === WebSocket.OPEN) ws.send(JSON.stringify(obj));
  };

  // ---------- screens ----------
  const stopArrowHold = () => {
    clearInterval(arrowHoldInterval);
    arrowHoldInterval = null;
    send({ type: "arrow", dir: "stop" });
  };

  const showScreen = (name) => {
    document.querySelectorAll(".screen").forEach(s => s.style.display = "none");
    document.getElementById("screen-" + name).style.display = "block";
    currentMode = name;
    const nice = { menu:"Menu", cal:"Calibration", joystick:"Joystick",
                   arrows:"Arrows", numeric:"Numeric" };
    modeLabel.textContent = nice[name] || name;
    stopArrowHold();
    send({ type: "mode", value: name });
  };

  // ---------- calibration UI ----------
  const refreshDiagram = () => {
    for (let p = 0; p < 4; p++) {
      document.getElementById(POS_IDS[p]).textContent = String(calMap[p] + 1);
    }
  };

  const validateMap = () => {
    const seen = {};
    let dup = false;
    calMap.forEach(v => { if (seen[v]) dup = true; seen[v] = true; });
    const el = document.getElementById("mapWarn");
    const btn = document.getElementById("saveCalBtn");
    if (dup) {
      el.textContent = "Each driver pair must be used exactly once.";
      btn.disabled = true;
      btn.style.opacity = "0.45";
    } else {
      el.textContent = "";
      btn.disabled = false;
      btn.style.opacity = "1";
    }
    return !dup;
  };

  const buildCalCards = () => {
    const wrap = document.getElementById("calCards");
    wrap.innerHTML = "";

    for (let p = 0; p < 4; p++) {
      const card = document.createElement("div");
      card.className = "posCard";

      const head = document.createElement("div");
      head.className = "posHead";

      const nm = document.createElement("span");
      nm.className = "posName";
      nm.textContent = POS_NAMES[p];
      head.appendChild(nm);

      const sel = document.createElement("select");
      sel.className = "posSel";
      for (let h = 0; h < 4; h++) {
        const o = document.createElement("option");
        o.value = String(h);
        o.textContent = "Pair " + (h + 1);
        sel.appendChild(o);
      }
      sel.value = String(calMap[p]);
      sel.addEventListener("change", () => {
        calMap[p] = parseInt(sel.value, 10);
        refreshDiagram();
        validateMap();
      });
      head.appendChild(sel);
      card.appendChild(head);

      const row1 = document.createElement("div");
      row1.className = "calBtnRow";

      const spinD = document.createElement("button");
      spinD.className = "spin";
      spinD.textContent = "SPIN drive";
      spinD.addEventListener("click", () => send({ type: "caltest", hw: calMap[p], steer: false }));
      row1.appendChild(spinD);

      const spinS = document.createElement("button");
      spinS.className = "spin";
      spinS.textContent = "SPIN steer";
      spinS.addEventListener("click", () => send({ type: "caltest", hw: calMap[p], steer: true }));
      row1.appendChild(spinS);
      card.appendChild(row1);

      const row2 = document.createElement("div");
      row2.className = "calBtnRow";
      row2.style.marginTop = "8px";

      const revD = document.createElement("button");
      const revS = document.createElement("button");

      const paint = () => {
        const h = calMap[p];
        revD.textContent = "Drive: " + (calDrv[h] < 0 ? "REVERSED" : "normal");
        revS.textContent = "Steer: " + (calStr[h] < 0 ? "REVERSED" : "normal");
        revD.className = calDrv[h] < 0 ? "rev" : "";
        revS.className = calStr[h] < 0 ? "rev" : "";
      };

      // Apply immediately so the next SPIN test shows the new direction. Saving is
      // still what makes it stick across a reboot.
      revD.addEventListener("click", () => {
        const h = calMap[p];
        calDrv[h] = -calDrv[h];
        send({ type: "calinv", hw: h, which: "drive", value: calDrv[h] });
        paint();
      });
      revS.addEventListener("click", () => {
        const h = calMap[p];
        calStr[h] = -calStr[h];
        send({ type: "calinv", hw: h, which: "steer", value: calStr[h] });
        paint();
      });

      paint();
      row2.appendChild(revD);
      row2.appendChild(revS);
      card.appendChild(row2);

      wrap.appendChild(card);
    }
  };

  document.getElementById("saveCalBtn").addEventListener("click", () => {
    if (!validateMap()) return;
    send({ type: "calsave", map: calMap, dinv: calDrv, sinv: calStr });
    showScreen("menu");
  });

  // ---------- socket ----------
  ws.onopen  = () => { connStatus.textContent = "connected";    connStatus.className = "connected"; };
  ws.onclose = () => { connStatus.textContent = "disconnected"; connStatus.className = "disconnected"; };

  ws.onmessage = (evt) => {
    let msg = null;
    try { msg = JSON.parse(evt.data); } catch (e) { return; }
    if (msg.type !== "calstate") return;

    calMap = msg.map.slice();
    calDrv = msg.dinv.slice();
    calStr = msg.sinv.slice();

    buildCalCards();
    refreshDiagram();
    validateMap();

    // Unconfigured robots land on the calibration screen; configured ones skip it.
    showScreen(msg.calibrated ? "menu" : "cal");
  };

  // ---------- speed ----------
  document.querySelectorAll(".speedBtn").forEach(b => {
    b.addEventListener("click", () => {
      document.querySelectorAll(".speedBtn").forEach(x => x.classList.remove("selected"));
      b.classList.add("selected");
      const v = parseFloat(b.dataset.v);
      speedLabel.textContent = "Speed: " + Math.round(v * 100) + "%";
      send({ type: "speed", value: v });
    });
  });

  document.getElementById("zeroBtn").addEventListener("click", () => {
    send({ type: "zero" });
    const b = document.getElementById("zeroBtn");
    b.textContent = "Zeroed";
    setTimeout(() => { b.textContent = "Re-zero module angles"; }, 1200);
  });

  document.getElementById("recalBtn").addEventListener("click", () => showScreen("cal"));

  document.querySelectorAll(".modeBtn").forEach(b => b.addEventListener("click", () => showScreen(b.dataset.mode)));
  document.querySelectorAll(".backBtn").forEach(b => b.addEventListener("click", () => showScreen("menu")));
  document.getElementById("stopBtn").addEventListener("click", () => send({ type: "stop" }));

  // ---------- joysticks ----------
  let leftX = 0, leftY = 0, rightX = 0;

  const setupJoystick = (zoneId, stickId, isLeft) => {
    const zone = document.getElementById(zoneId);
    const stick = document.getElementById(stickId);
    const c = 75, r = 60;
    let dragging = false;

    const move = (cx, cy) => {
      const rect = zone.getBoundingClientRect();
      let dx = cx - rect.left - c;
      let dy = cy - rect.top - c;
      const d = Math.sqrt(dx * dx + dy * dy);
      if (d > r) { dx = dx * r / d; dy = dy * r / d; }
      stick.style.left = (c + dx - 25) + "px";
      stick.style.top  = (c + dy - 25) + "px";
      if (isLeft) { leftX = dx / r; leftY = -dy / r; } else { rightX = dx / r; }
    };

    const reset = () => {
      stick.style.left = (c - 25) + "px";
      stick.style.top  = (c - 25) + "px";
      if (isLeft) { leftX = 0; leftY = 0; } else { rightX = 0; }
    };

    zone.addEventListener("pointerdown", e => { dragging = true; move(e.clientX, e.clientY); });
    window.addEventListener("pointermove", e => { if (dragging) move(e.clientX, e.clientY); });
    window.addEventListener("pointerup", () => { dragging = false; reset(); });
    reset();
  };

  setupJoystick("leftZone", "leftStick", true);
  setupJoystick("rightZone", "rightStick", false);

  // Resend continuously so holding a stick still doesn't trip the comms-timeout stop.
  setInterval(() => {
    if (currentMode === "joystick") {
      send({ type: "joystick", lx: leftX, ly: leftY, rx: rightX });
    }
  }, 100);

  // ---------- arrows ----------
  let arrowHoldInterval = null;

  const startArrowHold = (dir) => {
    send({ type: "arrow", dir: dir });
    clearInterval(arrowHoldInterval);
    arrowHoldInterval = setInterval(() => send({ type: "arrow", dir: dir }), 100);
  };

  document.querySelectorAll(".arrowBtn").forEach(b => {
    const dir = b.dataset.dir;
    b.addEventListener("pointerdown", () => startArrowHold(dir));
    b.addEventListener("pointerup", stopArrowHold);
    b.addEventListener("pointerleave", stopArrowHold);
    b.addEventListener("pointercancel", stopArrowHold);
  });

  // ---------- numeric ----------
  document.getElementById("fwdBtn").addEventListener("click", () => {
    const v = parseFloat(document.getElementById("distInput").value) || 0;
    send({ type: "numeric", action: "forward", value: v });
  });
  document.getElementById("backBtn2").addEventListener("click", () => {
    const v = parseFloat(document.getElementById("distInput").value) || 0;
    send({ type: "numeric", action: "backward", value: v });
  });
  document.getElementById("strafeBtn").addEventListener("click", () => {
    const v = parseFloat(document.getElementById("strafeInput").value) || 0;
    send({ type: "numeric", action: "strafe", value: v });
  });
  document.getElementById("rotBtn").addEventListener("click", () => {
    const v = parseFloat(document.getElementById("rotInput").value) || 0;
    send({ type: "numeric", action: "rotate", value: v });
  });

  // Until calstate arrives we show nothing, so a stale screen never implies a
  // calibration the robot doesn't actually have.
  buildCalCards();
  refreshDiagram();
</script>
</body>
</html>
)HTMLPAGE";

float wrapAngleDeg(float a) {
  while (a > 180.0f)   a -= 360.0f;
  while (a <= -180.0f) a += 360.0f;
  return a;
}

void driveSwerve(float vx, float vy, float omega_dps, float dt) {
  float omega_rads = omega_dps * (float)M_PI / 180.0f;

  float targetSpeed[NUM_MODULES];
  float targetAngleDeg[NUM_MODULES];

  for (int m = 0; m < NUM_MODULES; m++) {
    float mvx = vx - omega_rads * MODULE_Y[m];
    float mvy = vy + omega_rads * MODULE_X[m];

    targetSpeed[m] = sqrtf(mvx * mvx + mvy * mvy);

    targetAngleDeg[m] = (targetSpeed[m] < 1e-4f)
                        ? estimatedAngle[m]
                        : atan2f(mvy, mvx) * 180.0f / (float)M_PI;
  }

  // Desaturation: rotation stacks on translation, so a module can be asked to exceed max
  // speed. Scale all four down together — scaling only the fast one distorts the direction
  // the robot actually travels.
  float fastest = 0.0f;
  for (int m = 0; m < NUM_MODULES; m++) if (targetSpeed[m] > fastest) fastest = targetSpeed[m];
  if (fastest > MAX_DRIVE_SPEED_MPS) {
    float scale = MAX_DRIVE_SPEED_MPS / fastest;
    for (int m = 0; m < NUM_MODULES; m++) targetSpeed[m] *= scale;
  }

  for (int m = 0; m < NUM_MODULES; m++) {
    float error = wrapAngleDeg(targetAngleDeg[m] - estimatedAngle[m]);
    float speed = targetSpeed[m];

    // Angle optimization: never rotate a module more than 90 degrees. Past that, point the
    // opposite way and drive the wheel backwards — same motion, far less steering travel.
    if (fabsf(error) > 90.0f) {
      error = wrapAngleDeg(error + 180.0f);
      speed = -speed;
    }

    float steerRate = STEER_KP * error;
    if (steerRate >  MAX_MODULE_STEER_DPS) steerRate =  MAX_MODULE_STEER_DPS;
    if (steerRate < -MAX_MODULE_STEER_DPS) steerRate = -MAX_MODULE_STEER_DPS;

    estimatedAngle[m] = wrapAngleDeg(estimatedAngle[m] + steerRate * dt);

    setModuleVelocity(moduleMap[m], speed, steerRate);
  }
}

void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type,
               void *arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    Serial.printf("WS client #%u connected\n", client->id());
    fullStop();
    g_mode = MODE_MENU;
    sendCalState(client);

  } else if (type == WS_EVT_DISCONNECT) {
    Serial.printf("WS client #%u disconnected\n", client->id());
    fullStop();

  } else if (type == WS_EVT_DATA) {
    StaticJsonDocument<384> doc;
    if (deserializeJson(doc, data, len)) return;

    const char* msgType = doc["type"] | "";

    if (strcmp(msgType, "stop") == 0) {
      fullStop();
      g_lastCommandMillis = millis();
      return;
    }

    if (strcmp(msgType, "zero") == 0) {
      fullStop();
      zeroModuleAngles();
      Serial.println("Module angles re-zeroed.");
      return;
    }

    if (strcmp(msgType, "speed") == 0) {
      float v = doc["value"] | 1.0f;
      if (v < 0.05f) v = 0.05f;
      if (v > 1.0f)  v = 1.0f;
      g_speedScale = v;
      return;
    }

    if (strcmp(msgType, "mode") == 0) {
      const char* m = doc["value"] | "menu";
      fullStop();
      if      (strcmp(m, "joystick") == 0) g_mode = MODE_JOYSTICK;
      else if (strcmp(m, "arrows")   == 0) g_mode = MODE_ARROWS;
      else if (strcmp(m, "numeric")  == 0) g_mode = MODE_NUMERIC;
      else if (strcmp(m, "cal")      == 0) g_mode = MODE_CAL;
      else                                 g_mode = MODE_MENU;
      return;
    }

    if (strcmp(msgType, "caltest") == 0 && g_mode == MODE_CAL) {
      int hw = doc["hw"] | -1;
      if (hw < 0 || hw >= NUM_MODULES) return;
      fullStop();
      g_testHw = hw;
      g_testIsSteer = doc["steer"] | false;
      g_testStopAt = millis() + CAL_TEST_MS;
      g_testActive = true;
      g_lastCommandMillis = millis();
      Serial.printf("Cal test: pair %d, %s\n", hw + 1, g_testIsSteer ? "steer" : "drive");
      return;
    }

    if (strcmp(msgType, "calinv") == 0) {
      int hw = doc["hw"] | -1;
      if (hw < 0 || hw >= NUM_MODULES) return;
      int val = (doc["value"] | 1) < 0 ? -1 : 1;
      const char* which = doc["which"] | "";
      if      (strcmp(which, "drive") == 0) driveInvert[hw] = (int8_t)val;
      else if (strcmp(which, "steer") == 0) steerInvert[hw] = (int8_t)val;
      else return;
      Serial.printf("Live invert: pair %d %s = %d\n", hw + 1, which, val);
      return;
    }

    if (strcmp(msgType, "calsave") == 0) {
      JsonArray mp = doc["map"].as<JsonArray>();
      JsonArray di = doc["dinv"].as<JsonArray>();
      JsonArray si = doc["sinv"].as<JsonArray>();
      if (mp.isNull() || di.isNull() || si.isNull()) return;
      if (mp.size() != NUM_MODULES || di.size() != NUM_MODULES || si.size() != NUM_MODULES) return;

      // Must be a permutation of 0..3. A duplicate would leave one pair uncommanded and
      // double-command another.
      uint8_t cand[NUM_MODULES];
      bool seen[NUM_MODULES] = { false, false, false, false };
      for (int i = 0; i < NUM_MODULES; i++) {
        int v = mp[i] | -1;
        if (v < 0 || v >= NUM_MODULES || seen[v]) return;
        seen[v] = true;
        cand[i] = (uint8_t)v;
      }

      fullStop();
      for (int i = 0; i < NUM_MODULES; i++) {
        moduleMap[i]   = cand[i];
        driveInvert[i] = ((int)(di[i] | 1) < 0) ? -1 : 1;
        steerInvert[i] = ((int)(si[i] | 1) < 0) ? -1 : 1;
      }
      saveCalibration();
      g_mode = MODE_MENU;
      return;
    }

    if (strcmp(msgType, "joystick") == 0 && g_mode == MODE_JOYSTICK) {
      float lx = doc["lx"] | 0.0f;
      float ly = doc["ly"] | 0.0f;
      float rx = doc["rx"] | 0.0f;

      const float DEADZONE = 0.08f;
      if (fabsf(lx) < DEADZONE) lx = 0;
      if (fabsf(ly) < DEADZONE) ly = 0;
      if (fabsf(rx) < DEADZONE) rx = 0;

      g_vx    =  ly * MAX_DRIVE_SPEED_MPS  * g_speedScale;
      g_vy    = -lx * MAX_DRIVE_SPEED_MPS  * g_speedScale;
      g_omega = -rx * MAX_ANGULAR_RATE_DPS * g_speedScale;

      g_numericRunning = false;
      g_stopped = false;
      g_lastCommandMillis = millis();
      return;
    }

    if (strcmp(msgType, "arrow") == 0 && g_mode == MODE_ARROWS) {
      const char* dir = doc["dir"] | "stop";
      float vx = 0, vy = 0, w = 0;

      if      (strcmp(dir, "fwd")   == 0) vx =  1.0f;
      else if (strcmp(dir, "back")  == 0) vx = -1.0f;
      else if (strcmp(dir, "left")  == 0) vy =  1.0f;
      else if (strcmp(dir, "right") == 0) vy = -1.0f;
      else if (strcmp(dir, "rotL")  == 0) w  =  1.0f;
      else if (strcmp(dir, "rotR")  == 0) w  = -1.0f;

      g_vx    = vx * MAX_DRIVE_SPEED_MPS  * g_speedScale;
      g_vy    = vy * MAX_DRIVE_SPEED_MPS  * g_speedScale;
      g_omega = w  * MAX_ANGULAR_RATE_DPS * g_speedScale;

      g_numericRunning = false;
      g_stopped = false;
      g_lastCommandMillis = millis();
      return;
    }

    if (strcmp(msgType, "numeric") == 0 && g_mode == MODE_NUMERIC) {
      const char* action = doc["action"] | "";
      float value = doc["value"] | 0.0f;

      float speed = MAX_DRIVE_SPEED_MPS  * g_speedScale;
      float wrate = MAX_ANGULAR_RATE_DPS * g_speedScale;
      unsigned long durationMs = 0;

      g_vx = g_vy = g_omega = 0.0f;

      if (strcmp(action, "forward") == 0 || strcmp(action, "backward") == 0) {
        if (speed < 0.01f) return;
        float meters = fabsf(value) * 0.3048f;
        g_vx = (strcmp(action, "forward") == 0 ? speed : -speed);
        durationMs = (unsigned long)((meters / speed) * 1000.0f);

      } else if (strcmp(action, "strafe") == 0) {
        if (speed < 0.01f) return;
        float meters = fabsf(value) * 0.3048f;
        g_vy = (value >= 0 ? speed : -speed);
        durationMs = (unsigned long)((meters / speed) * 1000.0f);

      } else if (strcmp(action, "rotate") == 0) {
        if (wrate < 0.01f) return;
        g_omega = (value >= 0 ? wrate : -wrate);
        durationMs = (unsigned long)((fabsf(value) / wrate) * 1000.0f);

      } else {
        return;
      }

      g_numericRunning = true;
      g_numericStopAt = millis() + durationMs;
      g_stopped = false;
      g_lastCommandMillis = millis();
      return;
    }
  }
}

void setupWebServer() {
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  Serial.print("AP IP address: ");
  Serial.println(WiFi.softAPIP());

  ws.onEvent(onWsEvent);
  server.addHandler(&ws);

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send_P(200, "text/html", index_html);
  });

  server.begin();
}

void setup() {
  Serial.begin(115200);
  engine.init();

  for (int h = 0; h < NUM_MODULES; h++) {
    motorA[h] = engine.stepperConnectToPin(modulePins[h][0].step);
    motorB[h] = engine.stepperConnectToPin(modulePins[h][1].step);

    if (motorA[h]) {
      motorA[h]->setDirectionPin(modulePins[h][0].dir);
      motorA[h]->setAutoEnable(true);
    }
    if (motorB[h]) {
      motorB[h]->setDirectionPin(modulePins[h][1].dir);
      motorB[h]->setAutoEnable(true);
    }
  }

  loadCalibration();
  zeroModuleAngles();
  setupWebServer();
  g_lastLoopMicros = micros();

  Serial.println();
  Serial.println("=== SWERVE READY ===");
  if (calibrated) {
    Serial.print("Loaded calibration. Position -> pair: ");
    for (int i = 0; i < NUM_MODULES; i++) Serial.printf("%d ", moduleMap[i] + 1);
    Serial.println();
  } else {
    Serial.println("No saved calibration — the UI will open on the calibration screen.");
  }
  Serial.println("ALL FOUR MODULES MUST BE POINTING STRAIGHT FORWARD RIGHT NOW.");
  Serial.println("There is no azimuth sensor; this is the only homing reference.");
  Serial.println("Connect to WiFi \"" + String(AP_SSID) + "\" and open http://192.168.4.1");
}

void setModuleVelocity(int hw, float wheelSpeed_mps, float steerRate_dps) {
  if (hw < 0 || hw >= NUM_MODULES) return;

  float drive_rps  = (wheelSpeed_mps / DIST_PER_MOTOR_REV_M) * driveInvert[hw];
  float module_rps = steerRate_dps / 360.0f;
  float steer_rps  = module_rps * STEER_GEAR_RATIO * steerInvert[hw];

  float motorA_rps = drive_rps + steer_rps;
  float motorB_rps = drive_rps - steer_rps;

  runMotorAtRps(motorA[hw], motorA_rps);
  runMotorAtRps(motorB[hw], motorB_rps);
}

void runMotorAtRps(FastAccelStepper* motor, float rps) {
  if (!motor) return;

  float stepsPerSec = rps * STEPS_PER_REV;
  bool forward = (stepsPerSec >= 0);
  float rate = fabsf(stepsPerSec);

  if (rate > MAX_STEP_RATE_HZ) rate = MAX_STEP_RATE_HZ;

  if (rate < 1.0f) {
    motor->stopMove();
    return;
  }

  motor->setSpeedInHz((uint32_t)rate);
  motor->setAcceleration(ACCEL_STEPS_PER_S2);

  if (forward) motor->runForward();
  else         motor->runBackward();
}

void stopAllModules() {
  for (int h = 0; h < NUM_MODULES; h++) {
    if (motorA[h]) motorA[h]->stopMove();
    if (motorB[h]) motorB[h]->stopMove();
  }
}

void loop() {
  ws.cleanupClients();

  unsigned long nowMicros = micros();
  float dt = (nowMicros - g_lastLoopMicros) / 1000000.0f;
  g_lastLoopMicros = nowMicros;
  if (dt <= 0.0f || dt > 0.5f) dt = 0.02f;

  if (g_testActive) {
    if (millis() >= g_testStopAt) {
      g_testActive = false;
      g_testHw = -1;
      stopAllModules();
    } else {
      for (int h = 0; h < NUM_MODULES; h++) {
        if (h == g_testHw) {
          setModuleVelocity(h,
                            g_testIsSteer ? 0.0f : CAL_TEST_SPEED_MPS,
                            g_testIsSteer ? CAL_TEST_STEER_DPS : 0.0f);
        } else {
          setModuleVelocity(h, 0.0f, 0.0f);
        }
      }
    }
    delay(20);
    return;
  }

  if (g_numericRunning) {
    if (millis() >= g_numericStopAt) fullStop();
    else g_lastCommandMillis = millis();
  }

  if ((millis() - g_lastCommandMillis) > COMMAND_TIMEOUT_MS && !g_stopped) fullStop();

  if (g_stopped) {

    for (int h = 0; h < NUM_MODULES; h++) setModuleVelocity(h, 0.0f, 0.0f);
  } else {
    driveSwerve(g_vx, g_vy, g_omega, dt);
  }

  delay(20);
}
