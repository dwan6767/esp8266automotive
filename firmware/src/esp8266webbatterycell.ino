/*
  Auto Master — ESP8266 Battery Dashboard (Final)
  Author: Dwaipayan Shikari
  -----------------------------------------------
  - Runs ESP8266 in AP mode and serves a responsive web dashboard
  - Reads battery data from MCP2515 (8 MHz crystal only)
  - Falls back to realistic simulation if MCP2515 is not present
  - Supports up to 32 cells, adjustable from the web UI
  - Allows setting a pack current (A) which is persisted and sent via CAN
  - Saves preferences to EEPROM (cell count and current)
  - Hardware mapping below (as requested):
      MCP2515 CS   -> GPIO16 (D0)
      MCP2515 INT  -> GPIO4  (D2)
      MCP2515 SPI  -> HSPI (D5=CLK, D6=MISO, D7=MOSI)
      MCP2515 Crystal: 8 MHz (only)
  - Default CAN baud: 250 kbps
  - Beautifully commented and organized for readability
*/

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <SPI.h>
#include <mcp_can.h>
#include <EEPROM.h>

// ---------------- CONFIG ----------------
#define MAX_CELLS 32
#define DEFAULT_CELLS 8
#define UPDATE_MS 200            // frontend refresh (ms)
#define CAN_POLL_MS 200          // periodic drain interval (ms)
#define EEPROM_SIZE 64

// Hardware pins per final mapping
#define CAN_CS_PIN 16            // D0 (GPIO16) -> MCP2515 CS (user-specified)
#define CAN_INT_PIN 4            // D2 (GPIO4)  -> MCP2515 INT

// CAN bus config
const uint32_t CAN_BUS_SPEED = CAN_250KBPS; // default CAN speed (250kbps)

// CAN set-current placeholders (frame format is placeholder; adapt to BMS)
const uint32_t CAN_SET_ID = 0x01;
const uint8_t CAN_SET_FRAME_TYPE = 0x30;

// EEPROM layout
const uint8_t EEPROM_MAGIC = 0xA5;
const int EEPROM_ADDR_COUNT = 1;    // 1 byte for cell count
const int EEPROM_ADDR_CURRENT = 2;  // float (4 bytes) for saved current

// WiFi AP credentials
const char* AP_SSID = "AutoMaster_AP";
const char* AP_PASS = "12345678";

// ---------------- Globals ----------------
ESP8266WebServer server(80);
MCP_CAN CAN0(CAN_CS_PIN);
bool canPresent = false;

// runtime state
int activeCellCount = DEFAULT_CELLS;
float cells[MAX_CELLS];
float temperatureC = 29.0f;
float balanceCurrentA = 0.0f;
float packVoltageV = 0.0f;
float currentSettingA = 0.0f;

// per-slave rolling index map (for reconstruction of multi-frame cell sequences)
static int nextCellIndexPerSlave[256];

// IRQ flag set by ISR, drained in loop
volatile bool canIrqFlag = false;

