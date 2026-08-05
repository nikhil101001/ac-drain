/*
 * ============================================================
 *  AC CONDENSATE AUTO-DRAIN CONTROLLER   (ESP32)
 *  Timed pump cycle + web dashboard + Telegram control
 * ============================================================
 *
 *  BEHAVIOUR
 *    Reed HIGH closes (water at 70%)
 *        -> pump runs for 4 min, then stops. If the float is still wet, a fresh
 *           4 min cycle starts after a short gap (repeats).
 *    Reed OVERFLOW closes (water at 90%)
 *        -> pump forced ON to clear it, red LED + buzzer, Telegram alert. The
 *           overflow run gets a LONGER cap than the normal cycle - 5 min - on
 *           the grounds that there is more water to shift. If that full run
 *           does not drop the level, the pump is stopped (it plainly isn't
 *           draining) and a "check for a blockage" alert goes out. Better a wet
 *           tray than a burnt-out pump.
 *    Manual switch  -  an ENABLE switch, not a run switch
 *        -> OFF stops the pump at once and suspends automatic operation. The
 *           run timer is FROZEN, not reset: switch back on with a float still
 *           wet and the pump finishes the remainder of the 4 min rather than
 *           starting the cycle over. A 90% overflow ignores the switch
 *           entirely - water damage outranks it.
 *
 *  PRIORITY   overflow > manual switch OFF > web/Telegram manual > auto cycle
 *
 *  LEDS
 *    White  - wired DIRECTLY to 3V3 through a resistor, no GPIO. Lit whenever
 *             the board has power; it can't lie by staying on after a crash.
 *    Green  - GPIO, lit only while the pump relay is energised.
 *    Red    - GPIO, lit while the 90% overflow float is wet.
 *
 *  WEB   (all routes behind digest auth once WEB_PASSWORD is set in secrets.h)
 *    GET  /              dashboard
 *    GET  /api/status    JSON status
 *    GET  /api/log       event log, ?since=<seq> for incremental fetch
 *    GET  /api/log.csv   same log as a CSV download
 *    POST /pump/on       manual run, capped at the normal cycle length
 *    POST /pump/off      stop immediately
 *    POST /api/config    set the run caps - ?run=<sec>&overflow=<sec>, ?reset=1
 *
 *  RUN CAPS
 *    Both are set from the dashboard and kept in NVS, so retiming the pump
 *    needs no reflash and survives a power cut. 4 min / 5 min are only the
 *    defaults a fresh board starts from.
 *
 *  TELEGRAM   /status  /log  /pumpon  /pumpoff  /uptime  /help
 *             (a tap keyboard is attached, so nothing needs typing)
 *
 *  OTA
 *    Password-protected over-the-air updates, so the controller can be
 *    reflashed without unplugging it - "./flash main --ota". See the OTA
 *    section below for why the pump is parked first and why an update is
 *    refused mid-overflow.
 *
 *  REMOTE ACCESS
 *    Telegram already works from anywhere. For the dashboard, put a VPN or a
 *    tunnel in front of it - see the README. Do not port-forward this to the
 *    internet: it is plain HTTP, and on the other end of it is a mains relay.
 *
 *  FIRST RUN
 *    Copy secrets.example.h to secrets.h and fill in your WiFi, Telegram and
 *    OTA details. secrets.h is gitignored so credentials stay off GitHub.
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
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <time.h>

// Credentials live outside version control. Accepted either next to this
// sketch or one level up at the repo root, so moving the file doesn't break
// the build.
#if __has_include("secrets.h")
  #include "secrets.h"
#elif __has_include("../secrets.h")
  #include "../secrets.h"
#else
  #error "No secrets.h found. Copy secrets.example.h to secrets.h and fill it in."
#endif

// An OTA port with no password hands the pump, the relay and the WiFi
// credentials to anyone on the network, so the build stops rather than quietly
// opening one. Two lines in secrets.h fixes it - see secrets.example.h.
#ifndef OTA_PASSWORD
  #error "No OTA_PASSWORD in secrets.h. Add one (see secrets.example.h) - OTA is not offered unauthenticated."
#endif
#ifndef OTA_HOSTNAME
  #define OTA_HOSTNAME "ac-drain"
#endif

#include "web_ui.h"

// ---------------- PIN MAP ----------------
const uint8_t PIN_REED_HIGH     = 33;  // 70% float - starts the cycle
const uint8_t PIN_REED_OVERFLOW = 25;  // 90% float - safety
const uint8_t PIN_SWITCH_MANUAL = 26;  // manual enable switch - OFF inhibits the pump
const uint8_t PIN_PUMP          = 23;  // -> relay module IN
const uint8_t PIN_LED_GREEN     = 19;  // motor-running indicator
const uint8_t PIN_LED_RED       = 18;  // overflow indicator
const uint8_t PIN_BUZZER        = 22;  // -> BC337 base via 1k
// White LED: wire directly to 3V3 through a resistor. No pin used.

// ---------------- CONFIG ----------------
// This build energises the relay on IN=HIGH, so the idle level is LOW. Getting
// this backwards is not cosmetic: PUMP_IDLE_LEVEL is what setup() parks the pin
// at before it becomes an output, so an inverted value runs the pump from boot
// until the first float reading - continuously, ignoring every stop command.
// The 10k pull-down on IN covers the same window before setup() gets to run.
const bool RELAY_ACTIVE_LOW = false;
const uint8_t PUMP_IDLE_LEVEL = RELAY_ACTIVE_LOW ? HIGH : LOW;

// Which contact position means "automatic operation allowed". Default: closed
// (pin pulled to GND) is enabled, so a broken switch wire lands on the
// inhibited side - where the 90% handler still protects the tray and still
// raises an alert, rather than failing silently with the pump disabled.
const bool SWITCH_CLOSED_IS_ENABLED = true;

// Sound the buzzer briefly at boot. An alarm that is only ever exercised by a
// real 90% overflow is one nobody finds out is dead until the moment it
// matters, and it makes "is the buzzer broken?" answerable in two seconds
// instead of requiring a wet float. Set false if the beep becomes annoying.
const bool BUZZER_BOOT_TEST    = true;
const unsigned long BUZZER_BOOT_TEST_MS = 250;

const unsigned long LEVEL_DEBOUNCE_MS = 1000;
const unsigned long MIN_OFF_MS        = 5UL * 1000UL;          // gap between auto-repeats
const unsigned long TELEGRAM_POLL_MS  = 2000;
const unsigned long ALERT_COOLDOWN_MS = 5UL * 60UL * 1000UL;   // don't spam Telegram
const unsigned long WIFI_RETRY_MS     = 20UL * 1000UL;         // reconnect attempt spacing

// ---------------- RUN CAPS ----------------
/*
 * Two caps, because the two situations are not the same job.
 *
 * The RUN cap covers every ordinary timed run - the auto cycle and any
 * web/Telegram manual run. MIN_OFF_MS then lets a still-wet float start the
 * next one, so a tray that needs longer gets more cycles rather than one long
 * run; the cap is what stops the pump running dry against an empty tray.
 *
 * The OVERFLOW cap is longer only because it is the deadline on a single
 * uninterrupted run: at 90% there is more water to shift, and stopping at the
 * normal cap would declare a blockage that isn't one. Passing it is the signal
 * that the pump has had a fair go and still isn't draining.
 *
 * Both are settable from the dashboard, because the right numbers are a
 * property of the bucket and the pump - not of the firmware - and finding them
 * takes a few tries with a stopwatch. The values below are only the defaults a
 * freshly flashed board starts from.
 */
