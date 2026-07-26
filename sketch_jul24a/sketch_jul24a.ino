/*
 * ============================================================
 *  AC CONDENSATE AUTO-DRAIN CONTROLLER   (ESP32)
 *  Timed pump cycle + web dashboard + Telegram control
 * ============================================================
 *
 *  BEHAVIOUR
 *    Reed HIGH closes (water at 70%)
 *        -> pump runs for 6 minutes, then stops. If the float is still wet,
 *           a fresh 6-minute cycle starts after a short gap (repeats).
 *    Reed OVERFLOW closes (water at 90%)
 *        -> pump forced ON to clear it, red LED + buzzer, Telegram alert.
 *           If a full 6-minute run does not drop the level, the pump is
 *           stopped (it plainly isn't draining) and a "check for a blockage"
 *           alert goes out. Better a wet tray than a burnt-out pump.
 *    Manual rocker switch
 *        -> pump runs for exactly as long as the switch is closed, no cap.
 *
 *  PRIORITY   overflow > manual switch > web/Telegram manual > auto cycle
 *
 *  LEDS
 *    White  - wired DIRECTLY to 3V3 through a resistor, no GPIO. Lit whenever
 *             the board has power; it can't lie by staying on after a crash.
 *    Green  - GPIO, lit only while the pump relay is energised.
 *    Red    - GPIO, lit while the 90% overflow float is wet.
 *
 *  WEB
 *    GET  /              dashboard
 *    GET  /api/status    JSON status
 *    POST /pump/on       manual run, capped at 6 min
 *    POST /pump/off      stop immediately
 *
 *  TELEGRAM   /status  /pumpon  /pumpoff  /uptime  /help
 *             (a tap keyboard is attached, so nothing needs typing)
 *
 *  FIRST RUN
 *    Copy secrets.example.h to secrets.h and fill in your WiFi and Telegram
 *    details. secrets.h is gitignored so credentials stay off GitHub.
 *
 *  REQUIRED LIBRARIES (Arduino IDE -> Library Manager)
 *    "Universal Telegram Bot" by Brian Lough
 *    "ArduinoJson" by Benoit Blanchon
 *    (WiFi, WebServer, WiFiClientSecure ship with the ESP32 board package)
 */

#include <WiFi.h>
#include <WebServer.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>

#include "secrets.h"   // copy secrets.example.h -> secrets.h and fill it in
#include "web_ui.h"

// ---------------- PIN MAP ----------------
const uint8_t PIN_REED_HIGH     = 33;  // 70% float - starts the cycle
const uint8_t PIN_REED_OVERFLOW = 25;  // 90% float - safety
const uint8_t PIN_SWITCH_MANUAL = 26;  // manual rocker - pump runs while closed
const uint8_t PIN_PUMP          = 23;  // -> relay module IN
const uint8_t PIN_LED_GREEN     = 19;  // motor-running indicator
const uint8_t PIN_LED_RED       = 18;  // overflow indicator
const uint8_t PIN_BUZZER        = 22;  // -> BC337 base via 1k
// White LED: wire directly to 3V3 through a resistor. No pin used.

// ---------------- CONFIG ----------------
const bool RELAY_ACTIVE_LOW = true;   // most blue 5V relay modules: IN=LOW energises. Flip if backwards.
const uint8_t PUMP_IDLE_LEVEL = RELAY_ACTIVE_LOW ? HIGH : LOW;

const unsigned long LEVEL_DEBOUNCE_MS = 1000;
const unsigned long RUN_DURATION_MS   = 6UL * 60UL * 1000UL;   // 6 minutes
const unsigned long MIN_OFF_MS        = 5UL * 1000UL;          // gap between auto-repeats
const unsigned long TELEGRAM_POLL_MS  = 2000;
const unsigned long ALERT_COOLDOWN_MS = 5UL * 60UL * 1000UL;   // don't spam Telegram
const unsigned long WIFI_RETRY_MS     = 20UL * 1000UL;         // reconnect attempt spacing
// RUN_DURATION_MS (6 min) is used for every timed pump run - auto cycle,
// web/Telegram manual, and the overflow guard. It's not a safety margin, it's
// the actual time to empty a full bucket. Only the manual switch ignores it.

