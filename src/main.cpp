/*
  Cupcake Control System
  ESP32 firmware: animated mouth servo, NeoPixel eyes, flickering candle LED.

  Runs its own "Cupcake" WiFi AP (http://192.168.4.2) at all times, and also
  tries to join the shared "fazbear_sec" network (hosted by the springtrap
  animatronic) if it's in range -- both interfaces run simultaneously
  (WIFI_AP_STA). Cupcake is 192.168.4.2 in both modes, so springtrap can
  always reach it there directly. The web interface is also reachable via
  http://cupcake.local (mDNS) for convenience on platforms that support it
  (not Android browsers).
*/

#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <esp_system.h>  // esp_random() -- hardware RNG, reliable with WiFi active
#include <FastLED.h>
#include <ESP32Servo.h>
#include "secrets.h"

// ---------------------------------------------------------------------------
// Identity
// ---------------------------------------------------------------------------
const char* ap_ssid     = "Cupcake";
const char* ap_password = AP_PASSWORD;
const char* mdns_host   = "cupcake";        // reachable at http://cupcake.local

// Shared network hosted by springtrap. Joined opportunistically alongside
// this device's own AP -- if it's not in range, WiFi.begin() just never
// connects and cupcake keeps working fine on its own AP.
const char* fazbear_ssid     = "fazbear_sec";
const char* fazbear_password = FAZBEAR_PASSWORD;

// Fixed WiFi channel shared by every device on this rig. The ESP32 has one
// radio, so in AP+STA mode the SoftAP and station must live on the same
// channel. Pinning our AP, springtrap's AP (fazbear_sec), and our station's
// probe to this one channel keeps the radio from ever hopping off it --
// otherwise the station's scanning (or connecting to an AP on a different
// channel) drops our own AP clients. MUST match springtrap's WIFI_CHANNEL.
#define WIFI_CHANNEL 1

// STA connection status tracking, serial-logged on change only. Cosmetic --
// the web server listens on all interfaces regardless of this state.
wl_status_t   lastWifiStatus  = WL_IDLE_STATUS;
unsigned long lastWifiCheckMs = 0;
#define WIFI_CHECK_MS 5000

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
// Glitch state
// ---------------------------------------------------------------------------
bool          glitchOn          = false;
unsigned long glitchNextMs      = 0;   // when next glitch event fires (quiet phase)
int           glitchStep        = 0;   // half-cycles remaining (0 = quiet)
unsigned long glitchStepMs      = 0;   // when current half-cycle ends
int           glitchEyeMask     = 0;   // which eyes (1=left, 2=right, 3=both)
CRGB          glitchColor;             // glitch color for this event

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
  // Reset glitch so it starts fresh after the bite completes
  glitchStep = 0;
}

// ---------------------------------------------------------------------------
// Glitch eye animation — call every loop iteration
//
// When enabled, eyes normally show eyeNormalColor but intermittently
// malfunction: one or both eyes flicker 1–3 times (rapid half-cycles of
// glitch color / normal, ~30–70 ms each), then recover. Quiet gap between
// events is 1500–9000 ms. Suspends during the bite sequence.
// ---------------------------------------------------------------------------
void updateGlitch() {
  if (!glitchOn || biteState != BITE_IDLE) return;
  unsigned long now = millis();

  if (glitchStep > 0) {
    // Mid-flicker — wait for current half-cycle to expire
    if (now >= glitchStepMs) {
      glitchStep--;
      if (glitchStep == 0) {
        // Sequence complete — restore eyes and schedule next event
        applyEyes();
        FastLED.show();
        glitchNextMs = now + random(1500, 9000);
      } else if (glitchStep % 2 == 1) {
        // Odd steps remaining → show normal between flickers
        applyEyes();
        FastLED.show();
        glitchStepMs = now + random(30, 70);
      } else {
        // Even steps remaining → show glitch color again
        if (glitchEyeMask & 1) fill_solid(leftEye,  NUM_EYE_LEDS, glitchColor);
        if (glitchEyeMask & 2) fill_solid(rightEye, NUM_EYE_LEDS, glitchColor);
        FastLED.show();
        glitchStepMs = now + random(30, 70);
      }
    }
  } else {
    // Quiet phase — wait for next event
    if (now >= glitchNextMs) {
      glitchEyeMask = random(3) + 1;                                      // 1=left 2=right 3=both
      glitchColor   = (random(2) == 0) ? CRGB::Black : scaleEye(CRGB::Red);
      glitchStep    = random(1, 4) * 2;                                   // 1–3 flickers → 2–6 half-cycles
      // Show first glitch half-cycle immediately
      if (glitchEyeMask & 1) fill_solid(leftEye,  NUM_EYE_LEDS, glitchColor);
      if (glitchEyeMask & 2) fill_solid(rightEye, NUM_EYE_LEDS, glitchColor);
      FastLED.show();
      glitchStepMs = now + random(30, 70);
    }
  }
}

