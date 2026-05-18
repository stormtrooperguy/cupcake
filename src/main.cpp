/*
  Cupcake Control System
  ESP32 firmware: animated mouth servo, NeoPixel eyes, flickering candle LED.
  Web interface served over WiFi AP — connect and navigate to http://192.168.4.1
*/

#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
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
const char* currentEyePath = "eye_yellow";
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
// Action queue: AsyncWebServer handlers enqueue requested paths; loop() drains
// the queue and runs the actual dispatch. Keeps every hardware operation
// (servo, FastLED) single-threaded inside loop() — no concurrent access with
// the async HTTP task.
// ---------------------------------------------------------------------------
#define ACTION_PATH_MAX  32
#define ACTION_QUEUE_DEPTH 8
struct ActionMsg { char path[ACTION_PATH_MAX]; };
QueueHandle_t actionQueue = NULL;

// ---------------------------------------------------------------------------
// Async web server + SSE
// ---------------------------------------------------------------------------
AsyncWebServer server(80);
AsyncEventSource events("/events");

// Page HTML, built once in setup(), reused for every "/" request
String pageHtml;

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
void buildStatusJson(String &out);
void pushStatus();
void buildPageHtml(String &out);
void dispatchAction(const char* path);
void setupWebServer();

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
        pushStatus();
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
}

// ---------------------------------------------------------------------------
// SSE helpers
// ---------------------------------------------------------------------------
void buildStatusJson(String &out) {
  out = "{\"biting\":";
  out += (biteState != BITE_IDLE ? "true" : "false");
  out += ",\"candle\":";
  out += (candleOn ? "true" : "false");
  out += ",\"currentEye\":\"";
  out += currentEyePath;
  out += "\"}";
}

void pushStatus() {
  if (events.count() == 0) return;
  String json;
  buildStatusJson(json);
  events.send(json.c_str(), "message", millis());
}

// ---------------------------------------------------------------------------
// Eye options
// ---------------------------------------------------------------------------
struct EyeOption { const char* path; CRGB color; };
static const EyeOption eyeOptions[] = {
  {"eye_yellow",  CRGB::Yellow},
  {"eye_red",     CRGB::Red},
  {"eye_blue",    CRGB::Blue},
  {"eye_green",   CRGB::Green},
  {"eye_purple",  CRGB::Purple},
  {"eye_off",     CRGB::Black}
};
static const int numEyeOptions = sizeof(eyeOptions) / sizeof(eyeOptions[0]);

// ---------------------------------------------------------------------------
// HTML helpers (used by buildPageHtml)
// ---------------------------------------------------------------------------
static String rgbToHex(const CRGB &c) {
  char buf[8];
  snprintf(buf, sizeof(buf), "#%02X%02X%02X", c.r, c.g, c.b);
  return String(buf);
}

static bool isLightColor(const CRGB &c) {
  int lum = (c.r * 299 + c.g * 587 + c.b * 114) / 1000;
  return lum > 180;
}

// ---------------------------------------------------------------------------
// Action dispatch — runs on the main task (called from loop())
// ---------------------------------------------------------------------------
void dispatchAction(const char* path) {
  if (strcmp(path, "bite") == 0) {
    triggerBite();
    pushStatus();
    return;
  }
  if (strcmp(path, "mouth_open") == 0) {
    if (biteState == BITE_IDLE) { mouthServo.write(SERVO_OPEN); }
    pushStatus();
    return;
  }
  if (strcmp(path, "mouth_close") == 0) {
    if (biteState == BITE_IDLE) { mouthServo.write(SERVO_CLOSED); }
    pushStatus();
    return;
  }
  if (strcmp(path, "candle") == 0) {
    candleOn = !candleOn;
    if (!candleOn) { fill_solid(candle, NUM_CANDLE_LEDS, CRGB::Black); FastLED.show(); }
    pushStatus();
    return;
  }
  for (int i = 0; i < numEyeOptions; i++) {
    if (strcmp(path, eyeOptions[i].path) == 0) {
      eyeNormalColor = eyeOptions[i].color;
      currentEyePath = eyeOptions[i].path;
      eyesRed = false;
      applyEyes();
      FastLED.show();
      pushStatus();
      return;
    }
  }
}

