/*
 * Icebox Monitor
 * ESP32 + PlatformIO
 *
 * Measures:
 *   - Fridge temperature   (DS18B20)
 *   - Crisper temperature  (DS18B20)
 *   - Compressor duty cycle, sensed via a PC817 optocoupler that
 *     opto-isolates the 12V solid-state-relay output driving the
 *     compressor. See README.md for the wiring.
 *
 * Serves a live dashboard + historical graph over WiFi at a static IP.
 */

#include <WiFi.h>
#include <WiFiUdp.h>
#include <ESPAsyncWebServer.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <ArduinoJson.h>
#include <time.h>
#include <esp_task_wdt.h>
#include "secrets.h" // WIFI_SSID/WIFI_PASSWORD - see secrets.h.example

// ---------------- USER CONFIG ----------------
IPAddress local_IP(192, 168, 8, 67);
IPAddress gateway (192, 168, 8, 1);   // <-- set to your router's IP
IPAddress subnet  (255, 255, 255, 0);
IPAddress dns1    (192, 168, 8, 1);
IPAddress dns2    (8, 8, 8, 8);

// GPIO assignments (chosen to avoid ESP32 strapping pins 0/2/5/12/15).
// If your board is a WROVER module (has PSRAM), move FRIDGE/CRISPER off
// 16/17 since those are used for PSRAM on WROVER — pick e.g. 25/26 instead.
#define FRIDGE_ONEWIRE_PIN   4
#define CRISPER_ONEWIRE_PIN  16
#define COMPRESSOR_SENSE_PIN 17   // PC817 phototransistor collector

// Sampling / history
#define TEMP_READ_INTERVAL_MS   10000UL   // read probes every 10 s
#define HISTORY_INTERVAL_MS     60000UL   // one history point per minute
#define HISTORY_SIZE            1440      // 24 h at 1-minute resolution
#define DEBOUNCE_MS               100UL

// Push telemetry to the Victron dashboard board over UDP instead of it
// having to poll this board's HTTP /api/live endpoint. Plain unicast (not
// a subnet broadcast) since there's exactly one consumer - fire-and-forget,
// no TCP connection/socket lifecycle on either side. /api/live and
// /api/history stay too, still used by this board's own dashboard page.
IPAddress DASHBOARD_IP(192, 168, 8, 66);
const uint16_t TELEMETRY_UDP_PORT = 2000; // same port the heater board uses - one socket on the dashboard side, senders told apart by IP
const unsigned long TELEMETRY_INTERVAL_MS = 10000UL; // same cadence as temp reads
WiFiUDP telemetryUdp;
unsigned long lastTelemetryAt = 0;
uint32_t bootFreeHeap = 0; // set once in setup(), see there for why

// ---- WiFi reliability ----
// If this board's WiFi ever drops (fridges/coolers are often a rough RF
// environment - lots of metal near the antenna) and doesn't recover on
// its own, there was previously no way out other than someone noticing
// data had gone missing and power-cycling it. This actively retries a
// reconnect, and does a full reboot as a last resort if it's been down
// too long for a plain reconnect to be working.
unsigned long wifiDownSince = 0;      // 0 = currently connected
unsigned long lastReconnectAttempt = 0;
const unsigned long WIFI_RECONNECT_RETRY_MS = 15000UL;  // don't hammer reconnect() more often than this
const unsigned long WIFI_FORCE_RESTART_MS   = 180000UL; // 3 min continuously down -> full reboot

void maintainWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    wifiDownSince = 0;
    return;
  }
  unsigned long now = millis();
  if (wifiDownSince == 0) {
    wifiDownSince = now;
    Serial.println("WiFi dropped - will attempt to reconnect");
  }
  if (now - wifiDownSince > WIFI_FORCE_RESTART_MS) {
    Serial.println("WiFi down for 3+ minutes - restarting");
    delay(200); // let the Serial line flush before reset
    ESP.restart();
  }
  if (now - lastReconnectAttempt > WIFI_RECONNECT_RETRY_MS) {
    lastReconnectAttempt = now;
    Serial.println("Attempting WiFi reconnect...");
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }
}

