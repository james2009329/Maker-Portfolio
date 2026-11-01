# Maker portfolio

Firmware and code from my mechanical engineering build projects.

## [Differential swerve drivetrain](differential-swerve/)

A four-module holonomic swerve drivetrain on an ESP32, driving eight NEMA17 steppers.

Each module uses two motors through a differential, so both share driving and steering:

```
motorA = drive + steer
motorB = drive - steer
```

Equal speeds drive the wheel, opposite speeds rotate the module. The firmware solves each
module's speed and heading from a robot velocity and rotation rate, which allows strafing and
diagonal motion. It scales all four modules together when one would exceed max speed, and
never rotates a module past 90°, flipping the wheel direction instead.

The ESP32 hosts its own WiFi access point and serves a phone interface with joystick, arrow and
distance modes, plus a calibration screen for mapping and reversing modules.

**Limitation:** no encoders on the module pivots. Headings are dead-reckoned, so a skipped step
or a bump puts them permanently out of sync. An absolute encoder per module is the next fix.

| | |
|---|---|
| Controller | ESP32, DRV8825 drivers, 1/32 microstepping |
| Drive ratio | 1.969:1 |
| Steering ratio | 15:128 (8.533:1) |
| Wheel circumference | 327.4 mm |
| Chassis | 290 mm between pivot axes |

Libraries: FastAccelStepper, AsyncTCP, ESPAsyncWebServer, ArduinoJson.

## [Mechanical integrator](mechanical-integrator/)

A disk-and-wheel integrator, the same principle as Thomson's 1876 machine and Bush's
differential analyzer, rebuilt with steppers and a mostly 3D-printed frame.

A wheel rides on a turntable at radius r. With no slip:

```
wheel rotation = (1/R) * integral of r dθ
```

Set r = k·f(θ) and the wheel's rotation is proportional to the integral of f. Past the disk
centre r goes negative, so the machine gives signed area. Two stationary steppers drive the
turntable and carriage through a belt differential, which keeps the carriage light.

The firmware splits each sweep into segments of size h, and its three modes compute a left
Riemann sum (error ~h), the midpoint rule (~h²) or the trapezoidal rule (~h²). I tested whether
the measured error shrinks at those rates for my IB Math IA.

`integrator_controller/` and `integrator_display/` are separate Arduino sketches (motors and
encoder; touchscreen and graphs). `integrator_logger.py` runs the experiment over USB, logs each
run and fits the convergence order.

**Limitation:** the encoder resolves 0.0015 rad, so the error is mechanical. In a one-run trial
the left sum's error fell as predicted (fitted order 0.75 against 1), but the θ² midpoint and
trapezoid results carried a ~6% bias that hid their h² errors, and I haven't found its cause.
With no endstops, the wheel is centred by hand each session.

| | |
|---|---|
| Controller | ESP32 + separate ESP32 touchscreen board (480×320) |
| Wheel encoder | AS5600, 4096 counts/rev (0.0015 rad) |
| Transmission | GT3 belts, 20:100 reduction, belt differential |
| Integrating wheel | ~25 mm radius, omni-wheel |
| Best result | within 0.9% of the exact integral (midpoint rule, fine segments) |

Libraries: TFT_eSPI, QRCodeGenerator (display); pyserial, NumPy, matplotlib (logger).

## [Eye switch](eye-switch/)

A hands-free switch for students with multiple disabilities who can't press a normal adapted
switch. An Android phone on the wheelchair tray watches the student's eyes, and a held look left,
look right, or blink presses a toy's switch, plays a song, or fills the screen with soft colour.

The app runs Google's MediaPipe Face Landmarker on the phone, and averages the two eyes into one
sideways reading:

```
h = (outRight + inLeft)/2 - (outLeft + inRight)/2
```

The camera sits at one end of the phone, so left and right are never symmetrical. Calibration
records `h` at the centre and at each target, so 1.0 always means "looking at that target". A
gesture must be held for the full hold time, fires once, and can't repeat until the student
looks away and a short gap passes.

A press goes over USB OTG to an ESP32 (`A250\n`), which closes a relay wired to a 3.5 mm jack,
the standard plug on switch-adapted toys. Lights only fade, well under the WCAG limit of three
flashes a second, and those timings are fixed in code.

**Limitation:** the model gives eye direction as blendshape scores, not a gaze vector, so the app
can tell left, right and blink apart but not where on screen someone is looking. It needs a lit
room, and accuracy hasn't been measured with students yet.

| | |
|---|---|
| App | Kotlin, CameraX, Android 8.0+ |
| Tracking | MediaPipe Face Landmarker, on-device |
| Hold time | 150 to 2000 ms, saved per student |
| Output | Relay contact closure, 3.5 mm mono jack, 50 to 2000 ms pulse |
| Link | USB OTG serial, 115200 baud |

Libraries: MediaPipe Tasks Vision, CameraX, usb-serial-for-android, Material Components.
Build and wiring steps are in [eye-switch/README.md](eye-switch/README.md).