// tiny helper
static inline float clampf4(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// ---------------- Simulation helpers ----------------
void initSimData() {
  for (int i = 0; i < MAX_CELLS; i++) cells[i] = 3.90f + 0.06f * (i % 3);
  temperatureC = 29.0f;
  balanceCurrentA = 0.0f;
  packVoltageV = 0.0f;
  currentSettingA = 0.0f;
  for (int i = 0; i < 256; i++) nextCellIndexPerSlave[i] = 0;
}

void tickSimData() {
  // small random walk per cell (±3 mV)
  for (int i = 0; i < activeCellCount; i++) {
    cells[i] += (random(-3, 4)) * 0.001f;
    cells[i] = clampf4(cells[i], 3.60f, 4.20f);
  }
  // small temperature jitter
  temperatureC += (random(-2, 3)) * 0.05f;
  temperatureC = clampf4(temperatureC, 20.0f, 55.0f);
}

// ---------------- Stats ----------------
void computeStats(float& total, float& avg, float& vmin, float& vmax) {
  total = 0.0f;
  vmin = 100.0f; vmax = 0.0f;
  for (int i = 0; i < activeCellCount; i++) {
    total += cells[i];
    if (cells[i] < vmin) vmin = cells[i];
    if (cells[i] > vmax) vmax = cells[i];
  }
  avg = (activeCellCount > 0) ? (total / activeCellCount) : 0.0f;
}

// ---------------- HTML (preserved UI + simulated hardware reminder) ----------------
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Auto Master — Battery</title>
<style>
  :root{
    --bg1:#071021; --bg2:#0b2540; --card:#0d1f36;
    --glass:rgba(255,255,255,0.06); --accent:#00d4ff;
    --good:#49d08a; --warn:#ffb86b; --bad:#ff6b6b; --ink:#eaf9ff;
  }
  *{box-sizing:border-box}
  html,body{height:100%}
  body{
    margin:0; font-family: Inter, system-ui, -apple-system, "Segoe UI", Roboto, Arial;
    background: radial-gradient(1200px 600px at 20% -10%, #12365a22, transparent),
                linear-gradient(135deg, var(--bg1), var(--bg2));
    color:var(--ink); display:flex; align-items:center; justify-content:center; padding:14px;
  }
  .wrap{width:100%; max-width:1080px;}
  header{ display:flex; align-items:center; justify-content:space-between; gap:12px; margin-bottom:12px;}
  .brand{ font-weight:700; letter-spacing:.6px; color:#dff8ff; text-shadow:0 8px 30px rgba(0,212,255,.2)}
  h1 { font-size: 60px; font-weight: bold; color: #58a6ff; }
  .badge{ font-size:12px; color:#9fd9ee; background:rgba(0,0,0,.2); padding:6px 10px; border-radius:999px; }
  .card{
    background: linear-gradient(180deg, rgba(255,255,255,.04), rgba(255,255,255,.02));
    border: 1px solid rgba(255,255,255,.06);
    border-radius:16px; padding:14px; box-shadow:0 10px 32px rgba(0,0,0,.55); backdrop-filter: blur(6px);
  }

  .top{
    display:flex; gap:12px; align-items:stretch; flex-wrap:wrap;
  }
  .stat{
    flex:1 1 210px; background:var(--glass); border-radius:12px; padding:12px;
    display:flex; flex-direction:column; justify-content:center; min-height:96px;
  }
  .stat .k{ font-size:12px; color:#c8eefa; opacity:.9; margin-bottom:6px;}
  .stat .v{ font-size:36px; font-weight:800; color:var(--accent); line-height:1;}
  .stat .s{ font-size:12px; color:#a8d7eb;}

  .cells{
    margin-top:12px; display:grid; gap:10px; grid-template-columns: repeat(auto-fit, minmax(160px, 1fr));
  }
  .cell{
    background:var(--glass); border-radius:12px; padding:10px; position:relative; overflow:hidden;
    transition:transform .12s ease, box-shadow .12s ease;
  }
  .cell:hover{ transform:translateY(-3px); box-shadow:0 10px 24px rgba(0,0,0,.35); }
  .cell .name{ font-size:12px; color:#bfe7f7; margin-bottom:8px; letter-spacing:.3px;}
  .cell .reading{ display:flex; align-items:baseline; gap:6px;}
  .cell .val{ font-size:22px; font-weight:800; color:#fff;}
  .cell .unit{ font-size:12px; color:#b8dff0;}
  .bar{
    height:8px; background:#0b2239; border-radius:999px; margin-top:10px; position:relative;
  }
  .bar > i{
    position:absolute; left:0; top:0; bottom:0; width:40%;
    background: linear-gradient(90deg, #1bd6ff, #64ffda);
    border-radius:999px;
    box-shadow: 0 0 20px rgba(27,214,255,.25);
    transition: width .18s ease;
  }

  .spark {
    margin-top:10px; height:42px; background:#0a1d34; border-radius:10px; padding:6px;
  }
  .spark svg { width:100%; height:100%; display:block }
  .foot{ text-align:center; margin-top:10px; font-size:12px; color:#9fd9ee; }

  /* settings area */
  .settings { display:flex; gap:10px; align-items:center; }
  .settings input[type="number"]{ width:72px; padding:6px; border-radius:6px; border:none; }
  .settings input[type="range"]{ width:160px; }
  .settings button{ padding:6px 10px; border-radius:8px; border:none; background:#0e7db3; color:white; cursor:pointer; }

  /* simulated hardware banner */
  .hw-warning {
    background: #ff4d4d;
    color: white;
    padding:8px 12px;
    border-radius:10px;
    font-weight:700;
    box-shadow: 0 6px 18px rgba(255,77,77,0.14);
    margin-bottom:10px;
  }

  @media (max-width:480px){ .stat .v{font-size:30px} .settings{flex-direction:column;align-items:flex-start} }
</style>
</head>
<body>
  <div class="wrap">
    <header style="flex-direction:column;align-items:flex-start;">
      <div style="display:flex; width:100%; align-items:center; justify-content:space-between;">
        <h1>Auto Master Battery Dashboard</h1>
        <div style="display:flex; gap:8px; align-items:center;">
          <div class="badge">AP mode • %UPDATE_MS%ms</div>
        </div>
      </div>

      <!-- hardware warning banner (shown when no CAN present) -->
      <div id="hwWarning" style="display:none;" class="hw-warning">
        ⚠ Original CAN hardware not detected — showing simulated values.
      </div>

      <!-- settings (kept in header) -->
      <div style="display:flex; gap:10px; margin-top:8px;">
        <div class="settings card" style="background:linear-gradient(180deg, rgba(255,255,255,.02), rgba(255,255,255,.01)); padding:8px;">
          <div style="font-size:12px;color:#c8eefa;margin-bottom:4px;">Cells</div>
          <input id="cellCountInput" type="number" min="1" max="32" value="%NUM_CELLS%">
          <button onclick="setCells()">Set</button>

          <div style="width:12px"></div>

          <div style="font-size:12px;color:#c8eefa;margin-bottom:4px;">Current (A)</div>
          <input id="currentInput" type="number" step="0.1" min="0" max="500" value="0">
          <button onclick="setCurrent()">Apply</button>
        </div>
      </div>
    </header>

    <div class="card">
      <div class="top">
        <div class="stat">
          <div class="k">Total Voltage</div>
          <div id="total" class="v">--.-- V</div>
          <div class="s" id="status">online</div>
        </div>
        <div class="stat">
          <div class="k">Average / Cell</div>
          <div id="avg" class="v">--.-- V</div>
          <div class="s">min <span id="min">--.--</span> • max <span id="max">--.--</span></div>
        </div>

        <div class="stat">
          <div class="k">Max − Min (Δ)</div>
          <div id="delta" class="v">--.-- V</div>
          <div class="s">cell spread</div>
        </div>

        <div class="stat">
          <div class="k">Temperature</div>
          <div id="temp" class="v">--.- °C</div>
          <div class="s">ambient pack</div>
        </div>
      </div>

      <div class="cells" id="cells"></div>

      <div class="spark">
        <svg id="sparkTotal" viewBox="0 0 100 30" preserveAspectRatio="none">
          <path d="" fill="none" stroke="#00d4ff" stroke-width="1.8" stroke-linejoin="round" stroke-linecap="round"/>
        </svg>
      </div>
    </div>

    <div class="foot">Live demo (simulated values). Replace with real CAN/ADC later.</div>
  </div>

<script>
  // ---- front-end config (injected) ----
  const UPDATE_MS = %UPDATE_MS%;
  let VCELL_MIN = 3.60, VCELL_MAX = 4.20;
  let activeCount = %NUM_CELLS%;

  const cellWrap = document.getElementById('cells');
  let cellEls = [];

  function mkCell(i){
    const el = document.createElement('div');
    el.className='cell';
    el.innerHTML = `
      <div class="name">CELL ${i+1}</div>
      <div class="reading"><div class="val" id="v${i}">--.--</div><div class="unit">V</div></div>
      <div class="bar"><i id="b${i}"></i></div>
    `;
    cellWrap.appendChild(el);
    cellEls[i] = {
      v: document.getElementById('v'+i),
      b: document.getElementById('b'+i)
    };
  }

  function rebuildCells(n) {
    activeCount = n;
    cellWrap.innerHTML = '';
    cellEls = [];
    for (let i=0;i<n;i++) mkCell(i);
  }

  // initial build
  rebuildCells(activeCount);

  const totalEl = document.getElementById('total');
  const avgEl   = document.getElementById('avg');
  const minEl   = document.getElementById('min');
  const maxEl   = document.getElementById('max');
  const tempEl  = document.getElementById('temp');
  const statusEl= document.getElementById('status');
  const deltaEl = document.getElementById('delta');
  const hwWarning = document.getElementById('hwWarning');

  // tiny sparkline
  const sparkPath = document.querySelector('#sparkTotal path');
  const sparkBuf = [];
  const SPARK_N = 80;
  function drawSparkline(arr){
    if(arr.length<2){ sparkPath.setAttribute('d',''); return; }
    const min = Math.min(...arr), max = Math.max(...arr);
    const span = (max-min)||1;
    let d = `M 0 ${30 - 30*(arr[0]-min)/span}`;
    for(let i=1;i<arr.length;i++){
      const x = (i/(arr.length-1))*100;
      const y = 30 - 30*(arr[i]-min)/span;
      d += ` L ${x.toFixed(2)} ${y.toFixed(2)}`;
    }
    sparkPath.setAttribute('d', d);
  }

  function colorFor(v){
    if (v < 3.7) return 'var(--bad)';
    if (v < 3.9) return 'var(--warn)';
    return 'var(--good)';
  }

  async function tick(){
    try{
      const r = await fetch('/data', {cache:'no-store'});
      if(!r.ok) throw 0;
      const d = await r.json();

      totalEl.textContent = d.total.toFixed(2) + ' V';
      avgEl.textContent   = d.avg.toFixed(2)   + ' V';
      minEl.textContent   = d.min.toFixed(2);
      maxEl.textContent   = d.max.toFixed(2);
      tempEl.textContent  = d.temp.toFixed(1)  + ' °C';
      statusEl.textContent= d.can ? 'CAN' : 'sim';
      deltaEl.textContent = (d.max - d.min).toFixed(3) + ' V';

      // show or hide hardware warning banner
      hwWarning.style.display = d.can ? 'none' : 'block';

      // if backend cell count changed, rebuild UI
      if (d.count !== activeCount) {
        document.getElementById('cellCountInput').value = d.count;
        rebuildCells(d.count);
      }

      // cells
      for (let i=0;i<d.cells.length && i<cellEls.length;i++){
        const v = d.cells[i];
        const pct = Math.max(0, Math.min(100, (v-VCELL_MIN)/(VCELL_MAX-VCELL_MIN)*100));
        cellEls[i].v.textContent = v.toFixed(3);
        cellEls[i].v.style.color = colorFor(v);
        cellEls[i].b.style.width = pct.toFixed(1)+'%';
      }

      // sparkline (total)
      sparkBuf.push(d.total);
      if (sparkBuf.length > SPARK_N) sparkBuf.shift();
      drawSparkline(sparkBuf);

    }catch(e){
      statusEl.textContent = 'offline';
      hwWarning.style.display = 'none';
    }
  }

  async function setCells(){
    let v = parseInt(document.getElementById('cellCountInput').value) || activeCount;
    if (v < 1) v = 1;
    if (v > 32) v = 32;
    await fetch('/setCells?count=' + v);
  }

  async function setCurrent(){
    let v = parseFloat(document.getElementById('currentInput').value) || 0;
    await fetch('/setCurrent?value=' + v);
  }

  setInterval(tick, UPDATE_MS);
  tick();
</script>
</body>
</html>
)rawliteral";

// ---------------- HTTP handlers ----------------
void handleRoot() {
  // Return the stored HTML page with injected constants
  String page = INDEX_HTML;
  page.replace("%NUM_CELLS%", String(activeCellCount));
  page.replace("%UPDATE_MS%", String(UPDATE_MS));
  server.send(200, "text/html; charset=utf-8", page);
}

void handleData() {
  // Build a compact JSON snapshot for frontend
  float total, avg, vmin, vmax;
  computeStats(total, avg, vmin, vmax);
  float reportedTotal = (packVoltageV > 0.01f) ? packVoltageV : total;
  float diff = vmax - vmin;

  String json;
  json.reserve(160 + activeCellCount * 8);

  // cells array
  json += "{\"cells\":[";
  for (int i = 0; i < activeCellCount; i++) {
    if (i) json += ",";
    json += String(cells[i], 3);
  }
  json += "]";

  // numeric fields
  json += ",\"total\":";
  json += String(reportedTotal, 3);
  json += ",\"avg\":";
  json += String(avg, 3);
  json += ",\"min\":";
  json += String(vmin, 3);
  json += ",\"max\":";
  json += String(vmax, 3);
  json += ",\"diff\":";
  json += String(diff, 3);
  json += ",\"temp\":";
  json += String(temperatureC, 1);

  // meta fields
  json += ",\"count\":";
  json += String(activeCellCount);
  json += ",\"current\":";
  json += String(currentSettingA, 3);

  // boolean must be JSON literal true/false (not quoted)
  json += ",\"can\":";
  json += (canPresent ? "true" : "false");

  json += "}";

  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}

void handleSetCells() {
  if (server.hasArg("count")) {
    int c = server.arg("count").toInt();
    if (c < 1) c = 1;
    if (c > MAX_CELLS) c = MAX_CELLS;
    activeCellCount = c;

    // reset per-slave indices and clear trailing cells
    for (int i = 0; i < 256; i++) nextCellIndexPerSlave[i] = 0;
    for (int i = activeCellCount; i < MAX_CELLS; i++) cells[i] = 0.0f;

    // persist count
    EEPROM.write(0, EEPROM_MAGIC);
    EEPROM.write(EEPROM_ADDR_COUNT, (uint8_t)activeCellCount);
    EEPROM.commit();
  }
  server.send(200, "text/plain", "OK");
}

// ---------------- set current (persist + send CAN) ----------------
void formatCurrentPayload(float currentA, uint8_t out[8]) {
  // Placeholder payload:
  // [slave=0x01, frameType=CAN_SET_FRAME_TYPE, current_mA_low, current_mA_high, ...]
  for (int i = 0; i < 8; i++) out[i] = 0;
  out[0] = 0x01;
  out[1] = CAN_SET_FRAME_TYPE;
  uint32_t mA = (uint32_t) round(currentA * 1000.0f);
  if (mA > 0xFFFF) mA = 0xFFFF;
  out[2] = (uint8_t)(mA & 0xFF);
  out[3] = (uint8_t)((mA >> 8) & 0xFF);
}

void sendCurrentToBMS(float currentA) {
  if (!canPresent) return;
  uint8_t buf[8];
  formatCurrentPayload(currentA, buf);
  if (CAN0.sendMsgBuf(CAN_SET_ID, 0, 8, buf) == CAN_OK) {
    Serial.printf("Sent setCurrent CAN (%.2f A) -> ID 0x%03X\n", currentA, (unsigned)CAN_SET_ID);
  } else {
    Serial.println("Failed to send setCurrent CAN");
  }
}

void handleSetCurrent() {
  if (server.hasArg("value")) {
    float v = server.arg("value").toFloat();
    if (isnan(v)) v = 0.0f;
    currentSettingA = v;
    // persist
    EEPROM.write(0, EEPROM_MAGIC);
    EEPROM.put(EEPROM_ADDR_CURRENT, currentSettingA);
    EEPROM.commit();
    // send CAN command now (placeholder payload)
    sendCurrentToBMS(currentSettingA);
  }
  server.send(200, "text/plain", "OK");
}

// ---------------- CAN parsing (per-slave rolling 3-cell frames) ----------------
void drainCanMessages() {
  if (!canPresent) return;
  unsigned long rxId;
  unsigned char len;
  unsigned char buf[8];

  while (CAN0.checkReceive() == CAN_MSGAVAIL) {
    CAN0.readMsgBuf(&rxId, &len, buf);
    if (len < 2) continue;
    uint8_t slave = buf[0];
    uint8_t frameType = buf[1];

    if (frameType == 0x00) { // 3 cell voltages per frame (16-bit little-endian, mV)
      int idx = nextCellIndexPerSlave[slave];
      if (idx < 0 || idx >= activeCellCount) idx = 0;
      for (int k = 0; k < 3; ++k) {
        int b = 2 + k * 2;
        if (b + 1 >= len) break;
        uint16_t raw = (uint16_t)buf[b] | ((uint16_t)buf[b + 1] << 8);
        float v = raw / 1000.0f; // mV -> V
        if (idx >= 0 && idx < activeCellCount) cells[idx] = v;
        idx++;
        if (idx >= activeCellCount) idx = 0;
      }
      nextCellIndexPerSlave[slave] = idx;
    }
    else if (frameType == 0x08) { // balance current (mA)
      if (len >= 4) {
        uint16_t raw = (uint16_t)buf[2] | ((uint16_t)buf[3] << 8);
        balanceCurrentA = raw / 1000.0f;
      }
    }
    else if (frameType == 0x09) { // pack voltage (10mV units)
      if (len >= 4) {
        uint16_t raw = (uint16_t)buf[2] | ((uint16_t)buf[3] << 8);
        packVoltageV = raw * 0.01f;
      }
    }
    else if (frameType == 0x0A) { // temperature (°C)
      if (len >= 3) {
        temperatureC = (float)buf[2];
      }
    }
    // add additional frameType handling here as needed
  }
}

// ---------------- MCP2515 IRQ handler ----------------
void ICACHE_RAM_ATTR canIsr() {
  // Minimal ISR: set flag and return. Drain happens in loop().
  canIrqFlag = true;
}

// ---------------- Preferences (EEPROM) ----------------
void loadPrefs() {
  if (EEPROM.read(0) == EEPROM_MAGIC) {
    uint8_t c = EEPROM.read(EEPROM_ADDR_COUNT);
    if (c >= 1 && c <= MAX_CELLS) activeCellCount = c;
    EEPROM.get(EEPROM_ADDR_CURRENT, currentSettingA);
    if (isnan(currentSettingA) || currentSettingA < 0) currentSettingA = 0.0f;
  } else {
    // first run: write defaults
    EEPROM.write(0, EEPROM_MAGIC);
    EEPROM.write(EEPROM_ADDR_COUNT, (uint8_t)activeCellCount);
    EEPROM.put(EEPROM_ADDR_CURRENT, currentSettingA);
    EEPROM.commit();
  }
}

// ---------------- Setup & Loop ----------------
void setup() {
  Serial.begin(115200);
  delay(50);
  Serial.println("\nAuto Master — Final (schematic-mapped) — Author: Dwan");

  // EEPROM
  EEPROM.begin(EEPROM_SIZE);

  // seed & init simulation defaults
  randomSeed(ESP.getCycleCount());
  initSimData();
  loadPrefs();

  // Start WiFi AP
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.print("AP SSID: "); Serial.println(AP_SSID);
  Serial.print("AP IP:   "); Serial.println(WiFi.softAPIP());

  // HTTP routes
  server.on("/", HTTP_GET, handleRoot);
  server.on("/data", HTTP_GET, handleData);
  server.on("/setCells", HTTP_GET, handleSetCells);
  server.on("/setCurrent", HTTP_GET, handleSetCurrent);
  server.begin();
  Serial.println("HTTP server started");

  // Configure MCP2515 INT pin
  pinMode(CAN_INT_PIN, INPUT_PULLUP); // MCP2515 INT typically active-low
  attachInterrupt(digitalPinToInterrupt(CAN_INT_PIN), canIsr, FALLING);

  // Attempt MCP2515 init (8 MHz only as requested)
  Serial.println("Initializing MCP2515 (8 MHz)...");
  if (CAN0.begin(MCP_ANY, CAN_BUS_SPEED, MCP_8MHZ) == CAN_OK) {
    CAN0.setMode(MCP_NORMAL);
    canPresent = true;
    Serial.println("MCP2515 Initialized OK (8MHz)!");
  } else {
    canPresent = false;
    Serial.println("Error Initializing MCP2515. Continuing with simulation/demo values.");
  }

  // If CAN present and saved current exists, send it once
  if (canPresent && currentSettingA > 0.0f) {
    sendCurrentToBMS(currentSettingA);
  }
}

void loop() {
  server.handleClient();

  static uint32_t lastPoll = 0;
  uint32_t now = millis();

  // Drain CAN quickly when IRQ occurred
  if (canIrqFlag) {
    canIrqFlag = false;
    drainCanMessages();
    lastPoll = now;
  }

  // Poll periodically (also advances simulation)
  if (now - lastPoll >= CAN_POLL_MS) {
    lastPoll = now;
    if (canPresent) {
      drainCanMessages();
    } else {
      tickSimData();
    }
  }

  yield(); // keep WDT happy
}