const unsigned long RUN_DEFAULT_MS      = 4UL * 60UL * 1000UL;   // 4 min - normal cycle
const unsigned long OVERFLOW_DEFAULT_MS = 5UL * 60UL * 1000UL;   // 5 min - overflow guard

// Bounds on what the dashboard is allowed to set. The ceiling is the one that
// matters: the cap is the only thing that stops a pump running dry against a
// tray that has already emptied, so "no limit" is not on the menu. The floor
// just keeps a typo from turning the cycle into relay chatter.
const unsigned long RUN_MIN_S = 10;
const unsigned long RUN_MAX_S = 15UL * 60UL;

// The live values. Loaded from NVS at boot, changed by POST /api/config.
unsigned long runDurationMs = RUN_DEFAULT_MS;
unsigned long overflowRunMs = OVERFLOW_DEFAULT_MS;

// ---------------- GLOBALS ----------------
WebServer server(80);
WiFiClientSecure secured_client;
UniversalTelegramBot bot(BOT_TOKEN, secured_client);

/*
 * NVS, for the two run caps and nothing else.
 *
 * This is the one thing on the board that earns a flash write, and it is worth
 * being explicit about why - the event log a few sections down deliberately
 * refuses to persist for the opposite reason. The log would write every few
 * minutes, forever. These write only when a human moves a slider, which over
 * the life of the controller is a handful of times. A setting that silently
 * reverted to the compile-time default after a power cut would be far worse
 * than the wear: the pump would quietly go back to the wrong timing and the
 * dashboard would agree with it.
 */
Preferences prefs;

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

// Index order is part of the dashboard's contract: web_ui.h indexes NAME, SUB
// and LOCK by this value. Slot 3 is the inhibited state, not a running one.
enum State : uint8_t { ST_IDLE = 0, ST_RUNNING, ST_MANUAL, ST_SWITCH_OFF, ST_OVERFLOW };
State state = ST_IDLE;

bool pumpOn = false;
bool ledGreenOn = false, ledRedOn = false, buzzerOn = false;
bool wifiUp = false;
bool otaReady = false;            // OTA port is listening
bool overflowMaxRunHit = false;   // a full run didn't clear 90%
bool overflowAlerted   = false;   // first alert of this overflow already sent

// The two run caps rendered once at boot ("4m", "5m"). Every message, reply and
// dashboard label quotes these instead of a literal, so retiming a run cannot
// leave the UI confidently stating a number the pump no longer uses.
char runDurStr[12] = "";
char ovfDurStr[12] = "";

// Enable-switch state, and the timed run it interrupted.
//
// Freezing is the whole point of holding these: when the switch goes off
// mid-cycle we keep how far the run had got, so flipping it back on finishes
// the remainder instead of restarting a full run that was nearly done.
// heldState is ST_IDLE when there is nothing to resume.
bool switchEnabled  = true;       // debounced + polarity-corrected
State heldState     = ST_IDLE;    // ST_RUNNING or ST_MANUAL if a run was frozen
unsigned long heldElapsedMs = 0;  // how far that run had already got

unsigned long pumpStartMs   = 0;  // start of the current *timed* run
unsigned long pumpSinceMs   = 0;  // when the relay actually closed (runtime accounting)
unsigned long pumpTotalMs   = 0;  // lifetime pump runtime
unsigned long pumpStarts    = 0;  // lifetime pump starts
unsigned long lastStopMs    = 0;
unsigned long lastAlertMs   = 0;
unsigned long overflowStartMs = 0;
unsigned long overflowCount = 0;  // lifetime 90% events
unsigned long blockedCount  = 0;  // lifetime "ran a full cycle and still wet" events
unsigned long lastTelegramPollMs = 0;
unsigned long lastWifiTryMs = 0;

char ipStr[16] = "0.0.0.0";       // cached: building it per request churned the heap

// ---------------- EVENT LOG ----------------
/*
 * A fixed-size ring in RAM. Deliberately NOT in flash: this device switches a
 * relay every few minutes, and writing each event to NVS/SPIFFS would burn
 * flash endurance for no benefit. 256 entries x 8 bytes = 2 KB, allocated once
 * at compile time - it cannot grow and cannot fragment the heap.
 *
 * The device is the single source of truth, so every browser that opens the
 * dashboard sees the same history. The cost of staying out of flash is that the
 * log starts empty after a power cycle; /api/log.csv exists to archive it
 * off-device before that happens.
 */
// These numbers go out on the wire and the dashboard's EV table is indexed by
// them, so new codes are appended - never inserted.
enum EvCode : uint8_t {
  EV_BOOT = 0, EV_PUMP_ON, EV_PUMP_OFF, EV_OVERFLOW_ON, EV_OVERFLOW_OFF,
  EV_BLOCKED, EV_WIFI_DOWN, EV_WIFI_UP, EV_SWITCH_OFF, EV_SWITCH_ON, EV_OTA,
  EV_CONFIG
};
enum EvCause : uint8_t {
  CAUSE_NONE = 0, CAUSE_AUTO, CAUSE_MANUAL, CAUSE_SWITCH, CAUSE_OVERFLOW, CAUSE_OTA
};

struct LogEvent {
  uint32_t sec;      // seconds since boot
  uint16_t detail;   // run/overflow duration in seconds, else 0
  uint8_t  code;
  uint8_t  cause;
};

const uint16_t LOG_CAPACITY = 256;   // power of two, so the index is a mask not a modulo
LogEvent logBuf[LOG_CAPACITY];
uint32_t logSeq = 0;                 // total events ever recorded; also the next slot

// Wall-clock epoch of boot, from NTP. 0 until sync, and the dashboard falls
// back to uptime-relative labels in that case. Deliberately UTC with no
// timezone applied - the browser localises it, so there is no offset to get
// wrong on the device.
uint32_t bootEpoch = 0;

void logAdd(uint8_t code, uint8_t cause = CAUSE_NONE, uint16_t detail = 0) {
  LogEvent& e = logBuf[logSeq & (LOG_CAPACITY - 1)];
  e.sec    = millis() / 1000;
  e.detail = detail;
  e.code   = code;
  e.cause  = cause;
  logSeq++;
}