// ---------------- GLOBALS ----------------
WebServer server(80);
WiFiClientSecure secured_client;
UniversalTelegramBot bot(BOT_TOKEN, secured_client);

struct DebouncedInput {
  uint8_t pin = 0;
  bool state = false;
  bool lastRaw = false;
  unsigned long lastChangeMs = 0;

  void begin(uint8_t p) {
    pin = p;
    pinMode(pin, INPUT_PULLUP);
    lastRaw = state = (digitalRead(pin) == LOW);
    lastChangeMs = millis();
  }
  // `now` is passed in so one millis() read serves the whole loop pass.
  void update(unsigned long now) {
    bool raw = (digitalRead(pin) == LOW);
    if (raw != lastRaw) { lastRaw = raw; lastChangeMs = now; }
    if (raw != state && (now - lastChangeMs) >= LEVEL_DEBOUNCE_MS) state = raw;
  }
};

DebouncedInput reedHigh, reedOverflow, switchManual;

enum State : uint8_t { ST_IDLE = 0, ST_RUNNING, ST_MANUAL, ST_MANUAL_SWITCH, ST_OVERFLOW };
State state = ST_IDLE;

bool pumpOn = false;
bool ledGreenOn = false, ledRedOn = false, buzzerOn = false;
bool wifiUp = false;
bool overflowMaxRunHit = false;   // 6 min of pumping didn't clear 90%
bool overflowAlerted   = false;   // first alert of this overflow already sent

unsigned long pumpStartMs   = 0;  // start of the current *timed* run
unsigned long pumpSinceMs   = 0;  // when the relay actually closed (runtime accounting)
unsigned long pumpTotalMs   = 0;  // lifetime pump runtime
unsigned long pumpStarts    = 0;  // lifetime pump starts
unsigned long lastStopMs    = 0;
unsigned long lastAlertMs   = 0;
unsigned long overflowStartMs = 0;
unsigned long lastTelegramPollMs = 0;
unsigned long lastWifiTryMs = 0;

char ipStr[16] = "0.0.0.0";       // cached: building it per request churned the heap

// ---------------- SMALL HELPERS ----------------
static inline const char* jbool(bool v) { return v ? "true" : "false"; }

// Writes the pin only when the value actually changes. The overflow branch used
// to re-issue the same digitalWrite thousands of times a second.
static inline void writeIfChanged(uint8_t pin, bool& cache, bool on) {
  if (cache == on) return;
  cache = on;
  digitalWrite(pin, on ? HIGH : LOW);
}

// "3h 12m" / "5m 20s" / "40s" - short enough for a tile or a Telegram column.
void fmtDur(char* out, size_t n, unsigned long secs) {
  unsigned long h = secs / 3600, m = (secs / 60) % 60, s = secs % 60;
  if (h)      snprintf(out, n, "%luh %lum", h, m);
  else if (m) snprintf(out, n, "%lum %lus", m, s);
  else        snprintf(out, n, "%lus", s);
}

const char* stateName() {
  switch (state) {
    case ST_IDLE:          return "Idle";
    case ST_RUNNING:       return "Auto cycle";
    case ST_MANUAL:        return "Manual run";
    case ST_MANUAL_SWITCH: return "Manual switch";
    case ST_OVERFLOW:      return "Overflow";
  }
  return "Unknown";
}

const char* stateGlyph() {
  switch (state) {
    case ST_IDLE:          return "\xE2\x9A\xAA";          // white circle
    case ST_RUNNING:       return "\xF0\x9F\x9F\xA2";      // green circle
    case ST_MANUAL:
    case ST_MANUAL_SWITCH: return "\xF0\x9F\x9F\xA1";      // yellow circle
    case ST_OVERFLOW:      return "\xF0\x9F\x94\xB4";      // red circle
  }
  return "\xE2\x9A\xAA";
}

