#include <Preferences.h>
#include <esp_task_wdt.h>

#include "settings.h"

namespace settings {

static Preferences   prefs;
static Config        cfg;
static const char*   NS = "pond";   // NVS namespace

static void loadFromNvs() {
    prefs.begin(NS, /*readOnly=*/true);
    cfg.wifi_ssid = prefs.getString("ssid", "");
    cfg.wifi_pass = prefs.getString("pass", "");
    cfg.graf_url  = prefs.getString("url",  "");
    cfg.graf_id   = prefs.getString("id",   "");
    cfg.graf_tok  = prefs.getString("tok",  "");
    prefs.end();
}

static void saveToNvs() {
    prefs.begin(NS, /*readOnly=*/false);
    prefs.putString("ssid", cfg.wifi_ssid);
    prefs.putString("pass", cfg.wifi_pass);
    prefs.putString("url",  cfg.graf_url);
    prefs.putString("id",   cfg.graf_id);
    prefs.putString("tok",  cfg.graf_tok);
    prefs.end();
}

static void clearNvs() {
    prefs.begin(NS, /*readOnly=*/false);
    prefs.clear();
    prefs.end();
    cfg = Config();
}

void begin() { loadFromNvs(); }

bool provisioned() {
    return cfg.wifi_ssid.length() && cfg.graf_url.length() &&
           cfg.graf_id.length()   && cfg.graf_tok.length();
}

const Config& get() { return cfg; }

// --- Console --------------------------------------------------------------
static String mask(const String& s) {
    if (s.length() <= 4) return "****";
    return s.substring(0, 2) + "..." + s.substring(s.length() - 2);
}

static void printStatus() {
    Serial.println(F("\n--- config (NVS) ---"));
    Serial.printf("  ssid : %s\n", cfg.wifi_ssid.c_str());
    Serial.printf("  pass : %s\n", cfg.wifi_pass.length() ? mask(cfg.wifi_pass).c_str() : "(unset)");
    Serial.printf("  url  : %s\n", cfg.graf_url.c_str());
    Serial.printf("  id   : %s\n", cfg.graf_id.c_str());
    Serial.printf("  tok  : %s\n", cfg.graf_tok.length() ? mask(cfg.graf_tok).c_str() : "(unset)");
    Serial.printf("  provisioned: %s\n", provisioned() ? "yes" : "NO");
}

static void printHelp() {
    Serial.print(F(
        "\n=== pond-level provisioning ===\n"
        "  ssid <value>    WiFi SSID\n"
        "  pass <value>    WiFi password\n"
        "  url <value>     Grafana OTLP URL\n"
        "  id <value>      Grafana instance id\n"
        "  token <value>   Grafana API token\n"
        "  show            print current values (secrets masked)\n"
        "  save            persist to NVS\n"
        "  clear           erase all stored config\n"
        "  exit            leave console\n"));
}

// Blocking line read; feeds the watchdog while waiting.
static String readLine() {
    String line;
    for (;;) {
        // Only pet the WDT if this task is actually subscribed. During boot
        // provisioning the loop task isn't added yet, and calling reset()
        // then floods the log with "task not found".
        if (esp_task_wdt_status(NULL) == ESP_OK) esp_task_wdt_reset();
        while (Serial.available()) {
            char c = (char)Serial.read();
            if (c == '\r') continue;
            if (c == '\n') { line.trim(); return line; }
            if (line.length() < 512) line += c;
        }
        delay(10);
    }
}

// Apply one command line. Returns false on `exit`.
static bool apply(const String& line, bool& dirty) {
    int sp = line.indexOf(' ');
    String cmd = (sp < 0) ? line : line.substring(0, sp);
    String arg = (sp < 0) ? ""   : line.substring(sp + 1);
    cmd.toLowerCase();
    arg.trim();

    if      (cmd == "ssid")  { cfg.wifi_ssid = arg; dirty = true; }
    else if (cmd == "pass")  { cfg.wifi_pass = arg; dirty = true; }
    else if (cmd == "url")   { cfg.graf_url  = arg; dirty = true; }
    else if (cmd == "id")    { cfg.graf_id   = arg; dirty = true; }
    else if (cmd == "token") { cfg.graf_tok  = arg; dirty = true; }
    else if (cmd == "show")  { printStatus(); }
    else if (cmd == "save")  { saveToNvs(); dirty = false; Serial.println(F("[saved to NVS]")); }
    else if (cmd == "clear") { clearNvs();  dirty = false; Serial.println(F("[cleared]")); }
    else if (cmd == "exit")  { return false; }
    else if (cmd == "help" || cmd.isEmpty()) { printHelp(); }
    else { Serial.printf("? unknown '%s' (type help)\n", cmd.c_str()); }
    return true;
}

void runConsole() {
    printHelp();
    printStatus();
    bool dirty = false;
    for (;;) {
        Serial.print(F("\nconfig> "));
        String line = readLine();
        Serial.println(line);
        if (!apply(line, dirty)) break;
    }
    if (dirty) Serial.println(F("[warning] unsaved changes were NOT persisted (use 'save')"));
}

void pollSerial() {
    if (!Serial.available()) return;
    String line = Serial.readStringUntil('\n');
    line.trim();
    if (line == "config" || line == "menu") runConsole();
}

}  // namespace settings
