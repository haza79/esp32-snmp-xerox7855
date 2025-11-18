#include <WiFi.h>
#include <WiFiUdp.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <Arduino_SNMP_Manager.h>
#include <Preferences.h>
#include <time.h>

#define CONFIG_BUTTON 0  // Boot button
#define LED 2
unsigned long buttonPressStart = 0;
bool buttonHeld = false;


Preferences prefs;
WebServer server(80);
WiFiUDP udp;
SNMPManager snmp("public");
SNMPGet snmpRequest("public", 1);

bool configMode = false;
unsigned long lastWiFiCheck = 0;
unsigned long lastScheduleCheck = 0;
unsigned long lastCommandCheck = 0;
unsigned long lastPingCheck = 0;
int maxRetries = 3;       // number of retry attempts
int retryDelayMs = 10000;  // delay between retries


//================= CONFIG STRUCT =================//
struct Config {
  char esp32Name[32];
  char ssid[32];
  char password[32];
  char printerIP[16];
  bool firstSetup;
} config;

//================= SNMP DATA =================//
char deviceDescr[64], systemName[64], serialNum[64];
int blackCopy, colorCopy, blackPrint, colorPrint, networkScan, emailScan;
int blackTotal, blackLargeTotal, colorTotal, colorLargeTotal, totalAll;

// SNMP OID List
struct OIDItem {
  const char* oid;
  void* valuePtr;
  bool isString;
};
OIDItem oidList[] = {
  {".1.3.6.1.4.1.253.8.53.3.2.1.2.1", deviceDescr, true},
  {".1.3.6.1.2.1.43.5.1.1.16.1", systemName, true},
  {".1.3.6.1.2.1.43.5.1.1.17.1", serialNum, true},
  {".1.3.6.1.4.1.253.8.53.13.2.1.6.11.20.3", &blackCopy, false},
  {".1.3.6.1.4.1.253.8.53.13.2.1.6.11.20.25", &colorCopy, false},
  {".1.3.6.1.4.1.253.8.53.13.2.1.6.1.20.7", &blackPrint, false},
  {".1.3.6.1.4.1.253.8.53.13.2.1.6.1.20.29", &colorPrint, false},
  {".1.3.6.1.4.1.253.8.53.13.2.1.6.10.20.11", &networkScan, false},
  {".1.3.6.1.4.1.253.8.53.13.2.1.6.10.20.12", &emailScan, false},
  {".1.3.6.1.4.1.253.8.53.13.2.1.6.1.20.34", &blackTotal, false},
  {".1.3.6.1.4.1.253.8.53.13.2.1.6.1.20.44", &blackLargeTotal, false},
  {".1.3.6.1.4.1.253.8.53.13.2.1.6.1.20.33", &colorTotal, false},
  {".1.3.6.1.4.1.253.8.53.13.2.1.6.1.20.43", &colorLargeTotal, false},
  {".1.3.6.1.4.1.253.8.53.13.2.1.6.1.20.1", &totalAll, false}
};
const int OID_COUNT = sizeof(oidList) / sizeof(oidList[0]);
ValueCallback* callbacks[OID_COUNT];

int scheduleHours[] = {9, 15};
int scheduleMinutes[] = {0, 0};

String domain = "https://haracopy.x10.network";
String addEspUrl = domain+"/addEsp.php";
String getEspIdUrl = domain+"/getPrinterIdByEspSerial.php";
String addMeterUrl = domain+"/addMeter.php";
String getCommandUrl = domain+"/getCommand.php";
String setCommandUrl = domain+"/setCommand.php";
String pingUrl = domain+"/ping.php";

//================= FUNCTION PROTOTYPES =================//
void startConfigMode();
void setupWiFiNormal();
void checkWiFiReconnect();
void saveConfig();
void loadConfig();
void setupSNMP();
void runSNMP();
void saveMeter();
void printResults();
bool isScheduleTime();
String getChipID();

//================= SETUP =================//
void setup() {
  Serial.begin(115200);
  pinMode(CONFIG_BUTTON, INPUT_PULLUP);
  pinMode(LED,OUTPUT);

  loadConfig();
  if (strlen(config.ssid) == 0) {
    startConfigMode();
    return;
  }

  setupWiFiNormal();
  setupSNMP();
  configTime(7 * 3600, 0, "pool.ntp.org", "time.nist.gov");
  Serial.println("System ready — waiting for schedule...");
}