// Seconds left in the current timed run, or -1 when nothing is timed.
long remainingSecs(unsigned long now) {
  unsigned long elapsed;
  if (state == ST_RUNNING || state == ST_MANUAL)            elapsed = now - pumpStartMs;
  else if (state == ST_OVERFLOW && !overflowMaxRunHit)      elapsed = now - overflowStartMs;
  else                                                      return -1;
  return elapsed >= RUN_DURATION_MS ? 0 : (long)((RUN_DURATION_MS - elapsed) / 1000);
}

// ---------------- PUMP / INDICATORS ----------------
void setPump(bool on) {
  if (pumpOn == on) return;              // idempotent: no relay chatter, no double-counted runtime
  const unsigned long now = millis();
  if (on) {
    pumpSinceMs = now;
    pumpStarts++;
  } else {
    pumpTotalMs += now - pumpSinceMs;
    lastStopMs = now;                    // single place that records a stop
  }
  pumpOn = on;
  digitalWrite(PIN_PUMP, (RELAY_ACTIVE_LOW != on) ? HIGH : LOW);
  writeIfChanged(PIN_LED_GREEN, ledGreenOn, on);
}

// Both control paths ask first, so the web UI, Telegram and the state machine
// can never disagree about whether an override is allowed.
const char* startBlockedReason() {
  if (state == ST_OVERFLOW)      return "The 90% overflow float is wet.";
  if (state == ST_MANUAL_SWITCH) return "The manual rocker switch is already holding the pump on.";
  return nullptr;
}

const char* stopBlockedReason() {
  if (state == ST_OVERFLOW)      return "The overflow safety handler owns the pump right now.";
  if (state == ST_MANUAL_SWITCH) return "The manual rocker switch is closed - flip it off to stop.";
  return nullptr;
}

void startManualRun() {
  pumpStartMs = millis();
  state = ST_MANUAL;
  setPump(true);
}

// ---------------- TELEGRAM ----------------
/*
 * Replies use HTML parse mode: a glyph + bold headline for the state, and a
 * <pre> block for anything tabular - monospace is the only way to get columns
 * that line up in both the phone and desktop clients. One glyph per message,
 * no decoration beyond that.
 */
const char* KEYBOARD_JSON = "[[\"/status\"],[\"/pumpon\",\"/pumpoff\"],[\"/uptime\"]]";

void telegramSend(const char* html) {
  if (wifiUp) bot.sendMessage(CHAT_ID, html, "HTML");
}

void telegramReply(const String& chat_id, const char* html) {
  bot.sendMessage(chat_id, html, "HTML");
}

void telegramReplyWithMenu(const String& chat_id, const char* html) {
  bot.sendMessageWithReplyKeyboard(chat_id, html, "HTML", KEYBOARD_JSON, true);
}

void buildStatusCard(char* out, size_t n) {
  const unsigned long now = millis();

  const char* level = reedOverflow.state ? "90% OVERFLOW"
                    : reedHigh.state     ? "70% draining"
                                         : "below 70%";
  char run[28];
  const long rem = remainingSecs(now);
  if (rem >= 0) {
    char t[16];
    fmtDur(t, sizeof t, (unsigned long)rem);
    snprintf(run, sizeof run, "%s left", t);
  } else if (pumpOn) {
    char t[16];
    fmtDur(t, sizeof t, (now - pumpSinceMs) / 1000);
    snprintf(run, sizeof run, "%s, no cap", t);
  } else {
    snprintf(run, sizeof run, "-");
  }

  char up[16], total[16];
  fmtDur(up, sizeof up, now / 1000);
  fmtDur(total, sizeof total, (pumpTotalMs + (pumpOn ? now - pumpSinceMs : 0)) / 1000);

  snprintf(out, n,
    "%s <b>%s</b>\n"
    "<pre>Pump     %s\n"
    "Level    %s\n"
    "Switch   %s\n"
    "Run      %s\n"
    "Total    %s\n"
    "Uptime   %s</pre>",
    stateGlyph(), stateName(),
    pumpOn ? "on" : "off",
    level,
    switchManual.state ? "closed" : "open",
    run, total, up);
}

