# Eye switch

An Android app and ESP32 relay box that let a student press an adapted switch with their eyes.
See the [main README](../README.md) for how it works.

## Layout

```
app/                         Android app (Kotlin)
  src/main/java/com/eyeswitch/
  src/main/res/
firmware/eye_switch_relay/   ESP32 relay sketch (Arduino)
```

## Build the app

1. Open the `eye-switch` folder in Android Studio and let Gradle sync.
2. Build and run on a phone with Android 8.0 or newer.

The face model (about 3.6 MB) downloads automatically on the first build, so that first build
needs internet. After that the app works offline.

The Gradle wrapper scripts (`gradlew`, `gradle-wrapper.jar`) aren't in the repo. Android Studio can
usually sync without them. To build from the command line, run `gradle wrapper` once to generate them.

## Relay box

Flash `firmware/eye_switch_relay/eye_switch_relay.ino` to any ESP32 dev board.

| ESP32 | Relay module |
|---|---|
| 5V | VCC |
| GND | GND |
| D5 | IN1 (jack A) |
| D18 | IN2 (jack B, optional) |

Wire each relay's COM and NO terminals to the two wires of a 3.5 mm mono jack. Most cheap relay
modules switch on when IN goes LOW: if a relay clicks on at power-up and off when triggered, set
`RELAY_ACTIVE_HIGH = false` in the sketch.

Connect the phone to the ESP32 with a USB OTG cable. The phone powers the board. If the app
doesn't find it, check the board's USB chip is listed in `app/src/main/res/xml/device_filter.xml`.

To test without the app, open a serial monitor at 115200 baud and send `PING` (replies `PONG`)
or `A250` (pulses relay A for 250 ms).