// ---------------------------------------------------------------------------
// Page HTML — built once at startup into pageHtml
// ---------------------------------------------------------------------------
void buildPageHtml(String &out) {
  out.reserve(4096);
  out = "<!DOCTYPE html><html>"
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
        ".btn-mouth { background:#5d6d7e; }"
        ".toggle-grid { display:grid; grid-template-columns:repeat(auto-fit,minmax(180px,1fr));"
        "gap:8px; max-width:800px; margin:0 auto 15px; }"
        ".toggle { background-color:#2a2a2a; border:1px solid #444; border-radius:6px; padding:12px 16px;"
        "cursor:pointer; display:flex; align-items:center; justify-content:space-between; gap:12px;"
        "font-size:15px; font-weight:bold; color:white; transition:all .2s;"
        "box-shadow:0 3px 5px rgba(0,0,0,.3); user-select:none; -webkit-tap-highlight-color:transparent; }"
        ".toggle:hover { background-color:#353535; transform:translateY(-2px); box-shadow:0 5px 9px rgba(0,0,0,.4); }"
        ".toggle:active { transform:translateY(0); }"
        ".toggle-switch { width:42px; height:24px; background:#555; border-radius:12px;"
        "position:relative; flex-shrink:0; transition:background .2s; }"
        ".toggle-switch::before { content:''; position:absolute; top:3px; left:3px;"
        "width:18px; height:18px; background:white; border-radius:50%; transition:transform .2s; }"
        ".toggle.on { background-color:#1f3a2a; border-color:#4ade80; }"
        ".toggle.on .toggle-switch { background:#4ade80; }"
        ".toggle.on .toggle-switch::before { transform:translateX(18px); }"
        ".status-bar { position:fixed; bottom:0; left:0; right:0; background:#2a2a2a;"
        "border-top:2px solid #444; padding:8px 11px; box-shadow:0 -2px 8px rgba(0,0,0,.5); }"
        ".status-bar h3 { margin:0 0 6px; font-size:11px; color:#888; text-align:center; }"
        ".status-grid { display:grid; grid-template-columns:repeat(auto-fit,minmax(120px,1fr));"
        "gap:6px; max-width:800px; margin:0 auto; font-size:9px; }"
        ".status-item { background:#1a1a1a; padding:5px 8px; border-radius:3px; border:1px solid #444; }"
        ".status-item strong { color:#aaa; margin-right:5px; }"
        "</style></head>"
        "<body><h1>&#127874; Cupcake</h1>";

  // Actions section
  out += "<h2>Actions</h2><div class=\"grid\">"
         "<button class=\"btn btn-bite\" data-path=\"bite\" onclick=\"t('bite')\">BITE</button>"
         "</div>";

  // Toggles section
  out += "<h2>Toggles</h2><div class=\"toggle-grid\">"
         "<div class=\"toggle\" id=\"tog-candle\" onclick=\"t('candle')\">"
         "<span>Candle</span><span class=\"toggle-switch\"></span>"
         "</div>"
         "</div>";

  // Calibration section
  out += "<h2>Calibration</h2><div class=\"grid\">"
         "<button class=\"btn btn-mouth\" data-path=\"mouth_open\" onclick=\"t('mouth_open')\">mouth open</button>"
         "<button class=\"btn btn-mouth\" data-path=\"mouth_close\" onclick=\"t('mouth_close')\">mouth close</button>"
         "</div>";

  // Eye Color section — buttons use their own CRGB color as background
  out += "<h2>Eye Color</h2><div class=\"grid\">";
  for (int i = 0; i < numEyeOptions; i++) {
    const EyeOption &opt = eyeOptions[i];
    // "eye_off" uses a dark grey so the button is visible
    CRGB btnColor = (strcmp(opt.path, "eye_off") == 0) ? CRGB(40, 40, 40) : opt.color;
    String hex = rgbToHex(btnColor);
    bool light = isLightColor(btnColor);
    // Strip "eye_" prefix for display label
    String label = String(opt.path).substring(4);

    out += "<button class=\"btn\" data-path=\"";
    out += opt.path;
    out += "\" onclick=\"t('";
    out += opt.path;
    out += "')\" style=\"background-color:";
    out += hex;
    out += ";color:";
    out += (light ? "#111" : "#fff");
    out += ";\">";
    out += label;
    out += "</button>";
  }
  out += "</div>";

  // Status bar
  out += "<div class=\"status-bar\"><h3>System Status</h3><div class=\"status-grid\">"
         "<div class=\"status-item\"><strong>Network:</strong> Cupcake (192.168.4.1)</div>"
         "<div class=\"status-item\"><strong>Candle:</strong> <span id=\"cv\">&mdash;</span></div>"
         "<div class=\"status-item\"><strong>Eyes:</strong> <span id=\"ey\">&mdash;</span></div>"
         "</div></div>";

  // Embedded JS: SSE for live status pushes, fire-and-forget action triggers
  out += "<script>"
         "function hl(p,on){const b=document.querySelector('[data-path=\"'+p+'\"]');if(b)b.classList.toggle('on',on);}"
         "function r(d){if(!d)return;"
         "document.getElementById('cv').textContent=d.candle?'On':'Off';"
         "document.getElementById('ey').textContent=d.currentEye.replace('eye_','').replace('_',' ');"
         "document.querySelectorAll('.btn.on').forEach(b=>b.classList.remove('on'));"
         "if(d.currentEye)hl(d.currentEye,true);"
         "document.getElementById('tog-candle').classList.toggle('on',!!d.candle);}"
         "async function t(p){try{await fetch('/a/'+p);}catch(e){}}"
         "const es=new EventSource('/events');"
         "es.onmessage=e=>{try{r(JSON.parse(e.data));}catch(err){}};"
         "</script>"
         "</body></html>";
}

