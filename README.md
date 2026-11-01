# Maker portfolio

Firmware and code from my mechanical engineering build projects.

## [Differential swerve drivetrain](differential-swerve/)

A four-module holonomic swerve drivetrain on an ESP32, driving eight NEMA17 steppers.

Each module uses two motors through a differential, so both contribute to both functions
instead of one driving and one steering:

```
motorA = drive + steer
motorB = drive - steer
```

Equal speeds drive the wheel, opposite speeds rotate the module. The firmware takes a
robot velocity vector and a rotation rate, then solves each module's speed and heading
independently — that's what allows strafing and diagonal motion. It handles desaturation
(scaling all four modules together when one exceeds max speed) and angle optimization
(never rotating a module past 90°, flipping wheel direction instead).

Control is over WiFi: the ESP32 hosts its own access point and serves a phone interface
from program memory. Joystick, arrow, and distance-entry modes, plus a calibration screen
for mapping driver pairs to chassis corners and reversing a module in software.

**Limitation:** no encoders on the module pivots. Headings are dead-reckoned from commanded
steer rates, so a skipped step or a bump puts the estimate permanently out of sync. Adding
an absolute encoder per module is the next change worth making.

| | |
|---|---|
| Controller | ESP32, DRV8825 drivers, 1/32 microstepping |
| Drive ratio | 1.969:1 |
| Steering ratio | 15:128 (8.533:1) |
| Wheel circumference | 327.4 mm |
| Chassis | 290 mm between pivot axes |

Libraries: FastAccelStepper, AsyncTCP, ESPAsyncWebServer, ArduinoJson.

## [Eye switch](eye-switch/)

A hands-free switch for students with multiple disabilities who can't press a normal
adapted switch. An Android phone on the wheelchair tray watches the student's eyes, and
a deliberate look left, look right, or held blink presses a toy's switch, plays a song,
or fills the screen with soft colour.

The app runs Google's MediaPipe Face Landmarker on every camera frame, entirely on the
phone. It reports how far each eye is turned in and out, and the two eyes are averaged
into one sideways reading:

```
h = (outRight + inLeft)/2 - (outLeft + inRight)/2
```

The camera sits at one end of a landscape phone, so looking left and right are never
symmetrical. Calibration records `h` while the student looks at the centre, the left
target, and the right target, then maps live readings so 1.0 always means "looking at
that target". A gesture has to be held for the full hold time, fires once, and can't
fire again until the student lets go and the refractory gap passes.

A press goes over USB OTG to an ESP32 (`A250\n`), which closes a relay wired to a
3.5 mm mono jack, the standard plug on switch-adapted toys. The phone powers the board
over the same cable. Lights are capped well under the WCAG limit of three flashes a
second: they only fade, and the timings are constants, not settings.

**Limitation:** the face model reports eye direction as blendshape scores, not a true
gaze vector, so the app can only tell left, right and blink apart, not where on the
screen someone is looking. It also needs a reasonably lit room, and detection accuracy
hasn't yet been measured with students.

| | |
|---|---|
| App | Kotlin, CameraX, Android 8.0+ |
| Tracking | MediaPipe Face Landmarker, on-device |
| Hold time | 150 to 2000 ms, saved per student |
| Output | Relay contact closure, 3.5 mm mono jack, 50 to 2000 ms pulse |
| Link | USB OTG serial, 115200 baud |

Libraries: MediaPipe Tasks Vision, CameraX, usb-serial-for-android, Material Components.
The face model downloads on the first Gradle build and isn't stored in the repo.
