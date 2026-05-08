/*
  Cupcake Control System
  ESP32 firmware: animated mouth servo, NeoPixel eyes, flickering candle LED.
  Web interface served over WiFi AP — connect and navigate to http://192.168.4.1
*/

#include <WiFi.h>
#include <FastLED.h>
#include <ESP32Servo.h>

// ---------------------------------------------------------------------------
// Identity
// ---------------------------------------------------------------------------
const char* ap_ssid     = "Cupcake";
const char* ap_password = "changeme";   // 8-63 chars

// ---------------------------------------------------------------------------
// Hardware pins — adjust after physical install
// ---------------------------------------------------------------------------
#define LED_PIN   13    // NeoPixel data line
#define SERVO_PIN  17   // Mouth servo signal

// LED chain: left eye = 0–6, right eye = 7–13, candle = 14–20
#define NUM_EYE_LEDS    7
#define NUM_CANDLE_LEDS 7
#define NUM_LEDS       21   // 7 + 7 + 7

// ---------------------------------------------------------------------------
// Servo positions (degrees) — adjust after physical install
// ---------------------------------------------------------------------------
#define SERVO_CLOSED   90   // mouth closed / resting position
#define SERVO_OPEN     45   // mouth open (snapping position)

// ---------------------------------------------------------------------------
// Bite timing — adjust to taste
// ---------------------------------------------------------------------------
#define BITE_OPEN_MS          400   // how long mouth stays open during snap
#define BITE_CLOSE_MS         120   // pause after closing before flicker-out begins
#define BITE_FLICKER_HALF_MS   60   // duration of each on/off half-cycle during eye flicker
#define BITE_FLICKER_COUNT      3   // number of half-cycle transitions (odd = ends on target color)

// ---------------------------------------------------------------------------
// Eye brightness (0–255)
// ---------------------------------------------------------------------------
#define EYE_BRIGHTNESS   50

// ---------------------------------------------------------------------------
// Candle flicker timing
// ---------------------------------------------------------------------------
#define CANDLE_FLICKER_MIN_MS    30
#define CANDLE_FLICKER_MAX_MS   120

// ---------------------------------------------------------------------------
// LED array and servo
// ---------------------------------------------------------------------------
CRGB leds[NUM_LEDS];
CRGB * const leftEye  = leds;
CRGB * const rightEye = leds + NUM_EYE_LEDS;
CRGB * const candle   = leds + NUM_EYE_LEDS * 2;
Servo mouthServo;

// ---------------------------------------------------------------------------
// Eye state
// ---------------------------------------------------------------------------
CRGB eyeNormalColor = CRGB::Yellow;
char currentEyePath[32] = "eye_yellow";
bool eyesRed = false;

// ---------------------------------------------------------------------------
// Bite state machine
// ---------------------------------------------------------------------------
enum BiteState { BITE_IDLE, BITE_FLICKER_IN, BITE_OPENING, BITE_CLOSING, BITE_FLICKER_OUT };
BiteState     biteState   = BITE_IDLE;
unsigned long biteStepMs  = 0;
int           flickerStep = 0;

// ---------------------------------------------------------------------------
// Candle state
// ---------------------------------------------------------------------------
bool          candleOn      = true;
unsigned long candleNextMs  = 0;

// ---------------------------------------------------------------------------
// Web server
// ---------------------------------------------------------------------------
WiFiServer server(80);
#define MAX_HEADER_SIZE 512
#define MAX_PATH_SIZE    32
char          header[MAX_HEADER_SIZE];
int           headerLen   = 0;
unsigned long currentTime  = 0;
unsigned long previousTime = 0;
const long    timeoutTime  = 1000;

char lastAction[48] = "ready";

// ---------------------------------------------------------------------------
// LED helpers
// ---------------------------------------------------------------------------
CRGB scaleEye(CRGB c) {
  return CRGB(
    (uint16_t)c.r * EYE_BRIGHTNESS / 255,
    (uint16_t)c.g * EYE_BRIGHTNESS / 255,
    (uint16_t)c.b * EYE_BRIGHTNESS / 255
  );
}

void applyEyes() {
  CRGB c = eyesRed ? scaleEye(CRGB::Red) : scaleEye(eyeNormalColor);
  fill_solid(leftEye,  NUM_EYE_LEDS, c);
  fill_solid(rightEye, NUM_EYE_LEDS, c);
}

// ---------------------------------------------------------------------------
// Candle flicker — call every loop iteration
// Each pixel gets an independent random flame color so the ring looks organic.
// ---------------------------------------------------------------------------
void updateCandle() {
  if (!candleOn) return;
  unsigned long now = millis();
  if (now >= candleNextMs) {
    for (int i = 0; i < NUM_CANDLE_LEDS; i++) {
      candle[i] = CRGB(random(180, 256), random(20, 140), 0);
    }
    candleNextMs = now + random(CANDLE_FLICKER_MIN_MS, CANDLE_FLICKER_MAX_MS);
    FastLED.show();
  }
}