void handleTelegramMessages(int numNewMessages) {
  for (int i = 0; i < numNewMessages; i++) {
    const String chat_id = bot.messages[i].chat_id;
    if (chat_id != CHAT_ID) continue;          // ignore everyone else
    String text = bot.messages[i].text;
    text.trim();
    text.toLowerCase();

    if (text == "/status") {
      char card[320];
      buildStatusCard(card, sizeof card);
      telegramReply(chat_id, card);

    } else if (text == "/pumpon") {
      const char* why = startBlockedReason();
      if (why) {
        char msg[160];
        snprintf(msg, sizeof msg, "\xE2\x9A\xA0\xEF\xB8\x8F <b>Refused</b>\n%s", why);
        telegramReply(chat_id, msg);
      } else {
        startManualRun();
        telegramReply(chat_id, "\xF0\x9F\x9F\xA1 <b>Pump started</b>\nManual run, stops after 6 min.");
      }

    } else if (text == "/pumpoff") {
      const char* why = stopBlockedReason();
      if (why) {
        char msg[160];
        snprintf(msg, sizeof msg, "\xE2\x9A\xA0\xEF\xB8\x8F <b>Refused</b>\n%s", why);
        telegramReply(chat_id, msg);
      } else {
        setPump(false);
        state = ST_IDLE;
        telegramReply(chat_id, "\xE2\x9A\xAA <b>Pump stopped</b>\nBack on the automatic cycle.");
      }

    } else if (text == "/uptime") {
      char up[16], msg[96];
      fmtDur(up, sizeof up, millis() / 1000);
      snprintf(msg, sizeof msg,
               "%s <b>Uptime</b>\n<pre>%s on %s</pre>", stateGlyph(), up, ipStr);
      telegramReply(chat_id, msg);

    } else {
      char msg[260];
      snprintf(msg, sizeof msg,
        "\xF0\x9F\x92\xA7 <b>AC Drain</b>\n"
        "<pre>/status   level, pump, uptime\n"
        "/pumpon   6 min manual run\n"
        "/pumpoff  stop the pump\n"
        "/uptime   how long since boot</pre>\n"
        "Dashboard: http://%s", ipStr);
      telegramReplyWithMenu(chat_id, msg);
    }
  }
}

// ---------------- WEB SERVER ----------------
void handleRoot() {
  server.send_P(200, "text/html", INDEX_HTML);
}

void handleStatus() {
  const unsigned long now = millis();
  const unsigned long totalMs = pumpTotalMs + (pumpOn ? now - pumpSinceMs : 0);

  // Fixed buffer + snprintf instead of String concatenation: the dashboard
  // polls this every 2s forever, and String churn is what fragments the heap.
  char buf[320];
  snprintf(buf, sizeof buf,
    "{\"state\":%u,\"pump\":%s,\"reed70\":%s,\"reed90\":%s,\"manual\":%s,"
    "\"elapsed\":%lu,\"remaining\":%ld,\"duration\":%lu,\"blocked\":%s,"
    "\"starts\":%lu,\"pumpTotal\":%lu,\"uptime\":%lu,\"rssi\":%d,\"ip\":\"%s\"}",
    (unsigned)state, jbool(pumpOn), jbool(reedHigh.state), jbool(reedOverflow.state),
    jbool(switchManual.state),
    pumpOn ? (now - pumpSinceMs) / 1000 : 0UL,
    remainingSecs(now),
    RUN_DURATION_MS / 1000UL,
    jbool(overflowMaxRunHit),
    pumpStarts, totalMs / 1000UL, now / 1000UL,
    wifiUp ? WiFi.RSSI() : 0,
    ipStr);

  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", buf);
}

void handlePumpOn() {
  const char* why = startBlockedReason();
  if (why) { server.send(409, "text/plain", why); return; }
  startManualRun();
  server.send(200, "text/plain", "Pump started - 6 minute cap");
}