// NTP (gives real clock times on the graph instead of "seconds since boot")
const char* NTP_SERVER    = "pool.ntp.org";
const long  GMT_OFFSET_SEC = -25200;   // <-- set to your timezone offset in seconds
const int   DST_OFFSET_SEC = 0;
// ----------------------------------------------

OneWire oneWireFridge(FRIDGE_ONEWIRE_PIN);
OneWire oneWireCrisper(CRISPER_ONEWIRE_PIN);
DallasTemperature fridgeSensor(&oneWireFridge);
DallasTemperature crisperSensor(&oneWireCrisper);

AsyncWebServer server(80);

float fridgeTempC  = NAN;
float crisperTempC = NAN;

bool compressorOnDebounced = false;
bool lastRawState = HIGH;
unsigned long lastDebounceTime = 0;
unsigned long onTimeAccumMs = 0;
unsigned long windowStartMs = 0;
unsigned long lastLoopMs = 0;
uint32_t compressorCycles = 0;
uint8_t lastDutyPercent = 0;

struct HistoryPoint {
  uint32_t timestamp;     // unix time (or seconds-since-boot if NTP not synced)
  float fridgeTemp;
  float crisperTemp;
  uint8_t dutyPercent;
};

HistoryPoint history[HISTORY_SIZE];
int historyHead = 0;      // next write index
int historyCount = 0;     // number of valid entries so far

bool tempConversionInProgress = false;
unsigned long tempRequestedAt = 0;
unsigned long lastTempReadAt = 0;
unsigned long lastHistoryPushAt = 0;

uint32_t nowTimestamp() {
  time_t t = time(nullptr);
  if (t > 1700000000UL) return (uint32_t)t;   // NTP synced (post ~2023)
  return millis() / 1000;                     // fallback: seconds since boot
}

void startTempConversion() {
  fridgeSensor.requestTemperatures();
  crisperSensor.requestTemperatures();
  tempConversionInProgress = true;
  tempRequestedAt = millis();
}

void finishTempConversion() {
  float f = fridgeSensor.getTempCByIndex(0);
  float c = crisperSensor.getTempCByIndex(0);
  if (f != DEVICE_DISCONNECTED_C) fridgeTempC = f;
  if (c != DEVICE_DISCONNECTED_C) crisperTempC = c;
  tempConversionInProgress = false;
}

// Reads the PC817 output, debounces it, and accumulates ON time.
// HIGH on COMPRESSOR_SENSE_PIN = compressor/SSR is ON.
void updateCompressorSensing() {
  bool raw = digitalRead(COMPRESSOR_SENSE_PIN);
  if (raw != lastRawState) {
    lastDebounceTime = millis();
    lastRawState = raw;
  }
  if ((millis() - lastDebounceTime) > DEBOUNCE_MS) {
    bool newState = (raw == HIGH);
    if (newState != compressorOnDebounced) {
      compressorOnDebounced = newState;
      if (compressorOnDebounced) compressorCycles++;
    }
  }

  unsigned long now = millis();
  unsigned long dt = now - lastLoopMs;
  lastLoopMs = now;
  if (compressorOnDebounced) onTimeAccumMs += dt;
}

void pushHistoryPoint() {
  unsigned long now = millis();
  unsigned long windowMs = now - windowStartMs;
  if (windowMs == 0) windowMs = 1;
  uint8_t duty = (uint8_t)((onTimeAccumMs * 100UL) / windowMs);
  if (duty > 100) duty = 100;
  lastDutyPercent = duty;

  history[historyHead] = { nowTimestamp(), fridgeTempC, crisperTempC, duty };
  historyHead = (historyHead + 1) % HISTORY_SIZE;
  if (historyCount < HISTORY_SIZE) historyCount++;

  onTimeAccumMs = 0;
  windowStartMs = now;
}

// The "current" duty cycle reported live (API + dashboard + telemetry) is
// a trailing 4-hour average rather than the last single 1-minute reading
// - a fridge compressor's duty naturally jumps around a lot minute to
// minute (door opens, ice maker, ambient temp swings), so a 1-minute
// snapshot is noisy and not very representative of "how hard is this
// thing working". Averaging the last DUTY_WINDOW_MINUTES worth of the
// same per-minute samples already being recorded for the history graph
// gives a much steadier number, without changing the graph's resolution
// at all - history[] still holds one point per minute either way.
// Falls back to averaging over however much history exists yet if the
// board hasn't been up for a full 4 hours since boot.
#define DUTY_WINDOW_MINUTES (4 * 60) // 4 hours, at 1 history point/minute