// Oldest sequence number still held in the ring. Anything below this has been
// overwritten, so a client asking for it gets resynced instead of a silent gap.
uint32_t logOldest() {
  return (logSeq > LOG_CAPACITY) ? logSeq - LOG_CAPACITY : 0;
}

const char* evName(uint8_t code) {
  switch (code) {
    case EV_BOOT:         return "boot";
    case EV_PUMP_ON:      return "pump_on";
    case EV_PUMP_OFF:     return "pump_off";
    case EV_OVERFLOW_ON:  return "overflow_on";
    case EV_OVERFLOW_OFF: return "overflow_off";
    case EV_BLOCKED:      return "blocked";
    case EV_WIFI_DOWN:    return "wifi_down";
    case EV_WIFI_UP:      return "wifi_up";
    case EV_SWITCH_OFF:   return "switch_off";
    case EV_SWITCH_ON:    return "switch_on";
    case EV_OTA:          return "ota";
    case EV_CONFIG:       return "config";
  }
  return "?";
}

const char* causeName(uint8_t cause) {
  switch (cause) {
    case CAUSE_AUTO:     return "auto";
    case CAUSE_MANUAL:   return "manual";
    case CAUSE_SWITCH:   return "switch";
    case CAUSE_OVERFLOW: return "overflow";
    case CAUSE_OTA:      return "ota";
  }
  return "";
}

// ---------------- SMALL HELPERS ----------------
static inline const char* jbool(bool v) { return v ? "true" : "false"; }

/*
 * How long ago `start` was, from the loop's cached `now`.
 *
 * Not just `now - start`: web and Telegram handlers stamp millis() themselves,
 * and server.handleClient() runs *after* loop() samples `now`. A run started
 * from the dashboard therefore carries a timestamp a millisecond or two in the
 * future, the plain subtraction underflows to ~49 days, and every "has it been
 * long enough?" test fires instantly - which killed manual runs in the same
 * pass that started them. Anything more than half the millis() range in the
 * past is really a timestamp from the future, so report it as no time at all.
 */
static inline unsigned long elapsedSince(unsigned long now, unsigned long start) {
  const unsigned long d = now - start;
  return (d > (~0UL / 2)) ? 0UL : d;
}

// Writes the pin only when the value actually changes. The overflow branch used
// to re-issue the same digitalWrite thousands of times a second.
static inline void writeIfChanged(uint8_t pin, bool& cache, bool on) {
  if (cache == on) return;
  cache = on;
  digitalWrite(pin, on ? HIGH : LOW);
}

// ---------------- RUN CAP STORAGE ----------------
// Forward-declared: applyRunCaps() re-renders these, and they are defined with
// the rest of the small helpers below.
void fmtRunLen(char* out, size_t n, unsigned long secs);

static inline bool runCapInRange(unsigned long secs) {
  return secs >= RUN_MIN_S && secs <= RUN_MAX_S;
}

// Single place that moves the live caps, so the strings every message quotes
// can never drift out of step with the numbers the pump actually uses.
void applyRunCaps(unsigned long runS, unsigned long ovfS) {
  runDurationMs = runS * 1000UL;
  overflowRunMs = ovfS * 1000UL;
  fmtRunLen(runDurStr, sizeof runDurStr, runS);
  fmtRunLen(ovfDurStr, sizeof ovfDurStr, ovfS);
}

/*
 * Load at boot, and be suspicious of what comes back.
 *
 * NVS is the one thing here that survives a reflash, so a stored value can
 * outlive the firmware that wrote it - including a build whose bounds were
 * different, or a half-finished write. Anything outside the current range is
 * discarded in favour of the default rather than trusted, because the failure
 * mode of an absurd cap is a pump running dry for as long as it says.
 */
void loadRunCaps() {
  prefs.begin("acdrain", false);
  unsigned long runS = prefs.getULong("run", RUN_DEFAULT_MS / 1000UL);
  unsigned long ovfS = prefs.getULong("ovf", OVERFLOW_DEFAULT_MS / 1000UL);

  if (!runCapInRange(runS)) {
    Serial.printf("[CONFIG] stored run cap %lus out of range - using the default\n", runS);
    runS = RUN_DEFAULT_MS / 1000UL;
  }
  if (!runCapInRange(ovfS)) {
    Serial.printf("[CONFIG] stored overflow cap %lus out of range - using the default\n", ovfS);
    ovfS = OVERFLOW_DEFAULT_MS / 1000UL;
  }
  applyRunCaps(runS, ovfS);
}

// Writes only when a value actually changed - Preferences::putULong is a flash
// write, and the dashboard's Save button is perfectly capable of sending the
// same numbers back at you all afternoon.
void storeRunCaps(unsigned long runS, unsigned long ovfS) {
  if (prefs.getULong("run", 0) != runS) prefs.putULong("run", runS);
  if (prefs.getULong("ovf", 0) != ovfS) prefs.putULong("ovf", ovfS);
}

// "3h 12m" / "5m 20s" / "40s" - short enough for a tile or a Telegram column.
void fmtDur(char* out, size_t n, unsigned long secs) {
  unsigned long h = secs / 3600, m = (secs / 60) % 60, s = secs % 60;
  if (h)      snprintf(out, n, "%luh %lum", h, m);
  else if (m) snprintf(out, n, "%lum %lus", m, s);
  else        snprintf(out, n, "%lus", s);
}

// Same idea, but for quoting a configured run length rather than a measured
// one: a whole number of minutes reads "4m", not the "4m 0s" that fmtDur would
// give. Measured durations keep the trailing seconds - they are real.
void fmtRunLen(char* out, size_t n, unsigned long secs) {
  const unsigned long m = secs / 60, s = secs % 60;
  if (m && s) snprintf(out, n, "%lum %lus", m, s);
  else if (m) snprintf(out, n, "%lum", m);
  else        snprintf(out, n, "%lus", s);
}

const char* stateName() {
  switch (state) {
    case ST_IDLE:       return "Idle";
    case ST_RUNNING:    return "Auto cycle";
    case ST_MANUAL:     return "Manual run";
    case ST_SWITCH_OFF: return "Switched off";
    case ST_OVERFLOW:   return "Overflow";
  }
  return "Unknown";
}

const char* stateGlyph() {
  switch (state) {
    case ST_IDLE:       return "\xE2\x9A\xAA";          // white circle
    case ST_RUNNING:    return "\xF0\x9F\x9F\xA2";      // green circle
    case ST_MANUAL:
    case ST_SWITCH_OFF: return "\xF0\x9F\x9F\xA1";      // yellow circle
    case ST_OVERFLOW:   return "\xF0\x9F\x94\xB4";      // red circle
  }
  return "\xE2\x9A\xAA";
}

// The cap that applies to whatever run is in progress. Only an overflow gets
// the longer one - a run frozen by the switch is an ordinary cycle held
// mid-flight, so it finishes against the ordinary cap.
static inline unsigned long currentCapMs() {
  return (state == ST_OVERFLOW) ? overflowRunMs : runDurationMs;
}