// ---------------------------------------------------------------------------
// Bite state machine — call every loop iteration
//
// Full sequence:
//   FLICKER_IN  → eyes alternate normal/red BITE_FLICKER_COUNT times → ends red
//   OPENING     → servo opens, eyes hold red for BITE_OPEN_MS
//   CLOSING     → servo closes, hold for BITE_CLOSE_MS
//   FLICKER_OUT → eyes alternate red/normal BITE_FLICKER_COUNT times → ends normal
// ---------------------------------------------------------------------------
void updateBite() {
  if (biteState == BITE_IDLE) return;
  unsigned long now = millis();

  if (biteState == BITE_FLICKER_IN) {
    if (now - biteStepMs >= BITE_FLICKER_HALF_MS) {
      flickerStep++;
      if (flickerStep >= BITE_FLICKER_COUNT) {
        eyesRed = true;
        applyEyes();
        mouthServo.write(SERVO_OPEN);
        FastLED.show();
        biteState  = BITE_OPENING;
        biteStepMs = now;
      } else {
        eyesRed = !eyesRed;
        applyEyes();
        FastLED.show();
        biteStepMs = now;
      }
    }

  } else if (biteState == BITE_OPENING) {
    if (now - biteStepMs >= BITE_OPEN_MS) {
      mouthServo.write(SERVO_CLOSED);
      biteState  = BITE_CLOSING;
      biteStepMs = now;
    }

  } else if (biteState == BITE_CLOSING) {
    if (now - biteStepMs >= BITE_CLOSE_MS) {
      flickerStep = 0;
      biteState   = BITE_FLICKER_OUT;
      biteStepMs  = now;
    }

  } else if (biteState == BITE_FLICKER_OUT) {
    if (now - biteStepMs >= BITE_FLICKER_HALF_MS) {
      flickerStep++;
      if (flickerStep >= BITE_FLICKER_COUNT) {
        eyesRed = false;
        applyEyes();
        FastLED.show();
        biteState = BITE_IDLE;
        strncpy(lastAction, "bite done", sizeof(lastAction) - 1);
      } else {
        eyesRed = !eyesRed;
        applyEyes();
        FastLED.show();
        biteStepMs = now;
      }
    }
  }
}

void triggerBite() {
  if (biteState != BITE_IDLE) return;
  eyesRed     = false;   // start from normal color; flicker_in will toggle toward red
  flickerStep = 0;
  biteState   = BITE_FLICKER_IN;
  biteStepMs  = millis();
  strncpy(lastAction, "biting!", sizeof(lastAction) - 1);
}

// ---------------------------------------------------------------------------
// HTTP helpers
// ---------------------------------------------------------------------------
bool parseRequestUrl(const char* buf, char* out, int maxLen) {
  if (strncmp(buf, "GET ", 4) != 0) { out[0] = '\0'; return false; }
  const char* p = buf + 4;
  int i = 0;
  while (i < maxLen - 1 && *p && *p != ' ' && *p != '?' && *p != '\r' && *p != '\n')
    out[i++] = *p++;
  out[i] = '\0';
  return i > 0;
}

void sendStatusJSON(WiFiClient &client) {
  client.println("HTTP/1.1 200 OK");
  client.println("Content-type:application/json");
  client.println("Connection: close");
  client.println();
  client.print("{\"last\":\"");       client.print(lastAction);
  client.print("\",\"biting\":");     client.print(biteState != BITE_IDLE ? "true" : "false");
  client.print(",\"candle\":");       client.print(candleOn ? "true" : "false");
  client.print(",\"currentEye\":\""); client.print(currentEyePath);
  client.println("\"}");
}

void sendNotFound(WiFiClient &client) {
  client.println("HTTP/1.1 404 Not Found");
  client.println("Content-type:text/plain");
  client.println("Connection: close");
  client.println();
  client.println("Not Found");
}

// ---------------------------------------------------------------------------
// Action dispatch
// ---------------------------------------------------------------------------
struct EyeOption { const char* path; CRGB color; };
static const EyeOption eyeOptions[] = {
  {"eye_yellow",     CRGB::Yellow},
  {"eye_red",        CRGB::Red},
  {"eye_blue",       CRGB::Blue},
  {"eye_green",      CRGB::Green},
  {"eye_purple",     CRGB::Purple},
  {"eye_off",        CRGB::Black}
};
static const int numEyeOptions = sizeof(eyeOptions) / sizeof(eyeOptions[0]);