uint8_t dutyPercentLastWindow() {
  int n = (historyCount < DUTY_WINDOW_MINUTES) ? historyCount : DUTY_WINDOW_MINUTES;
  if (n == 0) return 0;
  uint32_t sum = 0;
  int start = (historyHead - n + HISTORY_SIZE) % HISTORY_SIZE;
  for (int i = 0; i < n; i++) {
    int idx = (start + i) % HISTORY_SIZE;
    sum += history[idx].dutyPercent;
  }
  return (uint8_t)(sum / n);
}

// Pushed to the dashboard every TELEMETRY_INTERVAL_MS (see loop()) - same
// fields /api/live serves, so the two stay trivially consistent.
void broadcastTelemetry() {
  JsonDocument doc;
  doc["fridge"]     = isnan(fridgeTempC) ? -127 : fridgeTempC;
  doc["crisper"]    = isnan(crisperTempC) ? -127 : crisperTempC;
  doc["compressor"] = compressorOnDebounced;
  doc["duty"]       = dutyPercentLastWindow();
  doc["cycles"]     = compressorCycles;
  doc["rssi"]       = WiFi.RSSI(); // helps tell "RF issue at this location" apart from other causes if data goes missing

  String out;
  serializeJson(doc, out);
  telemetryUdp.beginPacket(DASHBOARD_IP, TELEMETRY_UDP_PORT);
  telemetryUdp.print(out);
  telemetryUdp.endPacket();
}

// ---------------- Web page (embedded, no filesystem upload needed) ----------------
const char INDEX_HTML[] PROGMEM = R"HTMLPAGE(
<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Icebox Monitor</title>
<script src="https://cdn.jsdelivr.net/npm/chart.js@4"></script>
<style>
  html { font-size: 24px; } /* 1.5x the 16px default - scales every rem-based size below with it */
  :root {
    --bg: #0f1720; --card: #182634; --text: #e8eef4; --muted: #8ea0b3;
    --accent: #3ddc84; --accent2: #ffb020; --blue: #5b9dff; --bad: #ff5c5c;
    --border: #24384a;
  }
  * { box-sizing: border-box; }
  body {
    margin: 0; padding: 36px; background: var(--bg); color: var(--text);
    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Arial, sans-serif;
  }
  h1 { font-size: 1.3rem; font-weight: 600; margin: 0 0 30px 0; text-align: center; }
  .wrap { max-width: 720px; margin: 0 auto; }

  .card { background: var(--card); border: 1px solid var(--border); border-radius: 14px; padding: 30px 33px; }
  .card h2 { margin: 0 0 4px 0; font-size: 1.05rem; display: flex; align-items: center; gap: 12px; }
  .dot { width: 14px; height: 14px; border-radius: 50%; background: var(--muted); display: inline-block; }
  .dot.ok { background: var(--accent); }
  .dot.stale { background: var(--bad); }
  .sub-line { color: var(--muted); font-size: 0.8rem; margin-bottom: 21px; }
  .rows { display: grid; grid-template-columns: 1fr auto; row-gap: 15px; column-gap: 18px; }
  .rows .label { color: var(--muted); font-size: 0.9rem; }
  .rows .value { font-weight: 600; font-size: 1rem; text-align: right; font-variant-numeric: tabular-nums; }
  .value.big { font-size: 1.4rem; color: var(--accent); }
  .value.on  { color: var(--accent); }
  .value.off { color: var(--muted); }
  canvas { background: var(--card); border: 1px solid var(--border); border-radius: 14px; padding: 18px; margin-top: 27px; max-width: 100%; }
  .footer { text-align: center; color: var(--muted); font-size: 0.75rem; margin-top: 36px; }
  #err { color: var(--bad); text-align: center; font-size: 0.85rem; margin-bottom: 18px; }
