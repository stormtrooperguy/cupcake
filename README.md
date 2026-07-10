# Cupcake Control System

ESP32 firmware for an animatronic cupcake with a snapping mouth servo, two NeoPixel eyes, and a flickering NeoPixel candle. Controlled via a web interface served over a WiFi access point.

## Hardware

| Component | Details |
|---|---|
| Microcontroller | ESP32 (DevKit) |
| Eye LEDs | 2× 7-pixel WS2812B ring (NeoPixel) — wired as one chain |
| Candle LED | 1× 7-pixel WS2812B ring (NeoPixel) — continues the same chain |
| Mouth | 1× servo motor |

## Wiring

| Signal | ESP32 GPIO | Notes |
|---|---|---|
| NeoPixel data | 13 | Via 300–500 Ω series resistor; chain order: left eye (0–6), right eye (7–13), candle (14–20) |
| Servo signal | 17 | Signal wire only |

Daisy-chain the NeoPixels in this order:

```
ESP32 GPIO 13 → left eye ring (IN)
               left eye ring (OUT) → right eye ring (IN)
                                     right eye ring (OUT) → candle ring (IN)
```

The servo and LEDs are powered externally (5 V). All grounds must be common.

## Web Interface

**At boot**, cupcake joins **either** the shared `fazbear_sec` network (hosted by the springtrap animatronic) as a station, **or** — if `fazbear_sec` isn't found — hosts its own `Cupcake` access point. It tries to join `fazbear_sec` for up to `STA_BOOT_JOIN_TIMEOUT_MS` (default 15s), re-attempting every `STA_ATTEMPT_INTERVAL_MS` so a flaky first association still lands; only if that whole window fails does it fall back to the `Cupcake` AP.

It's one or the other — **never both**. Running an AP and a station on the ESP32's single radio, both on the same `192.168.4.x` subnet with the same IP, broke routing (cupcake became unreachable from `fazbear_sec`) and was unstable besides. Committing to a single active interface avoids both problems. The decision is made once at boot and not revisited. **Consequence:** if springtrap boots *after* cupcake, cupcake will have already fallen back to its own AP — so power springtrap on first, or reboot cupcake once springtrap is up, for cupcake to join `fazbear_sec`.

Cupcake is **192.168.4.2** in either mode — connect to whichever network is active and navigate to **http://192.168.4.2**. `http://cupcake.local` (mDNS) also works, on platforms that support it (not Android browsers).

Passwords are defined in `src/secrets.h` (gitignored). Copy `src/secrets.h.example` to `src/secrets.h` and set your own values before building:

```cpp
#define AP_PASSWORD "yourpassword"          // Cupcake's own AP
#define FAZBEAR_PASSWORD "matchingpassword" // must match springtrap's AP_PASSWORD
```

| Control | What it does |
|---|---|
| **CHOMP** | Eyes flicker to red, mouth opens then snaps closed, eyes flicker back to resting color |
| **Candle** toggle | Turns the candle LED on/off (defaults to on) |
| **Glitch** toggle | Enables glitch mode: eyes occasionally malfunction, flickering off or red on one or both eyes |
| **yellow / red / blue / green / purple / off** | Sets the resting eye color |
| **mouth open / mouth close** | Moves the servo to the open or closed position for calibration |

Beyond the UI buttons, every action is reachable over HTTP as `GET /a/<action>` (e.g. `/a/bite`, `/a/eye_red`). One action has no button and is meant for remote drivers like springtrap: **`/a/bite_multi`** repeatedly chomps for ~8 seconds (`MULTI_BITE_MS`) instead of a single snap — springtrap fires this during its error phase so cupcake's jaw flaps for the whole sequence alongside it.

## Behavior

- **Eyes** default to yellow at startup. During a chomp they flicker to red, then flicker back to the resting color when the snap completes.
- **`bite_multi`** repeats the chomp back-to-back until `MULTI_BITE_MS` elapses. When the resting eye color is red (e.g. springtrap sent `eye_red` first), the per-chomp flicker is invisible so the eyes simply hold red while the jaw flaps.
- **Candle** flickers all 7 pixels through independent random orange/yellow/red values at 30–120 ms intervals when enabled, creating an organic flame effect. On by default.
- **Glitch** mode intermittently malfunctions one or both eyes, snapping to black or red for 1–3 rapid flicker cycles, then recovering. Events occur every 1.5–9 seconds. Suspends automatically during a chomp.
- **Chomp** is non-blocking: the mouth snaps open (`BITE_OPEN_MS`) then closes (`BITE_CLOSE_MS`) while the web server keeps responding.

## Configuration

All tunable values are `#define` constants at the top of [`src/main.cpp`](src/main.cpp).

### Pins
```cpp
#define LED_PIN   13
#define SERVO_PIN  17
```

### Servo positions (degrees)
```cpp
#define SERVO_CLOSED   90   // resting / closed
#define SERVO_OPEN     45   // snapping position
```

### Chomp timing
```cpp
#define BITE_OPEN_MS          400   // mouth-open dwell
#define BITE_CLOSE_MS         120   // pause after close before flicker-out begins
#define BITE_FLICKER_HALF_MS   60   // duration of each on/off half-cycle during eye flicker
#define BITE_FLICKER_COUNT      3   // number of half-cycle transitions (odd = ends on target color)
```

### Eye brightness
```cpp
#define EYE_BRIGHTNESS   50   // 0–255
```

### Candle flicker rate
```cpp
#define CANDLE_FLICKER_MIN_MS    30
#define CANDLE_FLICKER_MAX_MS   120
```

## Building and Flashing

Uses [PlatformIO](https://platformio.org/).

```bash
pio run                    # build
pio run --target upload    # flash
pio device monitor         # serial output
```
