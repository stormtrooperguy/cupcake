# Cupcake Control System

ESP32 firmware for an animatronic cupcake with a snapping mouth servo, two NeoPixel eyes, and a flickering NeoPixel candle. Controlled via a web interface served over a WiFi access point.

## Hardware

| Component | Details |
|---|---|
| Microcontroller | ESP32 (DevKit) |
| Eye LEDs | 2× 7-pixel WS2812B ring (NeoPixel) — wired as one chain |
| Candle LED | 1× WS2812B (NeoPixel) — continues the same chain |
| Mouth | 1× servo motor |

## Wiring

| Signal | ESP32 GPIO | Notes |
|---|---|---|
| NeoPixel data | 5 | Via 300–500 Ω series resistor; chain order: left eye (0–6), right eye (7–13), candle (14) |
| Servo signal | 17 | Signal wire only |

Daisy-chain the NeoPixels in this order:

```
ESP32 GPIO 5 → left eye ring (IN)
               left eye ring (OUT) → right eye ring (IN)
                                     right eye ring (OUT) → candle pixel (IN)
```

The servo and LEDs are powered externally (5 V). All grounds must be common.

## Web Interface

Connect to the `Cupcake` WiFi network (password: `changeme`) and navigate to **http://192.168.4.1**.

| Button | What it does |
|---|---|
| **BITE** | Opens then snaps the mouth closed; eyes flash red during the snap |
| **candle** | Toggles the candle LED on/off |
| **warm white / red / blue / green / purple / off** | Sets the resting eye color |

## Behavior

- **Eyes** default to warm white at startup. During a bite they go red, then revert to the resting color when the snap completes.
- **Candle** flickers through random orange/yellow/red values at 30–120 ms intervals when enabled.
- **Bite** is non-blocking: the mouth snaps open (`BITE_OPEN_MS`) then closes (`BITE_CLOSE_MS`) while the web server keeps responding.

## Configuration

All tunable values are `#define` constants at the top of [`src/main.cpp`](src/main.cpp).

### Pins
```cpp
#define LED_PIN    5
#define SERVO_PIN  17
```

### Servo positions (degrees)
```cpp
#define SERVO_CLOSED   90   // resting / closed
#define SERVO_OPEN     45   // snapping position
```

### Bite timing
```cpp
#define BITE_OPEN_MS    150   // mouth-open dwell
#define BITE_CLOSE_MS   120   // pause after close before restoring eyes
```

### Eye brightness
```cpp
#define EYE_BRIGHTNESS   80   // 0–255
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