</style>
</head>
<body>
<div class="wrap">
  <h1>&#10052;&#65039; Icebox Monitor</h1>
  <div id="err"></div>

  <div class="card">
    <h2><span class="dot" id="statusDot"></span>Icebox</h2>
    <div class="rows">
      <div class="label">Fridge</div><div class="value big" id="fridge">&mdash;</div>
      <div class="label">Crisper</div><div class="value big" id="crisper">&mdash;</div>
      <div class="label">Compressor</div><div class="value" id="compressor">&mdash;</div>
      <div class="label">Duty Cycle</div><div class="value big" id="duty">&mdash;</div>
      <div class="label">Cycles</div><div class="value" id="cycles">&mdash;</div>
      <div class="label">Wi-Fi signal</div><div class="value" id="rssi">&mdash;</div>
    </div>
  </div>

  <canvas id="tempChart" height="135"></canvas>
  <canvas id="dutyChart" height="135"></canvas>

  <div class="footer">Auto-refreshing every 5s &middot; <span id="sys-stats">uptime &mdash;</span></div>
</div>

<script>
let tempChart, dutyChart;

function fmtTime(ts) {
  const d = new Date(ts * 1000);
  return d.toLocaleTimeString([], {hour:'2-digit', minute:'2-digit'});
}

function fmtUptime(seconds) {
  const d = Math.floor(seconds / 86400);
  const h = Math.floor((seconds % 86400) / 3600);
  const m = Math.floor((seconds % 3600) / 60);
  if (d > 0) return d + "d " + h + "h";
  if (h > 0) return h + "h " + m + "m";
  return m + "m";
}

async function refreshLive() {
  try {
    const r = await fetch('/api/live');
    const j = await r.json();
    document.getElementById('fridge').textContent = j.fridge.toFixed(1) + ' \u00b0C';
    document.getElementById('crisper').textContent = j.crisper.toFixed(1) + ' \u00b0C';
    const c = document.getElementById('compressor');
    c.textContent = j.compressor ? 'ON' : 'OFF';
    c.className = 'value ' + (j.compressor ? 'on' : 'off');
    document.getElementById('duty').textContent = j.duty + ' %';
    document.getElementById('cycles').textContent = j.cycles;
    document.getElementById('rssi').textContent = j.rssi + ' dBm';

    // Heap health - comparing against the boot-time baseline is what
    // actually tells a leak apart from a stable-but-tight baseline: a
    // steadily widening gap from "boot" here means a real leak; a gap
    // that opens once early on and then holds flat is just normal
    // settling, not an ongoing problem.
    const uptimeStr = fmtUptime(j.uptime);
    const freeKb = (j.freeHeap / 1024).toFixed(0);
    const bootKb = (j.bootFreeHeap / 1024).toFixed(0);
    const maxAllocKb = (j.maxAllocHeap / 1024).toFixed(0);
    document.getElementById('sys-stats').textContent =
      `uptime ${uptimeStr} \u00b7 heap ${bootKb}kB at boot \u2192 ${freeKb}kB now \u00b7 ${maxAllocKb}kB largest block`;

    const dot = document.getElementById('statusDot');
    dot.classList.add('ok');
    dot.classList.remove('stale');
    document.getElementById('err').textContent = '';
  } catch (e) {
    const dot = document.getElementById('statusDot');
    dot.classList.remove('ok');
    dot.classList.add('stale');
    document.getElementById('err').textContent = 'Connection error';
  }
}