// ---------------------------------------------------------------------------
// Web server setup
// ---------------------------------------------------------------------------
void setupWebServer() {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send(200, "text/html; charset=utf-8", pageHtml);
  });

  // SSE endpoint — clients open once, receive pushes
  events.onConnect([](AsyncEventSourceClient *client) {
    String json;
    buildStatusJson(json);
    client->send(json.c_str(), "message", millis());
  });
  server.addHandler(&events);

  // /a/<path> action dispatcher — enqueue for loop() to execute
  server.onNotFound([](AsyncWebServerRequest *req) {
    const String &url = req->url();
    if (req->method() == HTTP_GET && url.startsWith("/a/")) {
      if (actionQueue) {
        ActionMsg msg;
        strncpy(msg.path, url.c_str() + 3, ACTION_PATH_MAX - 1);
        msg.path[ACTION_PATH_MAX - 1] = '\0';
        xQueueSend(actionQueue, &msg, 0);  // non-blocking; drop if full
      }
      req->send(200, "text/plain", "OK");
    } else {
      req->send(404, "text/plain", "Not Found");
    }
  });

  server.begin();
}

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  Serial.println("Cupcake starting...");

  // Action queue (async handlers -> loop())
  actionQueue = xQueueCreate(ACTION_QUEUE_DEPTH, sizeof(ActionMsg));

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

  // Build page HTML once (static content, never changes at runtime)
  buildPageHtml(pageHtml);

  setupWebServer();
  Serial.println("Ready.");
}

// ---------------------------------------------------------------------------
// Loop
// ---------------------------------------------------------------------------
void loop() {
  // Drain any actions queued by the async HTTP task
  if (actionQueue) {
    ActionMsg msg;
    while (xQueueReceive(actionQueue, &msg, 0) == pdTRUE) {
      dispatchAction(msg.path);
    }
  }

  updateBite();
  updateCandle();

  delay(2);
}