// ---------------------------------------------------------------------------
// SSE helpers
// ---------------------------------------------------------------------------
void buildStatusJson(String &out) {
  out = "{\"biting\":";
  out += (biteState != BITE_IDLE ? "true" : "false");
  out += ",\"candle\":";
  out += (candleOn ? "true" : "false");
  out += ",\"glitch\":";
  out += (glitchOn ? "true" : "false");
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
  if (strcmp(path, "glitch") == 0) {
    glitchOn = !glitchOn;
    glitchStep = 0;
    if (!glitchOn) { applyEyes(); FastLED.show(); }
    else { glitchNextMs = millis() + random(1500, 9000); }
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
        // Chomp button — large circle, centred
        ".chomp-wrap { max-width:800px; margin:0 auto 15px; display:flex; justify-content:center; }"
        ".btn-chomp { width:200px; height:200px; border:none; border-radius:50%; background:#c0392b;"
        "color:#fff; font-family:inherit; font-size:26px; font-weight:bold; cursor:pointer;"
        "transition:all .2s; box-shadow:0 4px 8px rgba(0,0,0,.4); }"
        ".btn-chomp:hover { transform:translateY(-2px); box-shadow:0 6px 12px rgba(0,0,0,.5); opacity:.9; }"
        ".btn-chomp:active { transform:translateY(0); box-shadow:0 2px 4px rgba(0,0,0,.3); }"
        // Round button grid — like duckling
        ".button-grid { display:grid; grid-template-columns:repeat(auto-fit,88px);"
        "gap:10px; max-width:800px; margin:0 auto 15px; justify-content:center; }"
        ".btn { border:none; border-radius:50%; aspect-ratio:1/1; width:88px;"
        "color:#fff; padding:6px; font-family:inherit; font-size:12px; font-weight:bold;"
        "cursor:pointer; transition:all .2s; display:flex; align-items:center; justify-content:center;"
        "text-align:center; box-shadow:0 3px 5px rgba(0,0,0,.3); }"
        ".btn:hover { transform:translateY(-2px); box-shadow:0 5px 9px rgba(0,0,0,.4); opacity:.9; }"
        ".btn:active { transform:translateY(0); box-shadow:0 2px 3px rgba(0,0,0,.3); }"
        ".btn.on { outline:3px solid #fff; outline-offset:-3px; filter:brightness(1.3); }"
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

  // Chomp button — full-width on its own row
  out += "<h2>Actions</h2><div class=\"chomp-wrap\">"
         "<button class=\"btn-chomp\" data-path=\"bite\" onclick=\"t('bite')\">CHOMP</button>"
         "</div>";

  // Toggles section
  out += "<h2>Toggles</h2><div class=\"toggle-grid\">"
         "<div class=\"toggle\" id=\"tog-candle\" onclick=\"t('candle')\">"
         "<span>Candle</span><span class=\"toggle-switch\"></span>"
         "</div>"
         "<div class=\"toggle\" id=\"tog-glitch\" onclick=\"t('glitch')\">"
         "<span>Glitch</span><span class=\"toggle-switch\"></span>"
         "</div>"
         "</div>";

  // Calibration section — round buttons
  out += "<h2>Calibration</h2><div class=\"button-grid\">"
         "<button class=\"btn btn-mouth\" data-path=\"mouth_open\" onclick=\"t('mouth_open')\">mouth open</button>"
         "<button class=\"btn btn-mouth\" data-path=\"mouth_close\" onclick=\"t('mouth_close')\">mouth close</button>"
         "</div>";

  // Eye Color section — round buttons, each colored with its own CRGB value
  out += "<h2>Eye Color</h2><div class=\"button-grid\">";
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
         "<div class=\"status-item\"><strong>Network:</strong> 192.168.4.2 (Cupcake AP or fazbear_sec) &mdash; http://cupcake.local</div>"
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
         "document.getElementById('tog-candle').classList.toggle('on',!!d.candle);"
         "document.getElementById('tog-glitch').classList.toggle('on',!!d.glitch);}"
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

  randomSeed(esp_random());   // hardware RNG -- reliable even with WiFi active

  // Run our own AP and try to join the shared fazbear_sec network at the
  // same time (WIFI_AP_STA). If fazbear_sec isn't in range, WiFi.begin()
  // just never connects -- the AP keeps working fine on its own either way.
  //
  // Cupcake is always 192.168.4.2, in both modes: as our own AP's address
  // (unconventional -- normally the host is .1 -- but these are two
  // separate, unrelated networks, so there's no conflict) and as a static
  // IP on fazbear_sec (springtrap, the AP host there, owns .1). This gives
  // springtrap a fixed, known address to reach cupcake at without needing
  // mDNS for that specific link.
  WiFi.mode(WIFI_AP_STA);

  IPAddress local_IP(192, 168, 4, 2);
  IPAddress gateway(192, 168, 4, 2);
  IPAddress subnet(255, 255, 255, 0);
  WiFi.softAPConfig(local_IP, gateway, subnet);
  WiFi.softAP(ap_ssid, ap_password, WIFI_CHANNEL);

  Serial.print("AP started: "); Serial.println(ap_ssid);
  Serial.print("AP IP: ");      Serial.println(WiFi.softAPIP());

  // Static IP on the fazbear_sec side too -- springtrap owns .1 there.
  WiFi.config(IPAddress(192, 168, 4, 2), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
  // Pass WIFI_CHANNEL so the station only probes that one channel instead of
  // scanning all of them -- a full scan yanks the shared radio off our AP's
  // channel and drops AP clients, which is especially bad while fazbear_sec
  // is absent (springtrap off) and the station would otherwise scan forever.
  WiFi.begin(fazbear_ssid, fazbear_password, WIFI_CHANNEL);
  Serial.print("Also attempting to join: "); Serial.println(fazbear_ssid);

  if (MDNS.begin(mdns_host)) {
    MDNS.addService("http", "tcp", 80);
    Serial.print("mDNS responder started: http://");
    Serial.print(mdns_host);
    Serial.println(".local");
  } else {
    Serial.println("mDNS init failed");
  }

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
  updateGlitch();
  updateCandle();

  // Log fazbear_sec join/drop transitions -- doesn't affect anything else,
  // the control page works the same whether or not this connects.
  if (millis() - lastWifiCheckMs >= WIFI_CHECK_MS) {
    lastWifiCheckMs = millis();
    wl_status_t st = WiFi.status();
    if (st != lastWifiStatus) {
      lastWifiStatus = st;
      if (st == WL_CONNECTED) {
        Serial.print("Joined fazbear_sec, IP: ");
        Serial.println(WiFi.localIP());
      } else {
        Serial.println("Not connected to fazbear_sec (still reachable via Cupcake AP / cupcake.local)");
      }
    }
  }

  delay(2);
}