async function refreshHistory() {
  try {
    const r = await fetch('/api/history');
    const j = await r.json();
    const labels = j.map(p => fmtTime(p.t));
    const fridge = j.map(p => p.f);
    const crisper = j.map(p => p.c);

    // The board records one duty% point per minute, but a fridge
    // compressor typically cycles over 10-30+ minutes - at 1-minute
    // resolution, almost every point lands near 0% or 100% (it's rarely
    // mid-cycle exactly when a given minute's window closes), which
    // reads as a plain on/off square wave rather than a meaningful duty
    // curve. Smoothing over a trailing window long enough to span a
    // real chunk of a cycle turns it into an actual duty percentage
    // trend. This only affects what gets drawn - the board still stores
    // (and /api/history still returns) the real per-minute values.
    const DUTY_SMOOTHING_WINDOW = 15; // minutes
    function movingAverage(values, windowSize) {
      const out = new Array(values.length);
      let sum = 0;
      for (let i = 0; i < values.length; i++) {
        sum += values[i];
        if (i >= windowSize) sum -= values[i - windowSize];
        const n = Math.min(i + 1, windowSize);
        out[i] = sum / n;
      }
      return out;
    }
    const duty = movingAverage(j.map(p => p.d), DUTY_SMOOTHING_WINDOW);

    if (!tempChart) {
      tempChart = new Chart(document.getElementById('tempChart'), {
        type: 'line',
        data: { labels, datasets: [
          { label: 'Fridge °C', data: fridge, borderColor:'#5b9dff', tension:0.2, pointRadius:0 },
          { label: 'Crisper °C', data: crisper, borderColor:'#ffb020', tension:0.2, pointRadius:0 }
        ]},
        options: { responsive:true, animation:false,
          scales: { x: { ticks: { color:'#8ea0b3', maxTicksLimit:12 } }, y: { ticks: { color:'#8ea0b3' } } },
          plugins: { legend: { labels: { color:'#e8eef4' } } } }
      });
      dutyChart = new Chart(document.getElementById('dutyChart'), {
        type: 'line',
        data: { labels, datasets: [
          { label: 'Duty cycle % (15min avg)', data: duty, borderColor:'#3ddc84', backgroundColor:'rgba(61,220,132,0.15)', fill:true, tension:0.3, pointRadius:0 }
        ]},
        options: { responsive:true, animation:false,
          scales: { x: { ticks: { color:'#8ea0b3', maxTicksLimit:12 } }, y: { ticks: { color:'#8ea0b3' }, min:0, max:100 } },
          plugins: { legend: { labels: { color:'#e8eef4' } } } }
      });
    } else {
      tempChart.data.labels = labels;
      tempChart.data.datasets[0].data = fridge;
      tempChart.data.datasets[1].data = crisper;
      tempChart.update();
      dutyChart.data.labels = labels;
      dutyChart.data.datasets[0].data = duty;
      dutyChart.update();
    }
  } catch (e) {
    document.getElementById('err').textContent = 'History graph error: ' + e.message;
  }
}

refreshLive();
refreshHistory();
setInterval(refreshLive, 5000);
setInterval(refreshHistory, 60000);
</script>
</body>
</html>
)HTMLPAGE";

// ---------------- Routes ----------------
void setupRoutes() {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send_P(200, "text/html", INDEX_HTML);
  });

  server.on("/api/live", HTTP_GET, [](AsyncWebServerRequest *request) {
    JsonDocument doc;
    doc["fridge"]     = isnan(fridgeTempC) ? -127 : fridgeTempC;
    doc["crisper"]    = isnan(crisperTempC) ? -127 : crisperTempC;
    doc["compressor"] = compressorOnDebounced;
    doc["duty"]       = dutyPercentLastWindow();
    doc["cycles"]     = compressorCycles;
    doc["rssi"]       = WiFi.RSSI();
    doc["uptime"]     = millis() / 1000;
    doc["time"]       = nowTimestamp();
    doc["freeHeap"]      = ESP.getFreeHeap();
    doc["minFreeHeap"]   = ESP.getMinFreeHeap();
    doc["maxAllocHeap"]  = ESP.getMaxAllocHeap();
    doc["bootFreeHeap"]  = bootFreeHeap;
    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
  });

  server.on("/api/history", HTTP_GET, [](AsyncWebServerRequest *request) {
    // Streamed instead of built as one big JsonDocument+String - at up to
    // 1440 points, that was a single ~40-70KB contiguous allocation that
    // got harder to satisfy the longer the board had been up and the
    // more fragmented the heap got, which is exactly what was making the
    // graphs silently stop appearing after some hours of uptime. Writing
    // each point directly to the response as it's built needs only a
    // small buffer at a time, regardless of how many points there are.
    AsyncResponseStream *response = request->beginResponseStream("application/json");
    response->print('[');
    int start = (historyHead - historyCount + HISTORY_SIZE) % HISTORY_SIZE;
    for (int i = 0; i < historyCount; i++) {
      int idx = (start + i) % HISTORY_SIZE;
      if (i > 0) response->print(',');
      response->printf("{\"t\":%u,\"f\":%.2f,\"c\":%.2f,\"d\":%u}",
                        history[idx].timestamp, history[idx].fridgeTemp,
                        history[idx].crisperTemp, history[idx].dutyPercent);
    }
    response->print(']');
    request->send(response);
  });
}