// Seconds left in the current timed run, or -1 when nothing is timed.
//
// A run frozen by the enable switch still reports its remainder: that is what
// lets the dashboard show "paused, 3m 40s left on resume" instead of a bare
// stopped state, so a held cycle can't be mistaken for a cancelled one.
long remainingSecs(unsigned long now) {
  unsigned long elapsed;
  if (state == ST_RUNNING || state == ST_MANUAL)            elapsed = elapsedSince(now, pumpStartMs);
  else if (state == ST_OVERFLOW && !overflowMaxRunHit)      elapsed = elapsedSince(now, overflowStartMs);
  else if (state == ST_SWITCH_OFF && heldState != ST_IDLE)  elapsed = heldElapsedMs;
  else                                                      return -1;
  const unsigned long cap = currentCapMs();
  return elapsed >= cap ? 0 : (long)((cap - elapsed) / 1000);
}

// ---------------- PUMP / INDICATORS ----------------
// `cause` is only used for the log entry. Because setPump() is idempotent, the
// log gets exactly one entry per real relay transition - no duplicates from the
// overflow branch calling setPump(true) on every pass.
void setPump(bool on, uint8_t cause = CAUSE_NONE) {
  if (pumpOn == on) return;              // idempotent: no relay chatter, no double-counted runtime
  const unsigned long now = millis();
  if (on) {
    pumpSinceMs = now;
    pumpStarts++;
    logAdd(EV_PUMP_ON, cause);
  } else {
    const unsigned long runS = (now - pumpSinceMs) / 1000;
    pumpTotalMs += now - pumpSinceMs;
    lastStopMs = now;                    // single place that records a stop
    logAdd(EV_PUMP_OFF, cause, runS > 65535 ? 65535 : (uint16_t)runS);
  }
  pumpOn = on;
  digitalWrite(PIN_PUMP, (RELAY_ACTIVE_LOW != on) ? HIGH : LOW);
  writeIfChanged(PIN_LED_GREEN, ledGreenOn, on);
}

// Both control paths ask first, so the web UI, Telegram and the state machine
// can never disagree about whether an override is allowed.
const char* startBlockedReason() {
  if (state == ST_OVERFLOW)   return "The 90% overflow float is wet.";
  if (state == ST_SWITCH_OFF) return "The manual switch is in the OFF position - turn it on first.";
  return nullptr;
}

const char* stopBlockedReason() {
  if (state == ST_OVERFLOW)   return "The overflow safety handler owns the pump right now.";
  if (state == ST_SWITCH_OFF) return "The manual switch is OFF - the pump is already stopped.";
  return nullptr;
}

void startManualRun() {
  pumpStartMs = millis();
  state = ST_MANUAL;
  setPump(true, CAUSE_MANUAL);
}

// ---------------- TELEGRAM ----------------
/*
 * Replies use HTML parse mode: a glyph + bold headline for the state, and a
 * <pre> block for anything tabular - monospace is the only way to get columns
 * that line up in both the phone and desktop clients. One glyph per message,
 * no decoration beyond that.
 */
const char* KEYBOARD_JSON = "[[\"/status\",\"/log\"],[\"/pumpon\",\"/pumpoff\"]]";

void telegramSend(const char* html) {
  if (wifiUp) bot.sendMessage(CHAT_ID, html, "HTML");
}

void telegramReply(const String& chat_id, const char* html) {
  bot.sendMessage(chat_id, html, "HTML");
}

void telegramReplyWithMenu(const String& chat_id, const char* html) {
  bot.sendMessageWithReplyKeyboard(chat_id, html, "HTML", KEYBOARD_JSON, true);
}

// One log line for chat: "pump off, 6m run". Relative ages are used rather than
// clock times so this needs no timezone handling on the device.
void evLabelShort(const LogEvent& e, char* out, size_t n) {
  char d[16];
  switch (e.code) {
    case EV_PUMP_ON:
      snprintf(out, n, "pump on (%s)", causeName(e.cause));
      break;
    case EV_PUMP_OFF:
      fmtDur(d, sizeof d, e.detail);
      snprintf(out, n, "pump off, %s run", d);
      break;
    case EV_OVERFLOW_ON:  snprintf(out, n, "OVERFLOW 90%%"); break;
    case EV_OVERFLOW_OFF:
      fmtDur(d, sizeof d, e.detail);
      snprintf(out, n, "overflow cleared, %s", d);
      break;
    case EV_BLOCKED:      snprintf(out, n, "BLOCKED - not draining"); break;
    case EV_WIFI_DOWN:    snprintf(out, n, "wifi lost"); break;
    case EV_WIFI_UP:      snprintf(out, n, "wifi back"); break;
    case EV_SWITCH_OFF:   snprintf(out, n, "switch off"); break;
    case EV_SWITCH_ON:    snprintf(out, n, "switch on"); break;
    case EV_OTA:          snprintf(out, n, "firmware update"); break;
    case EV_CONFIG:
      fmtDur(d, sizeof d, e.detail);
      snprintf(out, n, "run time set to %s", d);
      break;
    default:              snprintf(out, n, "powered on"); break;
  }
}

// Last few events, newest first. Capped at LOG_TELEGRAM_LINES so a chat reply
// never turns into a wall of text - the dashboard is for browsing the full ring.
const uint8_t LOG_TELEGRAM_LINES = 8;

void buildLogCard(char* out, size_t n) {
  if (logSeq == 0) {
    snprintf(out, n, "\xF0\x9F\x93\x8B <b>Recent activity</b>\nNothing logged yet.");
    return;
  }

  const uint32_t oldest = logOldest();
  uint32_t from = (logSeq > LOG_TELEGRAM_LINES) ? logSeq - LOG_TELEGRAM_LINES : 0;
  if (from < oldest) from = oldest;

  const unsigned long nowS = millis() / 1000;
  size_t used = snprintf(out, n, "\xF0\x9F\x93\x8B <b>Recent activity</b>\n<pre>");

  for (uint32_t s = logSeq; s-- > from; ) {          // newest first
    const LogEvent& e = logBuf[s & (LOG_CAPACITY - 1)];
    char label[40], ago[16];
    evLabelShort(e, label, sizeof label);
    fmtDur(ago, sizeof ago, nowS > e.sec ? nowS - e.sec : 0);
    const int wrote = snprintf(out + used, n - used, "%-8s %s\n", ago, label);
    if (wrote < 0 || (size_t)wrote >= n - used) break;   // out of room, stop cleanly
    used += wrote;
  }
  snprintf(out + used, n - used, "</pre>");
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
    // A frozen run reads as "held", not "left" - the pump isn't running.
    snprintf(run, sizeof run, (state == ST_SWITCH_OFF) ? "%s held" : "%s left", t);
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
    "Runs     %lu, %lu overflow\n"
    "Total    %s\n"
    "Uptime   %s</pre>",
    stateGlyph(), stateName(),
    pumpOn ? "on" : "off",
    level,
    switchEnabled ? "on (auto)" : "off (inhibited)",
    run, pumpStarts, overflowCount, total, up);
}

