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
