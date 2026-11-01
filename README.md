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

## [Mechanical integrator](mechanical-integrator/)

A disk-and-wheel integrator, the same principle as Thomson's 1876 machine and the integrators
in Bush's differential analyzer, rebuilt with steppers and a mostly 3D-printed frame.

A wheel rides on a turntable at radius r. With no slip, its rotation adds up r as the disk turns:

```
wheel rotation = (1/R) * integral of r dθ
```

Set r = k·f(θ) and the wheel's total rotation is proportional to the integral of f. Taking the
wheel past the disk centre makes r negative, so the machine computes signed area. Two stationary
steppers drive the turntable and the carriage through a belt differential (same direction spins
the disk, opposite directions slide the carriage), which keeps the moving carriage light.

Steppers don't move continuously, so the firmware splits each sweep into segments of size h. Its
three modes turn the machine into three textbook numerical methods: a left Riemann sum (error ~h),
the midpoint rule (~h²) and the trapezoidal rule (~h²). That made it the subject of my IB Math
IA, testing whether the measured error shrinks at the predicted rates.

Two Arduino sketches, each in its own folder so the IDE doesn't merge them:
`integrator_controller/` (motors, AS5600 wheel encoder, serial commands) and
`integrator_display/` (touchscreen menus, live graphs, calibration screen). Plus
`integrator_logger.py` (runs the experiment over USB, logs each run to CSV, fits the convergence
order and plots it).

**Limitation:** the electronics aren't the limit: the encoder resolves 0.0015 rad, under 0.03% of
a run. The mechanics are. In a one-run-per-setting trial, the left sum's error fell as predicted
(fitted order 0.75 against 1), but the midpoint and trapezoid results for θ² carried a constant
~6% bias that hid their h² errors. I haven't pinned down its cause yet. There are also no
endstops, so the wheel has to be centred over the disk by hand before each session.

| | |
|---|---|
| Controller | ESP32 + separate ESP32 touchscreen board (480×320) |
| Wheel encoder | AS5600, 4096 counts/rev (0.0015 rad) |
| Transmission | GT3 belts, 20:100 reduction, belt differential |
| Integrating wheel | ~25 mm radius, omni-wheel |
| Best result | within 0.9% of the exact integral (midpoint rule, fine segments) |

Libraries: TFT_eSPI, QRCodeGenerator (display); pyserial, NumPy, matplotlib (logger).

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