void handleTelegramMessages(int numNewMessages) {
  for (int i = 0; i < numNewMessages; i++) {
    const String chat_id = bot.messages[i].chat_id;
    if (chat_id != CHAT_ID) continue;          // ignore everyone else
    String text = bot.messages[i].text;
    text.trim();
    text.toLowerCase();

    if (text == "/status") {
      char card[384];
      buildStatusCard(card, sizeof card);
      telegramReply(chat_id, card);

    } else if (text == "/log") {
      char card[512];
      buildLogCard(card, sizeof card);
      telegramReply(chat_id, card);

    } else if (text == "/pumpon") {
      const char* why = startBlockedReason();
      if (why) {
        char msg[160];
        snprintf(msg, sizeof msg, "\xE2\x9A\xA0\xEF\xB8\x8F <b>Refused</b>\n%s", why);
        telegramReply(chat_id, msg);
      } else {
        startManualRun();
        char msg[96];
        snprintf(msg, sizeof msg,
                 "\xF0\x9F\x9F\xA1 <b>Pump started</b>\nManual run, stops after %s.", runDurStr);
        telegramReply(chat_id, msg);
      }

    } else if (text == "/pumpoff") {
      const char* why = stopBlockedReason();
      if (why) {
        char msg[160];
        snprintf(msg, sizeof msg, "\xE2\x9A\xA0\xEF\xB8\x8F <b>Refused</b>\n%s", why);
        telegramReply(chat_id, msg);
      } else {
        setPump(false, CAUSE_MANUAL);
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
      char msg[300];
      snprintf(msg, sizeof msg,
        "\xF0\x9F\x92\xA7 <b>AC Drain</b>\n"
        "<pre>/status   level, pump, uptime\n"
        "/log      last 8 events\n"
        "/pumpon   %s manual run\n"
        "/pumpoff  stop the pump\n"
        "/uptime   how long since boot</pre>\n"
        "Dashboard: http://%s", runDurStr, ipStr);
      telegramReplyWithMenu(chat_id, msg);
    }
  }
}

// ---------------- WEB SERVER ----------------
/*
 * Authentication.
 *
 * On a LAN-only controller this was reasonably skippable. The moment the
 * dashboard is reachable from outside the house - by tunnel, VPN or anything
 * else - it stops being skippable: every control this page offers is a pump and
 * a mains relay, and /api/config can now set how long that relay stays closed.
 *
 * Digest rather than Basic, so the password is not sent in the clear on each of
 * the ~1800 polls an hour this page makes. That protects the credential, not the
 * traffic: the page and its JSON are still plain HTTP, so the transport itself
 * has to provide the encryption. Every remote-access route in the README does.
 *
 * Defining WEB_PASSWORD switches this on. Left undefined, the dashboard is open
 * exactly as it was before - fine on a trusted LAN, and the boot log says so
 * out loud rather than letting it pass unnoticed.
 */
#ifdef WEB_PASSWORD
  #ifndef WEB_USER
    #define WEB_USER "admin"
  #endif
// Returns true when the request has already been answered with a 401, so every
// handler can start with: if (needsAuth()) return;
bool needsAuth() {
  if (server.authenticate(WEB_USER, WEB_PASSWORD)) return false;
  server.requestAuthentication(DIGEST_AUTH, "AC Drain", "Authentication required");
  return true;
}
#else
static inline bool needsAuth() { return false; }
#endif

void handleRoot() {
  if (needsAuth()) return;
  server.send_P(200, "text/html", INDEX_HTML);
}

void handleStatus() {
  if (needsAuth()) return;
  const unsigned long now = millis();
  const unsigned long totalMs = pumpTotalMs + (pumpOn ? now - pumpSinceMs : 0);

  // Fixed buffer + snprintf instead of String concatenation: the dashboard
  // polls this every 2s forever, and String churn is what fragments the heap.
  // "manual" is the raw contact position; "enabled" is that with the polarity
  // flag applied, which is the one the dashboard shows - so a flipped
  // SWITCH_CLOSED_IS_ENABLED never leaves the panel disagreeing with the pump.
  //
  // "duration" is the cap on the run in progress, which is what the progress
  // bar is drawn against; "autoDur" and "ovfDur" are the two configured caps
  // and "minDur"/"maxDur" the range the config form may offer, so the page
  // states no duration of its own and cannot fall out of step with the device.
  //
  // "host" is width-limited rather than trusted to be short: it comes from
  // secrets.h, and a long one would truncate the JSON into something the
  // dashboard could not parse - which would look like the controller dying.
  char buf[512];
  snprintf(buf, sizeof buf,
    "{\"state\":%u,\"pump\":%s,\"reed70\":%s,\"reed90\":%s,\"manual\":%s,"
    "\"enabled\":%s,"
    "\"elapsed\":%lu,\"remaining\":%ld,\"duration\":%lu,"
    "\"autoDur\":%lu,\"ovfDur\":%lu,\"minDur\":%lu,\"maxDur\":%lu,\"blocked\":%s,"
    "\"starts\":%lu,\"overflows\":%lu,\"blocks\":%lu,"
    "\"pumpTotal\":%lu,\"uptime\":%lu,\"rssi\":%d,\"ip\":\"%s\","
    "\"ota\":%s,\"host\":\"%.32s\",\"seq\":%lu,\"boot\":%lu}",
    (unsigned)state, jbool(pumpOn), jbool(reedHigh.state), jbool(reedOverflow.state),
    jbool(switchManual.state), jbool(switchEnabled),
    pumpOn ? (now - pumpSinceMs) / 1000 : 0UL,
    remainingSecs(now),
    currentCapMs() / 1000UL,
    runDurationMs / 1000UL, overflowRunMs / 1000UL, RUN_MIN_S, RUN_MAX_S,
    jbool(overflowMaxRunHit),
    pumpStarts, overflowCount, blockedCount,
    totalMs / 1000UL, now / 1000UL,
    wifiUp ? WiFi.RSSI() : 0,
    ipStr,
    jbool(otaReady), OTA_HOSTNAME,
    (unsigned long)logSeq, (unsigned long)bootEpoch);

  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", buf);
}

/*
 * GET /api/log?since=<seq>
 *
 * Streamed with chunked encoding rather than assembled into one String: a full
 * 256-event dump is ~8 KB, and building that on the heap is exactly the kind of
 * allocation this firmware avoids. Events are emitted as compact arrays
 * [seq, uptimeSec, code, cause, detail] - the dashboard owns the labels, so the
 * wire format stays small on a 2-second poll.
 */
void handleLog() {
  if (needsAuth()) return;
  uint32_t since = server.hasArg("since")
                 ? strtoul(server.arg("since").c_str(), nullptr, 10) : 0;
  const uint32_t oldest = logOldest();
  const bool truncated = since < oldest;   // caller fell behind the ring
  if (truncated) since = oldest;

  server.sendHeader("Cache-Control", "no-store");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");

  char chunk[96];
  snprintf(chunk, sizeof chunk,
           "{\"seq\":%lu,\"oldest\":%lu,\"boot\":%lu,\"uptime\":%lu,\"lost\":%s,\"ev\":[",
           (unsigned long)logSeq, (unsigned long)oldest, (unsigned long)bootEpoch,
           millis() / 1000UL, jbool(truncated));
  server.sendContent(chunk);

  for (uint32_t s = since; s < logSeq; s++) {
    const LogEvent& e = logBuf[s & (LOG_CAPACITY - 1)];
    snprintf(chunk, sizeof chunk, "%s[%lu,%lu,%u,%u,%u]",
             (s == since) ? "" : ",",
             (unsigned long)s, (unsigned long)e.sec, e.code, e.cause, e.detail);
    server.sendContent(chunk);
  }

  server.sendContent("]}");
  server.sendContent("");   // zero-length chunk terminates the response
}

// GET /api/log.csv - archive the ring off-device before a power cycle clears it.
void handleLogCsv() {
  if (needsAuth()) return;
  server.sendHeader("Content-Disposition", "attachment; filename=ac-drain-log.csv");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/csv", "");
  server.sendContent("seq,epoch,uptime_s,event,cause,duration_s\n");

  char chunk[110];
  for (uint32_t s = logOldest(); s < logSeq; s++) {
    const LogEvent& e = logBuf[s & (LOG_CAPACITY - 1)];
    snprintf(chunk, sizeof chunk, "%lu,%lu,%lu,%s,%s,%u\n",
             (unsigned long)s,
             bootEpoch ? (unsigned long)(bootEpoch + e.sec) : 0UL,
             (unsigned long)e.sec, evName(e.code), causeName(e.cause), e.detail);
    server.sendContent(chunk);
  }
  server.sendContent("");
}

void handlePumpOn() {
  if (needsAuth()) return;
  const char* why = startBlockedReason();
  if (why) { server.send(409, "text/plain", why); return; }
  startManualRun();
  char msg[48];
  snprintf(msg, sizeof msg, "Pump started - %s cap", runDurStr);
  server.send(200, "text/plain", msg);
}

void handlePumpOff() {
  if (needsAuth()) return;
  const char* why = stopBlockedReason();
  if (why) { server.send(409, "text/plain", why); return; }
  setPump(false, CAUSE_MANUAL);
  state = ST_IDLE;
  server.send(200, "text/plain", "Pump stopped");
}

/*
 * POST /api/config?run=<sec>&overflow=<sec>   - either argument, or both
 * POST /api/config?reset=1                    - back to the compile-time defaults
 *
 * Timing the pump is a stopwatch job that takes a few attempts, and doing it by
 * editing a constant and reflashing is what made it tedious enough to leave at
 * a guess. Anything omitted keeps its current value.
 *
 * Taking effect immediately includes a run already in progress: the state
 * machine measures elapsed time against whatever the cap is on the pass it
 * checks, so shortening it below what has already elapsed stops the pump on the
 * next pass rather than at some point in the past. That is the behaviour you
 * want while standing over the tray with a stopwatch, which is the only time
 * anyone is on this endpoint.
 */
void handleConfig() {
  if (needsAuth()) return;
  unsigned long runS = runDurationMs / 1000UL;
  unsigned long ovfS = overflowRunMs / 1000UL;
  const bool reset = server.hasArg("reset");

  if (reset) {
    runS = RUN_DEFAULT_MS / 1000UL;
    ovfS = OVERFLOW_DEFAULT_MS / 1000UL;
  } else {
    if (server.hasArg("run"))      runS = strtoul(server.arg("run").c_str(), nullptr, 10);
    if (server.hasArg("overflow")) ovfS = strtoul(server.arg("overflow").c_str(), nullptr, 10);
  }

  char msg[160];
  if (!runCapInRange(runS) || !runCapInRange(ovfS)) {
    // Reject the whole request rather than applying the half of it that was
    // valid - a partly-applied setting is the worst of both answers.
    snprintf(msg, sizeof msg, "Run times must be between %lu and %lu seconds.",
             RUN_MIN_S, RUN_MAX_S);
    server.send(400, "text/plain", msg);
    return;
  }

  const bool changed = (runS != runDurationMs / 1000UL) || (ovfS != overflowRunMs / 1000UL);
  applyRunCaps(runS, ovfS);
  if (changed) {
    storeRunCaps(runS, ovfS);
    logAdd(EV_CONFIG, CAUSE_MANUAL, (uint16_t)runS);
    Serial.printf("[CONFIG] run caps set - %s cycle, %s overflow\n", runDurStr, ovfDurStr);
  }

  snprintf(msg, sizeof msg, "%s - %s cycle, %s overflow cap",
           changed ? (reset ? "Reset" : "Saved") : "Unchanged", runDurStr, ovfDurStr);
  server.send(200, "text/plain", msg);
}

// ---------------- CONTROL LOGIC ----------------
void handleOverflow(unsigned long now) {
  if (!reedOverflow.state) {
    if (state == ST_OVERFLOW) {
      // Leaving overflow MUST stop the pump. Previously the state was reset to
      // idle with the relay still closed, so if the 70% float was dry too the
      // pump had nothing left to switch it off and ran indefinitely.
      setPump(false, CAUSE_OVERFLOW);
      state = ST_IDLE;
      overflowAlerted = false;
      const unsigned long heldS = (now - overflowStartMs) / 1000;
      logAdd(EV_OVERFLOW_OFF, CAUSE_NONE, heldS > 65535 ? 65535 : (uint16_t)heldS);
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
    overflowCount++;
    logAdd(EV_OVERFLOW_ON);
    Serial.println("[OVERFLOW] 90% reached - forcing pump ON");
  }

  writeIfChanged(PIN_LED_RED, ledRedOn, true);
  writeIfChanged(PIN_BUZZER, buzzerOn, true);

  // The overflow run gets its own, longer cap: there is more water to shift at
  // 90% than the normal cycle is timed for, and cutting it off at 4 min would
  // report a blockage that is really just a bigger job.
  if (!overflowMaxRunHit) {
    setPump(true, CAUSE_OVERFLOW);
    if (elapsedSince(now, overflowStartMs) >= overflowRunMs) {
      overflowMaxRunHit = true;
      setPump(false, CAUSE_OVERFLOW);
      blockedCount++;
      logAdd(EV_BLOCKED);
      Serial.println("[OVERFLOW] full run done, still wet - pump stopped, needs a look");
      char msg[200];
      snprintf(msg, sizeof msg,
               "\xF0\x9F\x94\xB4 <b>Not draining</b>\n"
               "The pump ran a full %s and the water is still at 90%%. "
               "Stopped to protect it - check for a blockage now.", ovfDurStr);
      telegramSend(msg);
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

// Contact position -> "automatic operation allowed", with the polarity flag
// applied once here so nothing else has to reason about it.
static inline bool switchIsEnabled() {
  return SWITCH_CLOSED_IS_ENABLED ? switchManual.state : !switchManual.state;
}

/*
 * The manual switch, as an ENABLE switch.
 *
 * OFF stops the pump at once and suspends automatic operation. The run that was
 * interrupted is frozen rather than cancelled: we keep how far it had got, so
 * switching back on finishes the remainder instead of restarting a cycle that
 * was nearly done.
 *
 * Overflow never reaches this function - loop() skips it while the 90% float is
 * wet, deliberately. The switch does not get a vote on water damage.
 */
void serviceEnableSwitch(unsigned long now) {
  if (switchEnabled) {
    if (state != ST_SWITCH_OFF) return;

    // An auto cycle only resumes if the 70% float is still wet: if it dried
    // while we were inhibited there is nothing left to pump, and resuming would
    // run the pump against an empty tray. A manual run was asked for
    // explicitly, so it owns its own remainder.
    if (heldState == ST_RUNNING && !reedHigh.state) heldState = ST_IDLE;

    if (heldState != ST_IDLE) {
      pumpStartMs = now - heldElapsedMs;      // finish the remainder, don't restart
      state = heldState;
      setPump(true, heldState == ST_RUNNING ? CAUSE_AUTO : CAUSE_MANUAL);
      Serial.println("[SWITCH] on - resuming the held run");
    } else {
      state = ST_IDLE;
      Serial.println("[SWITCH] on - automatic operation resumed");
    }
    heldState = ST_IDLE;
    heldElapsedMs = 0;
    return;
  }

  if (state == ST_SWITCH_OFF) return;

  if (state == ST_RUNNING || state == ST_MANUAL) {
    const unsigned long elapsed = elapsedSince(now, pumpStartMs);
    heldElapsedMs = (elapsed >= runDurationMs) ? runDurationMs : elapsed;
    heldState = state;
  } else {
    heldState = ST_IDLE;               // nothing was running, nothing to hold
    heldElapsedMs = 0;
  }
  setPump(false, CAUSE_SWITCH);
  state = ST_SWITCH_OFF;
  Serial.println("[SWITCH] off - pump stopped, automatic operation suspended");
}

void handleAutoCycle(unsigned long now) {
  switch (state) {
    case ST_IDLE:
      if (reedHigh.state && elapsedSince(now, lastStopMs) >= MIN_OFF_MS) {
        pumpStartMs = now;
        state = ST_RUNNING;
        setPump(true, CAUSE_AUTO);
        Serial.printf("[CYCLE] 70%% reached - pump ON for %s\n", runDurStr);
      }
      break;

    case ST_RUNNING:
    case ST_MANUAL:
      if (elapsedSince(now, pumpStartMs) >= runDurationMs) {
        const bool wasAuto = (state == ST_RUNNING);
        setPump(false, wasAuto ? CAUSE_AUTO : CAUSE_MANUAL);
        state = ST_IDLE;
        // Still wet? ST_IDLE above starts a fresh cycle once MIN_OFF_MS has
        // passed - that is the "repeat automatically" behaviour.
        Serial.printf(wasAuto ? "[CYCLE] %s elapsed - pump OFF\n"
                              : "[MANUAL] %s cap reached - pump OFF\n", runDurStr);
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

// Fixes the log's wall-clock reference once, then never asks again. The device
// keeps no calendar of its own - events store uptime seconds, and bootEpoch is
// what lets the dashboard render them as real times.
void syncClock() {
  if (bootEpoch || !wifiUp) return;
  const time_t t = time(nullptr);
  if (t < 1700000000) return;                    // SNTP hasn't answered yet
  bootEpoch = (uint32_t)t - (uint32_t)(millis() / 1000);
  Serial.printf("[TIME] clock synced, boot epoch %lu\n", (unsigned long)bootEpoch);
}

void serviceWifi(unsigned long now) {
  const bool up = (WiFi.status() == WL_CONNECTED);
  if (up != wifiUp) {
    wifiUp = up;
    if (up) {
      cacheIp();
      logAdd(EV_WIFI_UP);
      beginOta();          // first join, or a router reboot we have come back from
      Serial.printf("[WIFI] connected - http://%s\n", ipStr);
    } else {
      logAdd(EV_WIFI_DOWN);
      Serial.println("[WIFI] lost - retrying, pump logic keeps running locally");
    }
  }
  // The original never reconnected: one router reboot and the dashboard and
  // every Telegram alert were gone until someone power-cycled the board.
  if (!up && (now - lastWifiTryMs) >= WIFI_RETRY_MS) {
    lastWifiTryMs = now;
    WiFi.reconnect();
  }
  syncClock();
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

// ---------------- OTA ----------------
/*
 * Over-the-air updates, so this can be reflashed from a laptop on the sofa
 * rather than unplugging the controller and carrying it to a USB cable.
 *
 * Three things matter more here than they would on a blinking-LED project:
 *
 *  1. THE PUMP IS PARKED BEFORE THE FIRST BYTE LANDS. loop() does not run
 *     during a transfer, so a relay left closed stays closed for the whole
 *     upload and the reboot after it - with nothing still running that could
 *     switch it off if the transfer stalled halfway. onStart() drives the relay
 *     pin to its idle level directly as well as through setPump(), so it is off
 *     even if the incoming firmware never boots.
 *
 *  2. AN UPDATE IS REFUSED WHILE THE OVERFLOW HANDLER IS PUMPING. That is the
 *     one moment the pump is the only thing between the tray and the floor, and
 *     the priority rule that governs everything else here - overflow outranks
 *     the lot - applies to firmware too. The refusal is simply not servicing the
 *     OTA port: the sender times out and reports it. Once a blocked overflow has
 *     stopped the pump the update is allowed again, which is usually the exact
 *     moment somebody wants to push a fix.
 *
 *  3. IT IS PASSWORD PROTECTED, and the build refuses to compile without one.
 *     An open OTA port on the LAN hands over the pump, the relay and the WiFi
 *     credentials to anyone who can reach it.
 *
 * ArduinoOTA.begin() also brings up mDNS, so the dashboard answers on
 * http://OTA_HOSTNAME.local once the HTTP service is advertised below - no more
 * hunting for the address the router handed out this week.
 */
void beginOta() {
  if (otaReady || !wifiUp) return;

  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);

  ArduinoOTA.onStart([]() {
    // Hardware first, bookkeeping second. Everything below this point can be
    // interrupted by a failed transfer; the relay must not be.
    setPump(false, CAUSE_OTA);
    digitalWrite(PIN_PUMP, PUMP_IDLE_LEVEL);
    writeIfChanged(PIN_LED_RED, ledRedOn, false);
    writeIfChanged(PIN_BUZZER, buzzerOn, false);

    // Drop any timed run rather than leave a half-elapsed one behind: if the
    // update fails and loop() resumes, ST_RUNNING with the pump off would sit
    // out the rest of its cap doing nothing. From ST_IDLE a still-wet float
    // just starts a fresh cycle, and an off switch re-asserts itself on the
    // next pass.
    if (state != ST_OVERFLOW) state = ST_IDLE;

    logAdd(EV_OTA, CAUSE_OTA);
    Serial.println("\n[OTA] update starting - pump parked, holding the loop");
  });

  ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
    static unsigned int lastPct = 999;
    const unsigned int pct = total ? (done * 100U) / total : 0;
    if (pct == lastPct) return;              // one line per percent, not per packet
    lastPct = pct;
    Serial.printf("[OTA] %u%%\r", pct);
  });

  ArduinoOTA.onEnd([]() {
    Serial.println("\n[OTA] written - rebooting into the new firmware");
  });

  ArduinoOTA.onError([](ota_error_t err) {
    // loop() resumes after this, so the controller goes straight back to
    // watching the floats on the firmware it already had. Nothing to recover.
    Serial.printf("\n[OTA] failed (error %u) - keeping the current firmware\n", err);
  });

  ArduinoOTA.begin();
  MDNS.addService("http", "tcp", 80);        // dashboard on http://OTA_HOSTNAME.local
  otaReady = true;
  Serial.printf("[OTA] ready as %s.local - ./flash main --ota\n", OTA_HOSTNAME);
}

// Overflow outranks a firmware update, so the OTA port goes unserviced while
// the safety handler is actually driving the pump. Once it has given up and
// stopped (overflowMaxRunHit) the pump is idle and an update is safe again.
static inline bool otaAllowed() {
  return otaReady && !(state == ST_OVERFLOW && !overflowMaxRunHit);
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

  // Restores whatever was set from the dashboard, and renders the strings every
  // later message quotes - so the caps and the words describing them come from
  // the same place, once, before anything can print either.
  loadRunCaps();

  Serial.printf("\n[BOOT] AC drain controller - %s cycle, %s overflow cap\n",
                runDurStr, ovfDurStr);
  logAdd(EV_BOOT);

  pinMode(PIN_LED_GREEN, OUTPUT); digitalWrite(PIN_LED_GREEN, LOW);
  pinMode(PIN_LED_RED, OUTPUT);   digitalWrite(PIN_LED_RED, LOW);
  pinMode(PIN_BUZZER, OUTPUT);    digitalWrite(PIN_BUZZER, LOW);

  // Self-test: if this is silent, the fault is in the buzzer wiring rather
  // than in the overflow logic that normally drives the same pin.
  if (BUZZER_BOOT_TEST) {
    Serial.println("[BUZZER] self-test - listen for one short beep");
    digitalWrite(PIN_BUZZER, HIGH);
    delay(BUZZER_BOOT_TEST_MS);
    digitalWrite(PIN_BUZZER, LOW);
  }

  reedHigh.begin(PIN_REED_HIGH);
  reedOverflow.begin(PIN_REED_OVERFLOW);
  switchManual.begin(PIN_SWITCH_MANUAL);

  // Adopt the switch's actual position rather than assuming enabled: booting
  // with it off must start inhibited, and seeding the edge detector here is
  // what stops the first loop pass logging a transition that never happened.
  switchEnabled = switchIsEnabled();
  state = switchEnabled ? ST_IDLE : ST_SWITCH_OFF;
  Serial.printf("[SWITCH] %s at boot\n", switchEnabled ? "on" : "off");

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
    logAdd(EV_WIFI_UP);
    // UTC only - the dashboard localises. Nothing to misconfigure here.
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    Serial.printf("[WIFI] connected - http://%s\n", ipStr);
    beginOta();
    char msg[220];
    snprintf(msg, sizeof msg,
             "\xE2\x9A\xAA <b>Controller online</b>\n"
             "Dashboard: http://%s\nor http://%s.local", ipStr, OTA_HOSTNAME);
    telegramReplyWithMenu(CHAT_ID, msg);   // also installs the tap keyboard
  } else {
    Serial.println("[WIFI] failed - continuing offline, reed/pump logic still works");
  }

  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/log", HTTP_GET, handleLog);
  server.on("/api/log.csv", HTTP_GET, handleLogCsv);
  server.on("/pump/on", HTTP_POST, handlePumpOn);
  server.on("/pump/off", HTTP_POST, handlePumpOff);
  server.on("/api/config", HTTP_POST, handleConfig);
  server.onNotFound([]() { server.send(404, "text/plain", "not found"); });
  server.begin();
#ifdef WEB_PASSWORD
  Serial.println("[HTTP] dashboard up on port 80 - digest auth as " WEB_USER);
#else
  // Said out loud, every boot. An unauthenticated pump control is a defensible
  // choice on a LAN you trust and an indefensible one the moment anything
  // tunnels in from outside, and the difference is easy to forget you made.
  Serial.println("[HTTP] dashboard up on port 80 - NO PASSWORD SET, anyone on this "
                 "network can run the pump. Set WEB_PASSWORD in secrets.h before "
                 "exposing it beyond the LAN.");
#endif
}

// ---------------- MAIN LOOP ----------------
void loop() {
  const unsigned long now = millis();   // one read, one consistent view of time

  reedHigh.update(now);
  reedOverflow.update(now);
  switchManual.update(now);

  // Log the switch on its real edge, even mid-overflow: the position changed
  // whether or not it is allowed to act on it yet, and a log that only shows
  // the ones that took effect would be misleading during an overflow.
  const bool enabledNow = switchIsEnabled();
  if (enabledNow != switchEnabled) {
    switchEnabled = enabledNow;
    logAdd(enabledNow ? EV_SWITCH_ON : EV_SWITCH_OFF, CAUSE_SWITCH);
  }

  server.handleClient();

  // Safety first, and always before anything that can block. Overflow outranks
  // the switch, so the switch is only serviced once the 90% float is dry.
  handleOverflow(now);
  if (state != ST_OVERFLOW) {
    serviceEnableSwitch(now);
    if (state != ST_SWITCH_OFF) handleAutoCycle(now);
  }

  // After the safety logic, so `state` is this pass's answer: an update that
  // arrives while the overflow handler is driving the pump is left unanswered
  // until the pump is out of its hands. Once a transfer does start, this call
  // does not return until the new firmware is written and the board reboots.
  if (otaAllowed()) ArduinoOTA.handle();

  serviceWifi(now);
  serviceTelegram();

  delay(1);   // yields to the WiFi/idle tasks instead of spinning a core flat out
}