//================= LOOP =================//
void loop() {
  server.handleClient();
  snmp.loop();

  // Detect long press on BOOT (2 seconds)
  if (digitalRead(CONFIG_BUTTON) == LOW) {
    if (!buttonHeld) {
      buttonHeld = true;
      buttonPressStart = millis();
    }
    if (millis() - buttonPressStart >= 2000) {  // 2-second hold
      Serial.println("⚙️ Long press detected — entering Config Mode...");
      buttonHeld = false;
      startConfigMode();
      return;
    }
  } else {
    buttonHeld = false;
  }

  checkWiFiReconnect();

  if (configMode) return;

  unsigned long now = millis();
  if (now - lastScheduleCheck >= 60000) {
    lastScheduleCheck = now;
    if (isScheduleTime()){
      runSNMP();
      saveMeter();
    } 
  }

if (millis() - lastCommandCheck >= 60000) {
  lastCommandCheck = millis();
  checkCloudCommand();
}

if (millis() - lastPingCheck >= 60000) {
  lastPingCheck = millis();
  pingServer();
}


}

// ================= GET =================
String getWithRetry(String url, int maxRetries = 3, int timeoutMs = 60000) {
    HTTPClient http;
    int attempt = 0;
    String response = "";

    while (attempt < maxRetries) {
        http.begin(url);
        http.setTimeout(timeoutMs);
        int code = http.GET();
        if (code > 0) {
            response = http.getString();
            Serial.printf("📡 GET Success (HTTP %d): %s\n", code, response.c_str());
            http.end();
            return response;
        } else {
            Serial.printf("⚠️ GET failed (attempt %d/%d) code %d\n", attempt + 1, maxRetries, code);
            http.end();
            attempt++;
            delay(2000);
        }
    }
    Serial.printf("❌ GET failed after %d attempts: %s\n", maxRetries, url.c_str());
    return "";  // failed
}

String postWithRetry(String url, String payload, int maxRetries = 3, int timeoutMs = 60000) {
    HTTPClient http;
    int attempt = 0;
    String response = "";

    while (attempt < maxRetries) {
        http.begin(url);
        http.setTimeout(timeoutMs);
        http.addHeader("Content-Type", "application/x-www-form-urlencoded");
        int code = http.POST(payload);
        if (code > 0) {
            response = http.getString();
            Serial.printf("📤 POST Success (HTTP %d): %s\n", code, response.c_str());
            http.end();
            return response;
        } else {
            Serial.printf("⚠️ POST failed (attempt %d/%d) code %d\n", attempt + 1, maxRetries, code);
            http.end();
            attempt++;
            delay(2000);
        }
    }
    Serial.printf("❌ POST failed after %d attempts: %s\n", maxRetries, url.c_str());
    return "";  // failed
}



//================= CONFIG MODE =================//
void startConfigMode() {
  digitalWrite(LED,HIGH);
  configMode = true;
  WiFi.disconnect(true);
  delay(300);
  WiFi.mode(WIFI_AP);
  WiFi.softAP("ESP32_Config", "12345678");
  IPAddress IP = WiFi.softAPIP();
  Serial.printf("📶 Config mode — connect to 'ESP32_Config' and open http://%s\n", IP.toString().c_str());
  
  server.on("/", []() {
    String page = "<html><body><h2>ESP32 Config</h2>"
                  "<form action='/save' method='POST'>"
                  "ESP 32 Board name: <input name='esp32Name'><br>"
                  "WiFi SSID: <input name='ssid'><br>"
                  "Password: <input name='password' type='password'><br>"
                  "Printer IP: <input name='printer'><br>"
                  "<input type='submit' value='Save & Reboot'>"
                  "</form></body></html>";
    server.send(200, "text/html", page);
  });

  server.on("/save", []() {
  prefs.begin("esp32cfg", false);
  prefs.putString("esp32Name", server.arg("esp32Name"));
  prefs.putString("ssid", server.arg("ssid"));
  prefs.putString("pass", server.arg("password"));
  prefs.putString("printer", server.arg("printer"));
  prefs.putBool("firstSetup", true);
  prefs.end();

  server.send(200, "text/html", "<h3>✅ Saved! Rebooting...</h3>");
  delay(1000);
  digitalWrite(LED,LOW);
  ESP.restart();
});




  server.begin();
  Serial.println("🌐 Config web active.");
}

//================= WIFI NORMAL =================//
void setupWiFiNormal() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(config.ssid, config.password);
  Serial.printf("Connecting to %s", config.ssid);

  int retry = 0;
  while (WiFi.status() != WL_CONNECTED && retry++ < 40) {
    delay(500); Serial.print(".");
  }

  

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\n⚠️ WiFi failed — waiting for reconnect (won’t enter config).");
    return;
  }