void dispatchAction(const char* path) {
  if (strcmp(path, "bite") == 0) {
    triggerBite();
    return;
  }
  if (strcmp(path, "mouth_open") == 0) {
    if (biteState == BITE_IDLE) { mouthServo.write(SERVO_OPEN); strncpy(lastAction, "mouth open", sizeof(lastAction) - 1); }
    return;
  }
  if (strcmp(path, "mouth_close") == 0) {
    if (biteState == BITE_IDLE) { mouthServo.write(SERVO_CLOSED); strncpy(lastAction, "mouth closed", sizeof(lastAction) - 1); }
    return;
  }
  if (strcmp(path, "candle") == 0) {
    candleOn = !candleOn;
    if (!candleOn) { fill_solid(candle, NUM_CANDLE_LEDS, CRGB::Black); FastLED.show(); }
    strncpy(lastAction, candleOn ? "candle on" : "candle off", sizeof(lastAction) - 1);
    return;
  }
  for (int i = 0; i < numEyeOptions; i++) {
    if (strcmp(path, eyeOptions[i].path) == 0) {
      eyeNormalColor = eyeOptions[i].color;
      strncpy(currentEyePath, eyeOptions[i].path, sizeof(currentEyePath) - 1);
      eyesRed = false;
      applyEyes();
      FastLED.show();
      strncpy(lastAction, path, sizeof(lastAction) - 1);
      return;
    }
  }
}

// ---------------------------------------------------------------------------
// Page HTML — streamed in chunks to avoid large stack allocations
// ---------------------------------------------------------------------------
void sendPageHTML(WiFiClient &client) {
  client.println("HTTP/1.1 200 OK");
  client.println("Content-type:text/html");
  client.println("Connection: close");
  client.println();

  // Head + styles
  client.print(
    "<!DOCTYPE html><html>"
    "<head><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<link rel=\"icon\" href=\"data:,\">"
    "<style>"
    "* { margin:0; padding:0; box-sizing:border-box; }"
    "html { font-family:Helvetica,Arial,sans-serif; }"
    "body { background:#1a1a1a; color:#fff; padding:11px; padding-bottom:84px; }"
    "h1 { text-align:center; margin-bottom:15px; font-size:24px; }"
    "h2 { text-align:center; margin:15px 0 8px; font-size:17px; color:#aaa; }"
    ".grid { display:grid; grid-template-columns:repeat(auto-fit,minmax(120px,1fr));"
    "gap:8px; max-width:800px; margin:0 auto 15px; }"
    ".btn { border:none; border-radius:6px; color:#fff; padding:15px 11px;"
    "font-family:inherit; font-size:15px; font-weight:bold; cursor:pointer;"
    "transition:all .2s; text-align:center; box-shadow:0 3px 5px rgba(0,0,0,.3); }"
    ".btn:hover { transform:translateY(-2px); box-shadow:0 5px 9px rgba(0,0,0,.4); opacity:.9; }"
    ".btn:active { transform:translateY(0); box-shadow:0 2px 3px rgba(0,0,0,.3); }"
    ".btn.on { outline:3px solid #fff; outline-offset:-3px; filter:brightness(1.3); }"
    ".btn-bite { background:#c0392b; font-size:22px; padding:22px; grid-column:1/-1; }"
    ".btn-candle { background:#d35400; }"
    ".btn-mouth { background:#5d6d7e; }"
    ".btn-eye { background:#2471a3; }"
    ".status-bar { position:fixed; bottom:0; left:0; right:0; background:#2a2a2a;"
    "border-top:2px solid #444; padding:8px 11px; box-shadow:0 -2px 8px rgba(0,0,0,.5); }"
    ".status-bar h3 { margin:0 0 6px; font-size:11px; color:#888; text-align:center; }"
    ".status-grid { display:grid; grid-template-columns:repeat(auto-fit,minmax(120px,1fr));"
    "gap:6px; max-width:800px; margin:0 auto; font-size:9px; }"
    ".status-item { background:#1a1a1a; padding:5px 8px; border-radius:3px; border:1px solid #444; }"
    ".status-item strong { color:#aaa; margin-right:5px; }"
    "</style></head>"
    "<body><h1>&#127874; Cupcake</h1>"
  );

  // Actions section
  client.print(
    "<h2>Actions</h2><div class=\"grid\">"
    "<button class=\"btn btn-bite\" data-path=\"bite\" onclick=\"t('bite')\">BITE</button>"
    "<button class=\"btn btn-candle\" data-path=\"candle\" onclick=\"t('candle')\">candle</button>"
    "</div>"
  );

  // Calibration section
  client.print(
    "<h2>Calibration</h2><div class=\"grid\">"
    "<button class=\"btn btn-mouth\" data-path=\"mouth_open\" onclick=\"t('mouth_open')\">mouth open</button>"
    "<button class=\"btn btn-mouth\" data-path=\"mouth_close\" onclick=\"t('mouth_close')\">mouth close</button>"
    "</div>"
  );

  // Eye color section
  client.print(
    "<h2>Eye Color</h2><div class=\"grid\">"
    "<button class=\"btn btn-eye\" data-path=\"eye_yellow\" onclick=\"t('eye_yellow')\">yellow</button>"
    "<button class=\"btn btn-eye\" data-path=\"eye_red\" onclick=\"t('eye_red')\">red</button>"
    "<button class=\"btn btn-eye\" data-path=\"eye_blue\" onclick=\"t('eye_blue')\">blue</button>"
    "<button class=\"btn btn-eye\" data-path=\"eye_green\" onclick=\"t('eye_green')\">green</button>"
    "<button class=\"btn btn-eye\" data-path=\"eye_purple\" onclick=\"t('eye_purple')\">purple</button>"
    "<button class=\"btn btn-eye\" data-path=\"eye_off\" onclick=\"t('eye_off')\">off</button>"
    "</div>"
  );

  // Status bar
  client.print(
    "<div class=\"status-bar\"><h3>System Status</h3><div class=\"status-grid\">"
    "<div class=\"status-item\"><strong>Network:</strong> Cupcake (192.168.4.1)</div>"
    "<div class=\"status-item\"><strong>Candle:</strong> <span id=\"cv\">&mdash;</span></div>"
    "<div class=\"status-item\"><strong>Eyes:</strong> <span id=\"ey\">&mdash;</span></div>"
    "<div class=\"status-item\"><strong>Last:</strong> <span id=\"la\">&mdash;</span></div>"
    "</div></div>"
  );

  // Embedded JS: button dispatch + 2s status polling + highlight management
  client.print(
    "<script>"
    "function hl(p,on){const b=document.querySelector('[data-path=\"'+p+'\"]');if(b)b.classList.toggle('on',on);}"
    "function r(d){if(!d)return;"
    "document.getElementById('la').textContent=d.last;"
    "document.getElementById('cv').textContent=d.candle?'On':'Off';"
    "document.getElementById('ey').textContent=d.currentEye.replace('eye_','').replace('_',' ');"
    "document.querySelectorAll('.btn-eye.on').forEach(b=>b.classList.remove('on'));"
    "hl(d.currentEye,true);"
    "hl('candle',d.candle);}"
    "async function t(p){try{const x=await fetch('/a/'+p);r(await x.json());}catch(e){}}"
    "async function q(){try{const x=await fetch('/status');r(await x.json());}catch(e){}}"
    "setInterval(q,2000);q();"
    "</script>"
    "</body></html>"
  );
  client.println();
}

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  Serial.println("Cupcake starting...");

  FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, NUM_LEDS);
  FastLED.clear();
  applyEyes();
  fill_solid(candle, NUM_CANDLE_LEDS, CRGB::Black);
  FastLED.show();

  mouthServo.attach(SERVO_PIN);
  mouthServo.write(SERVO_CLOSED);

  randomSeed(analogRead(0));

  IPAddress local_IP(192, 168, 4, 1);
  IPAddress gateway(192, 168, 4, 1);
  IPAddress subnet(255, 255, 255, 0);
  WiFi.softAPConfig(local_IP, gateway, subnet);
  WiFi.softAP(ap_ssid, ap_password);

  Serial.print("AP started: "); Serial.println(ap_ssid);
  Serial.print("IP: ");         Serial.println(WiFi.softAPIP());

  server.begin();
  Serial.println("Ready.");
}