void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(COMPRESSOR_SENSE_PIN, INPUT_PULLUP);

  fridgeSensor.begin();
  crisperSensor.begin();
  fridgeSensor.setWaitForConversion(false);
  crisperSensor.setWaitForConversion(false);

  WiFi.mode(WIFI_STA);
  if (!WiFi.config(local_IP, gateway, subnet, dns1, dns2)) {
    Serial.println("Static IP config failed");
  }
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");
  // Bounded wait, not an infinite loop - if the router isn't up yet, reboot
  // and try the whole boot sequence again rather than hanging forever with
  // nothing running (same fix applied to the Victron dashboard board).
  const unsigned long WIFI_CONNECT_TIMEOUT_MS = 60000UL;
  unsigned long wifiWaitStart = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - wifiWaitStart > WIFI_CONNECT_TIMEOUT_MS) {
      Serial.println("\nWiFi didn't connect within 60s - restarting to retry.");
      delay(200);
      ESP.restart();
    }
    delay(400);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  configTime(GMT_OFFSET_SEC, DST_OFFSET_SEC, NTP_SERVER);

  setupRoutes();
  server.begin();

  telemetryUdp.begin(0); // ephemeral local port; we only ever send
  Serial.printf("Telemetry UDP -> %s:%u every %lus\n",
                DASHBOARD_IP.toString().c_str(), TELEMETRY_UDP_PORT, TELEMETRY_INTERVAL_MS / 1000);

  unsigned long now = millis();
  lastLoopMs = now;
  windowStartMs = now;
  lastTempReadAt = now - TEMP_READ_INTERVAL_MS; // force first read immediately
  lastHistoryPushAt = now;
  lastTelemetryAt = now;

  startTempConversion();

  // ---- Task watchdog: auto-reboot if loop() ever gets stuck ----
  // Same reasoning as the Victron dashboard board: if a single pass
  // through loop() ever takes longer than this, something's genuinely
  // wedged, and this forces a reboot rather than needing someone to
  // notice the data's gone missing and power-cycle it.
  esp_task_wdt_deinit(); // clear any watchdog the Arduino core already set up, so init below doesn't fail
  esp_task_wdt_config_t wdtConfig = {
    .timeout_ms = 20000,
    .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
    .trigger_panic = true
  };
  esp_task_wdt_init(&wdtConfig);
  esp_task_wdt_add(NULL);
  Serial.println("Task watchdog armed (20s).");

  // Snapshot free heap right after everything's up, so later readings can
  // be compared against a real starting point - the only way to tell a
  // genuine leak (steadily dropping from here) apart from a stable-but-
  // tight baseline, and to catch the kind of large-allocation failure
  // that was silently breaking /api/history before it happens again.
  bootFreeHeap = ESP.getFreeHeap();
  Serial.printf("Boot free heap: %u bytes\n", (unsigned)bootFreeHeap);
}

void loop() {
  esp_task_wdt_reset(); // feed the watchdog - must happen every pass through loop()
  maintainWiFi();
  updateCompressorSensing();

  unsigned long now = millis();

  if (tempConversionInProgress && (now - tempRequestedAt >= 800)) {
    finishTempConversion();
  }

  if (!tempConversionInProgress && (now - lastTempReadAt >= TEMP_READ_INTERVAL_MS)) {
    lastTempReadAt = now;
    startTempConversion();
  }

  if (now - lastHistoryPushAt >= HISTORY_INTERVAL_MS) {
    lastHistoryPushAt = now;
    pushHistoryPoint();
  }

  if (now - lastTelemetryAt >= TELEMETRY_INTERVAL_MS) {
    lastTelemetryAt = now;
    broadcastTelemetry();
  }
}