void handlePumpOff() {
  const char* why = stopBlockedReason();
  if (why) { server.send(409, "text/plain", why); return; }
  setPump(false);
  state = ST_IDLE;
  server.send(200, "text/plain", "Pump stopped");
}

// ---------------- CONTROL LOGIC ----------------
void handleOverflow(unsigned long now) {
  if (!reedOverflow.state) {
    if (state == ST_OVERFLOW) {
      // Leaving overflow MUST stop the pump. Previously the state was reset to
      // idle with the relay still closed, so if the 70% float was dry too the
      // pump had nothing left to switch it off and ran indefinitely.
      setPump(false);
      state = ST_IDLE;
      overflowAlerted = false;
      Serial.println("[OVERFLOW] cleared - resuming normal operation");
    }
    writeIfChanged(PIN_LED_RED, ledRedOn, false);
    writeIfChanged(PIN_BUZZER, buzzerOn, false);
    return;
  }

  if (state != ST_OVERFLOW) {
    state = ST_OVERFLOW;
    overflowStartMs = now;
    overflowMaxRunHit = false;
    Serial.println("[OVERFLOW] 90% reached - forcing pump ON");
  }

  writeIfChanged(PIN_LED_RED, ledRedOn, true);
  writeIfChanged(PIN_BUZZER, buzzerOn, true);

  if (!overflowMaxRunHit) {
    setPump(true);
    if (now - overflowStartMs >= RUN_DURATION_MS) {
      overflowMaxRunHit = true;
      setPump(false);
      Serial.println("[OVERFLOW] full run done, still wet - pump stopped, needs a look");
      telegramSend("\xF0\x9F\x94\xB4 <b>Not draining</b>\n"
                   "The pump ran a full 6-minute cycle and the water is still at 90%. "
                   "Stopped to protect it - check for a blockage now.");
    }
  }

  // First alert fires immediately; the cooldown only throttles the repeats.
  // (The old `millis() - lastAlertMs > COOLDOWN` test silently swallowed any
  // alert during the first 5 minutes of uptime, when lastAlertMs was still 0.)
  if (!overflowAlerted || now - lastAlertMs >= ALERT_COOLDOWN_MS) {
    overflowAlerted = true;
    lastAlertMs = now;
    telegramSend("\xF0\x9F\x94\xB4 <b>Overflow - 90%</b>\n"
                 "Pump is running to clear it. Check the drain line if this doesn't resolve.");
  }
}

void handleManualSwitch() {
  if (switchManual.state) {
    if (state != ST_MANUAL_SWITCH) {
      state = ST_MANUAL_SWITCH;
      setPump(true);
      Serial.println("[MANUAL SWITCH] pump ON - runs until the switch opens");
    }
  } else if (state == ST_MANUAL_SWITCH) {
    setPump(false);
    state = ST_IDLE;
    Serial.println("[MANUAL SWITCH] pump OFF - switch opened");
  }
}

void handleAutoCycle(unsigned long now) {
  switch (state) {
    case ST_IDLE:
      if (reedHigh.state && (now - lastStopMs) >= MIN_OFF_MS) {
        pumpStartMs = now;
        state = ST_RUNNING;
        setPump(true);
        Serial.println("[CYCLE] 70% reached - pump ON for 6 min");
      }
      break;

    case ST_RUNNING:
    case ST_MANUAL:
      if (now - pumpStartMs >= RUN_DURATION_MS) {
        const bool wasAuto = (state == ST_RUNNING);
        setPump(false);
        state = ST_IDLE;
        // Still wet? ST_IDLE above starts a fresh cycle once MIN_OFF_MS has
        // passed - that is the "repeat automatically" behaviour.
        Serial.println(wasAuto ? "[CYCLE] 6 min elapsed - pump OFF"
                               : "[MANUAL] 6 min cap reached - pump OFF");
      }
      break;

    default:
      break;
  }
}

// ---------------- NETWORK ----------------
void cacheIp() {
  const String ip = WiFi.localIP().toString();
  strncpy(ipStr, ip.c_str(), sizeof(ipStr) - 1);
  ipStr[sizeof(ipStr) - 1] = '\0';
}

