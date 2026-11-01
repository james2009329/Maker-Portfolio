/*
  Eye Switch - ESP32 relay bridge

  Reads one command per line from the Android app over USB serial and pulses a
  relay that closes a 3.5 mm mono jack. To the toy, this looks exactly like an
  ordinary adapted switch being pressed.

  Protocol:
      A<ms>\n   pulse relay A (jack A), e.g. A250
      B<ms>\n   pulse relay B (jack B)
      T<ms>\n   same as A, kept so the first app version still works
      PING\n    replies PONG
  Anything else is ignored. Pulse length is clamped to MIN/MAX_PULSE_MS.

  Wiring:
    ESP32 5V -> VCC, GND -> GND, D5 -> IN1 (A), D18 -> IN2 (B, optional)
    each relay's COM / NO -> the two wires of its jack (NO stays open until pressed)

  IMPORTANT: most cheap relay modules energise when IN is pulled LOW. Set
  RELAY_ACTIVE_HIGH to match your board. If a relay clicks on at power-up and
  off when triggered, you have it the wrong way round.
*/

const int  RELAY_A_PIN       = 5;
const int  RELAY_B_PIN       = 18;
const bool RELAY_ACTIVE_HIGH = true;   // set false if your module is active low

const int  DEFAULT_PULSE_MS  = 250;
const int  MIN_PULSE_MS      = 50;
const int  MAX_PULSE_MS      = 2000;

const unsigned long SERIAL_BAUD = 115200;

// The LED gives a visible confirmation while testing without the phone.
const int LED_PIN = 2;

void setRelay(int pin, bool closed) {
  bool level = RELAY_ACTIVE_HIGH ? closed : !closed;
  digitalWrite(pin, level ? HIGH : LOW);
  digitalWrite(LED_PIN, closed ? HIGH : LOW);
}

void pulseRelay(int pin, char name, int ms) {
  if (ms <= 0) ms = DEFAULT_PULSE_MS;
  if (ms < MIN_PULSE_MS) ms = MIN_PULSE_MS;
  if (ms > MAX_PULSE_MS) ms = MAX_PULSE_MS;

  setRelay(pin, true);
  delay(ms);
  setRelay(pin, false);

  Serial.print("OK ");
  Serial.print(name);
  Serial.println(ms);
}

void setup() {
  pinMode(RELAY_A_PIN, OUTPUT);
  pinMode(RELAY_B_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  setRelay(RELAY_A_PIN, false);    // start with both switches open
  setRelay(RELAY_B_PIN, false);

  Serial.begin(SERIAL_BAUD);
  delay(200);
  Serial.println("EyeSwitch relay ready");
}

void loop() {
  if (!Serial.available()) return;

  String line = Serial.readStringUntil('\n');
  line.trim();
  if (line.length() == 0) return;

  if (line == "PING") {
    // Lets the app or the Arduino serial monitor confirm the board is alive.
    Serial.println("PONG");
    return;
  }

  char c = line.charAt(0);
  int ms = line.substring(1).toInt();
  if (c == 'A' || c == 'T') {
    pulseRelay(RELAY_A_PIN, 'A', ms);
  } else if (c == 'B') {
    pulseRelay(RELAY_B_PIN, 'B', ms);
  }
}
