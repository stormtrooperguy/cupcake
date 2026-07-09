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

Cupcake runs its own `Cupcake` WiFi access point at all times. **At boot** it makes a single attempt (up to `STA_BOOT_JOIN_TIMEOUT_MS`, default 8s) to also join the shared `fazbear_sec` network (hosted by the springtrap animatronic). If `fazbear_sec` answers, cupcake stays in AP+STA mode and is reachable on both networks; if not, it shuts the station off and runs AP-only.

This is a one-time decision — cupcake **never re-scans** for `fazbear_sec` after boot. The reason is a hardware constraint: the ESP32 has a single radio, so a station that sits there *searching* for an absent network keeps yanking the radio off the SoftAP's channel and drops the AP's clients. By committing to "connected or off" at boot, the `Cupcake` AP stays rock-stable in every case. **Consequence:** if springtrap boots *after* cupcake, cupcake won't see it until cupcake is rebooted — so power springtrap on first, or reboot cupcake once springtrap is up.

Both APs and the station link are pinned to a fixed WiFi channel (`WIFI_CHANNEL`, default 1 — **must match springtrap's**) so that, when cupcake does join `fazbear_sec`, its shared radio never has to change channels.

Cupcake is **192.168.4.2** in both modes — connect to either network and navigate to **http://192.168.4.2**. `http://cupcake.local` (mDNS) also works, on platforms that support it (not Android browsers).

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

## Behavior

- **Eyes** default to yellow at startup. During a chomp they flicker to red, then flicker back to the resting color when the snap completes.
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