void serviceWifi(unsigned long now) {
  const bool up = (WiFi.status() == WL_CONNECTED);
  if (up != wifiUp) {
    wifiUp = up;
    if (up) {
      cacheIp();
      Serial.printf("[WIFI] connected - http://%s\n", ipStr);
    } else {
      Serial.println("[WIFI] lost - retrying, pump logic keeps running locally");
    }
  }
  // The original never reconnected: one router reboot and the dashboard and
  // every Telegram alert were gone until someone power-cycled the board.
  if (!up && (now - lastWifiTryMs) >= WIFI_RETRY_MS) {
    lastWifiTryMs = now;
    WiFi.reconnect();
  }
}

void serviceTelegram() {
  if (!wifiUp || (millis() - lastTelegramPollMs) < TELEGRAM_POLL_MS) return;

  // One batch per loop pass, deliberately. The old `while (numNewMessages)`
  // drain could hold the loop inside blocking TLS calls indefinitely, which
  // starved both server.handleClient() and the overflow check.
  const int n = bot.getUpdates(bot.last_message_received + 1);
  if (n > 0) handleTelegramMessages(n);

  lastTelegramPollMs = millis();   // measured from the end of the poll, so a
                                   // slow TLS handshake can't queue back-to-back
}

// ---------------- SETUP ----------------
void setup() {
  // Relay first, and set the idle level BEFORE the pin becomes an output: on an
  // active-low module, pinMode(OUTPUT) alone drives IN low and kicks the pump.
  digitalWrite(PIN_PUMP, PUMP_IDLE_LEVEL);
  pinMode(PIN_PUMP, OUTPUT);
  digitalWrite(PIN_PUMP, PUMP_IDLE_LEVEL);

  Serial.begin(115200);
  delay(200);
  Serial.println("\n[BOOT] AC drain controller");

  pinMode(PIN_LED_GREEN, OUTPUT); digitalWrite(PIN_LED_GREEN, LOW);
  pinMode(PIN_LED_RED, OUTPUT);   digitalWrite(PIN_LED_RED, LOW);
  pinMode(PIN_BUZZER, OUTPUT);    digitalWrite(PIN_BUZZER, LOW);

  reedHigh.begin(PIN_REED_HIGH);
  reedOverflow.begin(PIN_REED_OVERFLOW);
  switchManual.begin(PIN_SWITCH_MANUAL);

  secured_client.setInsecure();   // Telegram cert not pinned - keep it simple.
                                  // Set unconditionally so a late WiFi join works.

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);           // mains powered; keeps the dashboard snappy
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);         // stop rewriting credentials to flash on every boot
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("[WIFI] connecting");
  const unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 20000) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    wifiUp = true;
    cacheIp();
    Serial.printf("[WIFI] connected - http://%s\n", ipStr);
    char msg[160];
    snprintf(msg, sizeof msg,
             "\xE2\x9A\xAA <b>Controller online</b>\nDashboard: http://%s", ipStr);
    telegramReplyWithMenu(CHAT_ID, msg);   // also installs the tap keyboard
  } else {
    Serial.println("[WIFI] failed - continuing offline, reed/pump logic still works");
  }

  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/pump/on", HTTP_POST, handlePumpOn);
  server.on("/pump/off", HTTP_POST, handlePumpOff);
  server.onNotFound([]() { server.send(404, "text/plain", "not found"); });
  server.begin();
  Serial.println("[HTTP] dashboard up on port 80");
}

// ---------------- MAIN LOOP ----------------
void loop() {
  const unsigned long now = millis();   // one read, one consistent view of time

  reedHigh.update(now);
  reedOverflow.update(now);
  switchManual.update(now);

  server.handleClient();

  // Safety first, and always before anything that can block.
  handleOverflow(now);
  if (state != ST_OVERFLOW) {
    handleManualSwitch();
    handleAutoCycle(now);
  }

  serviceWifi(now);
  serviceTelegram();

  delay(1);   // yields to the WiFi/idle tasks instead of spinning a core flat out
}