// ---------------------------------------------------------------------------
// Loop
// ---------------------------------------------------------------------------
void loop() {
  updateBite();
  updateCandle();

  WiFiClient client = server.accept();
  if (!client) return;

  client.setNoDelay(true);
  currentTime  = millis();
  previousTime = currentTime;
  headerLen    = 0;
  header[0]    = '\0';
  int lineLen  = 0;

  while (client.connected() && currentTime - previousTime <= timeoutTime) {
    currentTime = millis();
    if (client.available()) {
      char c = client.read();
      if (headerLen < MAX_HEADER_SIZE - 1) { header[headerLen++] = c; header[headerLen] = '\0'; }
      if (c == '\n') {
        if (lineLen == 0) {
          char url[MAX_PATH_SIZE];
          if (parseRequestUrl(header, url, MAX_PATH_SIZE)) {
            if (strncmp(url, "/a/", 3) == 0) {
              dispatchAction(url + 3);
              sendStatusJSON(client);
            } else if (strcmp(url, "/status") == 0) {
              sendStatusJSON(client);
            } else if (strcmp(url, "/") == 0) {
              sendPageHTML(client);
            } else {
              sendNotFound(client);
            }
          } else {
            sendNotFound(client);
          }
          break;
        }
        lineLen = 0;
      } else if (c != '\r') {
        lineLen++;
      }
    }
  }

  headerLen = 0;
  header[0] = '\0';
  client.stop();
}