if (WiFi.status() == WL_CONNECTED) {
  Serial.printf("\n✅ Connected, IP: %s\n", WiFi.localIP().toString().c_str());

  // ✅ Send registration only if first setup
  if (config.firstSetup) {
    String espSerial = getChipID();
    String esp32Name = config.esp32Name;

    String postData = "espSerial=" + espSerial + "&espName=" + esp32Name;
    String response = postWithRetry(addEspUrl, postData);  // calls API with retry

    // Optional: check response
    if(response != ""){
      Serial.println("✅ API call successful!");
      Serial.println(response);
    } else {
      Serial.println("❌ API call failed after retries!");
    }

    // Reset flag so it doesn't repeat next boot
    prefs.begin("esp32cfg", false);
    prefs.putBool("firstSetup", false);
    prefs.end();
  }
}

  server.on("/config", []() {
    server.send(200, "text/html",
                "<h3>Entering Config Mode...</h3>"
                "<p>Reconnect to <b>ESP32_Config</b> WiFi.</p>");
    delay(500);
    startConfigMode();
  });
  server.begin();
}

void checkWiFiReconnect() {
  if (WiFi.status() != WL_CONNECTED) {
    unsigned long now = millis();
    if (now - lastWiFiCheck > 5000) {
      lastWiFiCheck = now;
      Serial.println("🔄 WiFi lost — retrying...");
      WiFi.reconnect();
    }
  }
}

//================= CONFIG LOAD =================//
void loadConfig() {
  prefs.begin("esp32cfg", true);

  String esp32Name = prefs.getString("esp32Name", "");
  String ssid = prefs.getString("ssid", "");
  String pass = prefs.getString("pass", "");
  String printer = prefs.getString("printer", "");
  config.firstSetup = prefs.getBool("firstSetup", false);

  prefs.end();

  // ✅ Correct assignments
  esp32Name.toCharArray(config.esp32Name, sizeof(config.esp32Name));
  ssid.toCharArray(config.ssid, sizeof(config.ssid));
  pass.toCharArray(config.password, sizeof(config.password));
  printer.toCharArray(config.printerIP, sizeof(config.printerIP));
}


//================= SNMP =================//
void setupSNMP() {
  snmp.setUDP(&udp);
  snmp.begin();
  IPAddress printerIP;
  printerIP.fromString(config.printerIP);
  for (int i = 0; i < OID_COUNT; i++) {
    if (oidList[i].isString)
      callbacks[i] = snmp.addStringHandler(printerIP, oidList[i].oid, (char**)&oidList[i].valuePtr);
    else
      callbacks[i] = snmp.addIntegerHandler(printerIP, oidList[i].oid, (int*)oidList[i].valuePtr);
  }
}

void runSNMP() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("⚠️ WiFi not connected, skip SNMP.");
    return;
  }

  IPAddress printerIP;
  printerIP.fromString(config.printerIP);

  SNMPGet request("public", 1);
  for (int i = 0; i < OID_COUNT; i++)
    request.addOIDPointer(callbacks[i]);

  request.setUDP(&udp);
  request.setRequestID(random(1000, 9999));
  request.sendTo(printerIP);

  // ✅ Wait securely for SNMP response
  if (waitForSNMPResponse(5000)) {
    Serial.println("✅ SNMP data received successfully.");
    printResults();
  } else {
    Serial.println("⚠️ SNMP timeout — values not valid after 5s.");
    printResults(); // still print what we got
  }
}



void saveMeter() {
    String getEspIdUrlBuild = getEspIdUrl + "?serial=" + getChipID();
    String printerId = getWithRetry(getEspIdUrlBuild);

    if (printerId.length() == 0) {
        Serial.println("❌ GET failed. Saving locally for retry.");
        prefs.begin("esp32cfg", false);
        prefs.putString("pendingCommand", "run");
        prefs.end();
        return;
    }

    String postData =
        "printerId=" + printerId +
        "&printerModel=" + deviceDescr +
        "&printerSystemName=" + systemName +
        "&printerSerialNumber=" + serialNum +
        "&blackCopy=" + blackCopy +
        "&colorCopy=" + colorCopy +
        "&blackPrint=" + blackPrint +
        "&colorPrint=" + colorPrint +
        "&networkScan=" + networkScan +
        "&emailScan=" + emailScan +
        "&totalBlack=" + blackTotal +
        "&totalColor=" + colorTotal +
        "&totalBlackLarge=" + blackLargeTotal +
        "&totalColorLarge=" + colorLargeTotal +
        "&total=" + totalAll;

    String postResponse = postWithRetry(addMeterUrl, postData);
    if (postResponse.length() == 0) {
        Serial.println("❌ POST failed. Saving locally for retry.");
        prefs.begin("esp32cfg", false);
        prefs.putString("pendingCommand", "run");
        prefs.end();
    }
}


