/**
 * XTeInk X4 Terminal
 *
 * Native Arduino/PlatformIO firmware for using the XTeInk X4 as an e-ink
 * terminal display.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <GxEPD2_BW.h>

// ============================================================================
// CONFIGURATION
// ============================================================================

const char* AP_NAME = "X4-Terminal-Setup";
const int HTTP_PORT = 80;
const bool ENABLE_DISPLAY = true;

// ============================================================================
// HARDWARE PINS (XTeInk X4)
// ============================================================================

#define EPD_SCLK  8
#define EPD_MOSI  10
#define EPD_MISO  9
#define EPD_CS    21
#define EPD_DC    4
#define EPD_RST   5
#define EPD_BUSY  6
#define BATTERY_PIN 0

// ============================================================================
// DISPLAY
// ============================================================================

GxEPD2_BW<GxEPD2_426_GDEQ0426T82, GxEPD2_426_GDEQ0426T82::HEIGHT> display(
    GxEPD2_426_GDEQ0426T82(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY)
);

// The X4 panel is 800x480. Text size 2 is the smallest readable terminal size.
const uint16_t DISPLAY_WIDTH = 800;
const uint16_t DISPLAY_HEIGHT = 480;
const uint8_t TEXT_SCALE = 2;
const uint8_t CHAR_WIDTH = 6 * TEXT_SCALE;
const uint8_t CHAR_HEIGHT = 8 * TEXT_SCALE;
const uint8_t LEFT_MARGIN = 4;
const uint8_t TOP_MARGIN = 16;
const uint8_t MAX_COLS = (DISPLAY_WIDTH - LEFT_MARGIN * 2) / CHAR_WIDTH;
const uint8_t MAX_ROWS = (DISPLAY_HEIGHT - TOP_MARGIN) / CHAR_HEIGHT;

// ============================================================================
// STATE
// ============================================================================

WebServer server(HTTP_PORT);

String rows[MAX_ROWS];
uint8_t rowCount = 0;
uint16_t reportedCols = 0;
uint16_t cursorX = 0;
uint16_t cursorY = 0;
bool frameDirty = true;
bool frameReceived = false;
bool displayReady = false;
bool fullRefreshNeeded = true;
uint8_t dirtyMinRow = 0;
uint8_t dirtyMaxRow = MAX_ROWS - 1;
unsigned long lastFrameAt = 0;
unsigned long lastDisplayUpdate = 0;

const unsigned long MIN_FULL_REFRESH_INTERVAL = 1800;
const unsigned long MIN_PARTIAL_REFRESH_INTERVAL = 450;
const unsigned long WIFI_CHECK_INTERVAL = 180000;
unsigned long lastWiFiCheck = 0;

// ============================================================================
// HELPERS
// ============================================================================

int readBatteryPercent() {
    int raw = analogRead(BATTERY_PIN);
    float voltage = raw * 2.0f * 3.3f / 4095.0f;
    int percent = (int)((voltage - 3.0f) / 1.2f * 100.0f);
    return constrain(percent, 0, 100);
}

void showMessage(const char* line1, const char* line2 = nullptr, const char* line3 = nullptr) {
    if (!displayReady) return;

    display.setFullWindow();
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);
        display.setTextColor(GxEPD_BLACK);
        display.setTextSize(2);
        display.setCursor(24, 180);
        display.print(line1);
        if (line2) {
            display.setCursor(24, 220);
            display.print(line2);
        }
        if (line3) {
            display.setTextSize(1);
            display.setCursor(24, 260);
            display.print(line3);
        }
    } while (display.nextPage());
}

String limitLine(const String& input) {
    String line = input;
    line.replace("\r", "");
    line.replace("\t", "    ");
    if (line.length() > MAX_COLS) {
        line = line.substring(0, MAX_COLS);
    }
    return line;
}

void markDirtyRow(uint8_t row) {
    if (row >= MAX_ROWS) return;
    if (!frameDirty) {
        dirtyMinRow = row;
        dirtyMaxRow = row;
    } else {
        dirtyMinRow = min(dirtyMinRow, row);
        dirtyMaxRow = max(dirtyMaxRow, row);
    }
    frameDirty = true;
}

void drawTerminalRows(uint8_t firstRow, uint8_t lastRow) {
    display.setTextColor(GxEPD_BLACK);
    display.setTextSize(TEXT_SCALE);
    display.setFont(nullptr);
    display.setTextWrap(false);

    for (uint8_t row = firstRow; row <= lastRow && row < MAX_ROWS; row++) {
        int y = TOP_MARGIN + row * CHAR_HEIGHT;
        display.setCursor(LEFT_MARGIN, y);
        if (frameReceived && row < rowCount) {
            display.print(rows[row]);
        }
    }

    if (frameReceived && cursorY >= firstRow && cursorY <= lastRow && cursorY < MAX_ROWS && cursorX < MAX_COLS) {
        int x = LEFT_MARGIN + cursorX * CHAR_WIDTH;
        int y = TOP_MARGIN + cursorY * CHAR_HEIGHT - CHAR_HEIGHT + 2;
        display.drawRect(x, y, CHAR_WIDTH, CHAR_HEIGHT, GxEPD_BLACK);
    }
}

bool applyFrameJson(JsonDocument& doc) {
    JsonArray incomingRows = doc["rows"].as<JsonArray>();
    if (incomingRows.isNull()) return false;

    String nextRows[MAX_ROWS];
    uint8_t nextRowCount = 0;
    for (JsonVariant value : incomingRows) {
        if (nextRowCount >= MAX_ROWS) break;
        nextRows[nextRowCount++] = limitLine(value.as<String>());
    }

    uint16_t nextCursorX = cursorX;
    uint16_t nextCursorY = cursorY;
    JsonArray cursor = doc["cursor"].as<JsonArray>();
    if (!cursor.isNull() && cursor.size() >= 2) {
        nextCursorX = min((uint16_t)(cursor[0] | 0), (uint16_t)(MAX_COLS - 1));
        nextCursorY = min((uint16_t)(cursor[1] | 0), (uint16_t)(MAX_ROWS - 1));
    }

    bool changed = !frameReceived || rowCount != nextRowCount;
    uint8_t oldRowCount = rowCount;
    uint16_t oldCursorX = cursorX;
    uint16_t oldCursorY = cursorY;

    if (!frameReceived) {
        fullRefreshNeeded = true;
    } else {
        uint8_t compareRows = max(oldRowCount, nextRowCount);
        for (uint8_t i = 0; i < compareRows && i < MAX_ROWS; i++) {
            String oldLine = (i < oldRowCount) ? rows[i] : "";
            String newLine = (i < nextRowCount) ? nextRows[i] : "";
            if (oldLine != newLine) {
                markDirtyRow(i);
                changed = true;
            }
        }
    }

    if (frameReceived && (oldCursorX != nextCursorX || oldCursorY != nextCursorY)) {
        markDirtyRow((uint8_t)oldCursorY);
        markDirtyRow((uint8_t)nextCursorY);
        changed = true;
    }

    reportedCols = doc["cols"] | 0;
    rowCount = nextRowCount;
    for (uint8_t i = 0; i < nextRowCount; i++) {
        rows[i] = nextRows[i];
    }
    cursorX = nextCursorX;
    cursorY = nextCursorY;

    frameReceived = true;
    if (fullRefreshNeeded) {
        frameDirty = true;
        dirtyMinRow = 0;
        dirtyMaxRow = MAX_ROWS - 1;
    } else if (changed && !frameDirty) {
        dirtyMinRow = 0;
        dirtyMaxRow = min((uint8_t)(MAX_ROWS - 1), nextRowCount);
        frameDirty = true;
    }
    lastFrameAt = millis();
    return true;
}

void drawTerminal(bool force = false) {
    if (!displayReady) return;
    if (!frameDirty && !force) return;

    unsigned long now = millis();
    bool useFullRefresh = force || fullRefreshNeeded || !frameReceived;
    unsigned long minInterval = useFullRefresh ? MIN_FULL_REFRESH_INTERVAL : MIN_PARTIAL_REFRESH_INTERVAL;
    if (!force && now - lastDisplayUpdate < minInterval) return;
    lastDisplayUpdate = now;

    uint8_t firstRow = useFullRefresh ? 0 : dirtyMinRow;
    uint8_t lastRow = useFullRefresh ? (MAX_ROWS - 1) : dirtyMaxRow;
    uint16_t partialY = max(0, TOP_MARGIN + firstRow * CHAR_HEIGHT - CHAR_HEIGHT);
    uint16_t partialH = min((uint16_t)(DISPLAY_HEIGHT - partialY), (uint16_t)((lastRow - firstRow + 2) * CHAR_HEIGHT + 4));

    if (useFullRefresh) {
        display.setFullWindow();
    } else {
        display.setPartialWindow(0, partialY, DISPLAY_WIDTH, partialH);
    }

    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);

        if (!frameReceived) {
            display.setTextSize(2);
            display.setCursor(24, 180);
            display.print("X4 Terminal");
            display.setTextSize(1);
            display.setCursor(24, 220);
            display.print("IP: ");
            display.print(WiFi.localIP());
            display.setCursor(24, 240);
            display.print("Waiting for frames...");
            display.setCursor(24, 260);
            display.print("POST /frame");
        } else {
            drawTerminalRows(firstRow, lastRow);
        }
    } while (display.nextPage());

    frameDirty = false;
    fullRefreshNeeded = false;
    dirtyMinRow = MAX_ROWS - 1;
    dirtyMaxRow = 0;
}

void sendJsonStatus() {
    JsonDocument doc;
    doc["ok"] = true;
    doc["ip"] = WiFi.localIP().toString();
    doc["port"] = HTTP_PORT;
    doc["cols"] = MAX_COLS;
    doc["rows"] = MAX_ROWS;
    doc["battery"] = readBatteryPercent();
    doc["lastFrameMs"] = lastFrameAt;

    String body;
    serializeJson(doc, body);
    server.send(200, "application/json", body);
}

// ============================================================================
// HTTP HANDLERS
// ============================================================================

void handleRoot() {
    String body = "X4 Terminal\n\n";
    body += "POST /frame with JSON rows to update the display.\n";
    body += "Status: /status\n";
    body += "Reset WiFi: /reset-wifi\n";
    body += "IP: ";
    body += WiFi.localIP().toString();
    body += "\n";
    server.send(200, "text/plain", body);
}

void handleStatus() {
    sendJsonStatus();
}

void handleFrame() {
    if (!server.hasArg("plain")) {
        server.send(400, "text/plain", "Missing JSON body\n");
        return;
    }

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, server.arg("plain"));
    if (error) {
        server.send(400, "text/plain", String("JSON error: ") + error.c_str() + "\n");
        return;
    }

    if (!applyFrameJson(doc)) {
        server.send(400, "text/plain", "Expected rows array\n");
        return;
    }
    server.send(204, "text/plain", "");
}

void handleResetWiFi() {
    server.send(200, "text/plain", "Clearing WiFi settings and rebooting...\n");
    delay(250);
    WiFiManager wifiManager;
    wifiManager.resetSettings();
    ESP.restart();
}

void handleNotFound() {
    server.send(404, "text/plain", "Not found\n");
}

void startServer() {
    server.on("/", HTTP_GET, handleRoot);
    server.on("/status", HTTP_GET, handleStatus);
    server.on("/frame", HTTP_POST, handleFrame);
    server.on("/reset-wifi", HTTP_GET, handleResetWiFi);
    server.onNotFound(handleNotFound);
    server.begin();

    Serial.print("HTTP server listening at http://");
    Serial.print(WiFi.localIP());
    Serial.println("/");
}

void ensureWiFi() {
    if (WiFi.status() == WL_CONNECTED) return;

    Serial.println("WiFi disconnected; starting config portal");
    showMessage("WiFi Lost", "Connect to:", AP_NAME);

    WiFiManager wifiManager;
    wifiManager.setConfigPortalTimeout(300);
    if (!wifiManager.startConfigPortal(AP_NAME)) {
        Serial.println("Config portal timed out; rebooting");
        ESP.restart();
    }

    Serial.print("WiFi reconnected: ");
    Serial.println(WiFi.localIP());
    frameReceived = false;
    frameDirty = true;
    startServer();
}

// ============================================================================
// SETUP / LOOP
// ============================================================================

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n\nXTeInk X4 Terminal");
    Serial.println("========================");

    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);

    WiFiManager wifiManager;
    wifiManager.setConfigPortalTimeout(300);
    showMessage("WiFi Setup", "Connect to:", AP_NAME);

    if (!wifiManager.autoConnect(AP_NAME)) {
        Serial.println("WiFi setup failed; rebooting");
        showMessage("WiFi Failed", "Rebooting...", nullptr);
        delay(3000);
        ESP.restart();
    }

    Serial.print("WiFi connected: ");
    Serial.println(WiFi.localIP());
    WiFi.setSleep(false);
    startServer();

    if (ENABLE_DISPLAY) {
        SPI.begin(EPD_SCLK, EPD_MISO, EPD_MOSI, EPD_CS);
        display.init(115200, true, 2, false, SPI, SPISettings(4000000, MSBFIRST, SPI_MODE0));
        display.setRotation(0);
        display.setTextWrap(false);
        displayReady = true;
        Serial.println("Display initialized");
        drawTerminal(true);
    }
}

void loop() {
    server.handleClient();

    if (millis() - lastWiFiCheck > WIFI_CHECK_INTERVAL) {
        lastWiFiCheck = millis();
        ensureWiFi();
    }

    drawTerminal();
    delay(5);
}