// ==========================
// Call this function in loop() to retry pending commands
void retryPendingCommand() {
    prefs.begin("esp32cfg", true);
    String cmd = prefs.getString("pendingCommand", "");
    prefs.end();
    
    if (cmd == "run") {
        Serial.println("🔄 Retrying pending Run command...");
        saveMeter();
        prefs.begin("esp32cfg", false);
        prefs.remove("pendingCommand");
        prefs.end();
    }
}






bool waitForSNMPResponse(unsigned long timeoutMs) {
  unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    snmp.loop();
    delay(50);
    if (isSNMPDataValid()) return true;
  }
  return false;
}

// Check if SNMP data has been filled correctly
bool isSNMPDataValid() {
  // choose key OIDs that should never be 0 when data is ready
  if (strlen(deviceDescr) == 0) return false;
  if (strlen(systemName) == 0) return false;
  if (strlen(serialNum) == 0) return false;
  if (blackTotal == 0 && colorTotal == 0 && totalAll == 0) return false;
  return true;
}

//================= PRINT RESULTS =================//
void printResults() {
  Serial.println("\n======= SNMP Results =======");
  Serial.printf("Device: %s\nSystem: %s\nSerial: %s\n", deviceDescr, systemName, serialNum);
  Serial.printf("Copy: B=%d C=%d | Print: B=%d C=%d\n", blackCopy, colorCopy, blackPrint, colorPrint);
  Serial.printf("Scan: Net=%d Email=%d\n", networkScan, emailScan);
  Serial.printf("Totals: BT=%d CT=%d BLT=%d CLT=%d ALL=%d\n", blackTotal, colorTotal, blackLargeTotal, colorLargeTotal, totalAll);
  Serial.println(getChipID());
  Serial.println("=============================\n");
}

void checkCloudCommand() {
    if (WiFi.status() != WL_CONNECTED) return;

    String url = getCommandUrl + "?serial=" + getChipID();
    String cmd = getWithRetry(url);

    if (cmd != "") {

        if (cmd == "run") {
            Serial.println("🚀 Cloud RUN triggered!");
            runSNMP();
            saveMeter();
            clearCloudCommand();
        }
    } else {
        Serial.println("⚠️ Failed to fetch cloud command");
    }
}


void clearCloudCommand() {
    String url = setCommandUrl + "?serial=" + getChipID() + "&cmd=none";
    String response = getWithRetry(url);
    if (response != "") {
        Serial.println("✅ Cloud command cleared successfully");
    } else {
        Serial.println("❌ Failed to clear cloud command after retries");
    }
}

// Call this function to ping the server and get command/status
String pingServer() {
    String url = pingUrl+"?serial=" + getChipID();

    // Use your getWithRetry wrapper
    String response = getWithRetry(url);

    if (response.length() == 0) {
        Serial.println("⚠️ Ping failed after retries.");
        return ""; // offline or failed
    }

    Serial.print("📡 Ping response: ");
    Serial.println(response);

    // Optionally, check for "run" command in JSON response
    if (response.indexOf("\"command\":\"run\"") > 0) {
        Serial.println("🚀 Run command received from server!");
        // Call your functions
        runSNMP();
        saveMeter();
        clearCloudCommand();
    }

    return response;
}


//================= SCHEDULE =================//
bool isScheduleTime() {
  time_t now = time(nullptr);
  struct tm* t = localtime(&now);
  for (int i = 0; i < sizeof(scheduleHours) / sizeof(int); i++) {
    if (t->tm_hour == scheduleHours[i] && t->tm_min == scheduleMinutes[i]) {
      Serial.printf("⏰ Scheduled SNMP run at %02d:%02d\n", t->tm_hour, t->tm_min);
      delay(60000);
      return true;
    }
  }
  return false;
}

String getChipID() {
  uint64_t chipid = ESP.getEfuseMac();
  char idString[20];
  sprintf(idString, "%04X%08X",
          (uint16_t)(chipid >> 32),
          (uint32_t)chipid);
  return String(idString);
}

