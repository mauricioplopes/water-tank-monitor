// ============================================================
// water_tank_monitor.ino
// ============================================================
// Water Tank Level Monitor – Version 1.1
//
// Monitors the water level in two 500-liter residential tanks
// using waterproof ultrasonic sensors (JSN-SR04T / HC-SR04).
//
// Features:
//   • Dual-sensor water level measurement
//   • Water volume calculation (truncated-cone / frustum model)
//   • Outlier rejection for noisy sensor readings
//   • MQTT publish to Adafruit IO dashboard
//   • WhatsApp push alerts via CallMeBot API
//   • OTA firmware update via built-in web server
//   • Weekly auto-reset to prevent memory fragmentation
//
// Hardware:
//   • ESP32 NodeMCU (30-pin)
//   • 2× JSN-SR04T waterproof ultrasonic sensors
//   • Voltage dividers (1 kΩ + 2.2 kΩ) on each ECHO line
//     (sensors output 5 V; ESP32 GPIOs are 3.3 V tolerant)
//
// Pin mapping:
//   GPIO 14 → TRIGGER (shared by both sensors)
//   GPIO 12 ← ECHO sensor 01 (through voltage divider)
//   GPIO 13 ← ECHO sensor 02 (through voltage divider)
//
// Author : Maurício Lopes
// Date   : May 3, 2025
// ============================================================

// ============================================================
// Libraries
// ============================================================
#include <WiFi.h>                 // ESP32 Wi-Fi driver
#include "Adafruit_MQTT.h"        // Adafruit IO MQTT protocol
#include "Adafruit_MQTT_Client.h" // MQTT client over TCP socket
#include <stdio.h>
#include <math.h>
#include <HCSR04.h>               // Multi-sensor ultrasonic driver
                                  // https://github.com/d03n3rfr1tz3/HC-SR04
#include <HTTPClient.h>           // HTTP requests (CallMeBot API)
#include <UrlEncode.h>            // URL-encode outgoing message text
#include <NetworkClient.h>
#include <WebServer.h>            // Embedded HTTP web server
#include <ESPmDNS.h>              // mDNS: resolve "esp32-webupdate.local"
#include <HTTPUpdateServer.h>     // OTA firmware update at /update

// ============================================================
// Wi-Fi credentials
// ============================================================
const char *host     = "esp32-webupdate"; // mDNS hostname
const char *ssid     = "YOUR_SSID";       // ← replace
const char *password = "YOUR_PASSWORD";   // ← replace

// ============================================================
// WhatsApp alert – CallMeBot credentials
// See https://www.callmebot.com to obtain your API key.
// Phone number format: +<country_code><number>
//   e.g. Brazil: +5511988880000
// ============================================================
String phoneNumber = "+55XXXXXXXXXXX"; // ← replace
String apiKey      = "XXXXXXX";        // ← replace

// ============================================================
// sendMessage()
//
// Sends a WhatsApp message through the CallMeBot HTTPS API.
//
// Parameters:
//   message  – plain-text message (URL-encoded before sending)
//
// The function makes a single HTTP POST and prints the result
// to the serial monitor. It does not retry on failure.
// ============================================================
void sendMessage(String message) {
  String url = "https://api.callmebot.com/whatsapp.php?phone="
               + phoneNumber
               + "&apikey=" + apiKey
               + "&text="   + urlEncode(message);

  HTTPClient http;
  http.begin(url);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");

  int httpResponseCode = http.POST(url);
  if (httpResponseCode == 200) {
    Serial.println("[WhatsApp] Message sent successfully.");
  } else {
    Serial.print("[WhatsApp] Send error. HTTP code: ");
    Serial.println(httpResponseCode);
  }
  http.end(); // release connection resources
}

// ============================================================
// Adafruit IO MQTT configuration
// Create a free account at https://io.adafruit.com
// ============================================================
#define AIO_SERVER     "io.adafruit.com"
#define AIO_SERVERPORT 1883                     // plain MQTT (no TLS)
#define AIO_USERNAME   "YOUR_AIO_USERNAME"      // ← replace
#define AIO_KEY        "YOUR_AIO_KEY"           // ← replace

// TCP socket used by the MQTT client
WiFiClient client;

// MQTT client instance connected to Adafruit IO
Adafruit_MQTT_Client mqtt(&client,
                          AIO_SERVER, AIO_SERVERPORT,
                          AIO_USERNAME, AIO_KEY);

// Publish feeds – path format: <username>/feeds/<feedname>
Adafruit_MQTT_Publish volume_caixa01 =
    Adafruit_MQTT_Publish(&mqtt, AIO_USERNAME "/feeds/volume.caixa01");
Adafruit_MQTT_Publish volume_caixa02 =
    Adafruit_MQTT_Publish(&mqtt, AIO_USERNAME "/feeds/volume.caixa02");

// ============================================================
// OTA update web server
// After flashing this sketch once via USB, subsequent updates
// can be done at: http://esp32-webupdate.local/update
// ============================================================
WebServer        httpServer(80);  // listen on port 80
HTTPUpdateServer httpUpdater;     // handles /update endpoint

// ============================================================
// GPIO assignments
// ============================================================
byte  trigger   = 14;                          // shared TRIGGER output
byte  echoCount = 2;                           // number of sensors
byte* echoPins  = new byte[echoCount]{12, 13}; // ECHO inputs

// ============================================================
// Volume tracking variables
//
// volume01_old / volume02_old  – last accepted reading
// volume01_new / volume02_new  – reading to publish after filtering
// Initialised to -1 to flag "no reading yet".
// ============================================================
float volume01     = 0;
float volume02     = 0;
float volume01_old = -1;
float volume01_new = -1;
float volume02_old = -1;
float volume02_new = -1;

// Current water height and radius (recalculated every loop)
float h01_current  = 0;
float h02_current  = 0;
float R01_current  = 0.48;
float R02_current  = 0.44;

// ── Sensor offsets (meters) ──────────────────────────────────
// Each sensor is mounted on the tank lid facing down.
// When the tank is completely full, the sensor still reads a
// non-zero distance because it sits above the water surface.
// These constants compensate for that dead zone.
float distance01_min = 0.24; // Tank 01: 24 cm lid-to-full-water gap
float distance02_min = 0.22; // Tank 02: 22 cm lid-to-full-water gap

// ============================================================
// Tank geometry – truncated-cone (frustum) model
//
// Both tanks are roughly cone-shaped, wider at the top.
// Volume formula:
//
//   V = (π · h / 3) · (R_l² + R_l · R2 + R2²)      [m³]
//
// where R_l is the radius at the current water height, found
// by linear interpolation (Thales' theorem):
//
//   R_l = R2 + (R1 - R2) · (h_current / h_max)
//
// The result is multiplied by 1000 to convert m³ → litres.
//
// All linear dimensions are in metres.
// ============================================================
#define PI 3.1415

// Tank 01
// Field measurement: sensor reads 66 cm to the bottom when empty.
// Subtracting the 24 cm offset gives h_max = 0.42 m.
#define h_max_1 0.42  // effective max water height (m)
#define R1_1    0.61  // radius at top water surface (m)
#define R2_1    0.48  // radius at tank base (m)

// Tank 02
#define h_max_2 0.59  // effective max water height (m)
#define R1_2    0.61  // radius at top water surface (m)
#define R2_2    0.44  // radius at tank base (m)

// ============================================================
// initWiFi()
//
// Connects the ESP32 to the configured Wi-Fi network.
// Blocks indefinitely until the connection is established.
// ============================================================
void initWiFi() {
  WiFi.disconnect();
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.println("[WiFi] Connecting...");
  }
  Serial.print("[WiFi] Connected. IP: ");
  Serial.println(WiFi.localIP());
}

// ============================================================
// setup()
//
// Executed once at power-on or after a hardware/software reset.
// ============================================================
void setup() {
  Serial.begin(115200);

  // Register TRIGGER and ECHO pins with the ultrasonic library
  HCSR04.begin(trigger, echoPins, echoCount);

  Serial.println("\n=============================");
  Serial.println(" MaLopes – Water Tank Monitor");
  Serial.println(" Version 1.1");
  Serial.println("=============================\n");
  delay(3000);

  initWiFi();

  // mDNS lets you access the device at http://esp32-webupdate.local/
  if (MDNS.begin(host)) {
    Serial.println("[mDNS] Responder started.");
  }

  // Mount the OTA update handler and start the server
  httpUpdater.setup(&httpServer);
  httpServer.begin();
  MDNS.addService("http", "tcp", 80);
  Serial.printf("[OTA] Ready at http://%s.local/update\n", host);

  // Notify that the device has (re)started.
  // This is useful for detecting unexpected power cycles.
  sendMessage("Water tank monitor restarted.");
}

// ============================================================
// MQTT_connect()
//
// Ensures an active connection to the Adafruit IO MQTT broker.
// If the MQTT socket is dropped, this function re-establishes
// it (first restoring Wi-Fi if necessary).
// After 50 failed MQTT attempts the ESP32 restarts itself.
// ============================================================
void MQTT_connect() {
  int8_t ret;

  if (mqtt.connected()) return; // already connected – nothing to do

  // Restore Wi-Fi if it dropped
  while (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] Connection lost. Reconnecting...");
    WiFi.reconnect();
    delay(10000);
  }

  Serial.print("[MQTT] Connecting... ");
  uint8_t retries = 50;
  while ((ret = mqtt.connect()) != 0) {
    Serial.println(mqtt.connectErrorString(ret));
    Serial.println("[MQTT] Retrying in 5 s...");
    mqtt.disconnect();
    delay(5000);
    if (--retries == 0) {
      Serial.println("[MQTT] Max retries reached. Restarting...");
      ESP.restart();
    }
  }
  Serial.println("[MQTT] Connected.");
}

// ============================================================
// Alert state-machine variables
//
// counter       – how many loop cycles to wait between alerts
//                 (1 cycle ≈ 1 min → 120 cycles ≈ 2 h cooldown)
// alert_counter – countdown timer; alert fires when it reaches 0
// alert_flag    – set to 1 after an alert is sent; cleared after
//                 the recovery message is sent
// ============================================================
const int counter = 120;
int alert_counter = counter;
int alert_flag    = 0;

// ============================================================
// Weekly reset counter
// 60 min/h × 24 h/day × 7 days = 10 080 loop cycles
// ============================================================
int rst_count = 10080;

// ============================================================
// loop()
//
// Main execution cycle. Each iteration takes approximately
// 1 minute: 5 sensor samples × 10-second intervals.
//
// Sequence per iteration:
//   1.  Keep OTA web server responsive
//   2.  Ensure MQTT connection
//   3.  Weekly auto-reset check
//   4.  Collect 5 distance samples from each sensor
//   5.  Average the samples
//   6.  Calculate water volume (frustum formula)
//   7.  Reject outlier readings
//   8.  Publish filtered volumes to Adafruit IO
//   9.  Evaluate and trigger WhatsApp alert if needed
// ============================================================
void loop(void) {
  // ── 1. OTA server ─────────────────────────────────────────
  httpServer.handleClient();

  // ── 2. MQTT connection ────────────────────────────────────
  MQTT_connect();

  // ── 3. Weekly auto-reset ──────────────────────────────────
  if (rst_count < 1) {
    Serial.println("[Reset] Weekly reset triggered. Restarting...");
    ESP.restart();
  } else {
    rst_count--;
  }

  // ── 4. Read 5 samples from each sensor ───────────────────
  float sum_01 = 0.0;
  float sum_02 = 0.0;

  for (int i = 0; i < 5; i++) {
    httpServer.handleClient();          // keep OTA server alive
    double* d = HCSR04.measureDistanceCm();
    sum_01 += (float)d[0];             // sensor 01 → GPIO 12
    sum_02 += (float)d[1];             // sensor 02 → GPIO 13
    delay(10000);                       // 10 s between samples
  }

  // ── 5. Average ────────────────────────────────────────────
  float avg_01 = sum_01 / 5.0;
  float avg_02 = sum_02 / 5.0;

  // ── 6a. Volume – Tank 01 ──────────────────────────────────
  // Water height = max height − (measured distance − dead zone)
  h01_current = fabs(h_max_1
                - fabs(fabs((float)round(avg_01)) / 100.0f
                       - distance01_min));
  // Radius at current water height (linear interpolation)
  R01_current = R2_1 + (R1_1 - R2_1) * (h01_current / h_max_1);
  // Frustum volume in litres (×1000 converts m³ → L)
  volume01 = 1000.0f * PI * h01_current
             * (R01_current * R01_current
                + R01_current * R2_1
                + R2_1 * R2_1) / 3.0f;
  if (volume01 < 0.0f) volume01 = 0.0f; // clamp negative values

  // ── 6b. Volume – Tank 02 ──────────────────────────────────
  h02_current = fabs(h_max_2
                - fabs(fabs((float)round(avg_02)) / 100.0f
                       - distance02_min));
  R02_current = R2_2 + (R1_2 - R2_2) * (h02_current / h_max_2);
  volume02 = 1000.0f * PI * h02_current
             * (R02_current * R02_current
                + R02_current * R2_2
                + R2_2 * R2_2) / 3.0f;
  if (volume02 < 0.0f) volume02 = 0.0f;

  Serial.print("[Volume] Tank01: "); Serial.print(volume01);
  Serial.print(" L  |  Tank02: "); Serial.print(volume02);
  Serial.println(" L");

  // ── 7. Outlier rejection ──────────────────────────────────
  // A reading that differs by more than 1000 L from the
  // previous accepted value is treated as a sensor glitch
  // and replaced by the previous value.

  // Tank 01
  if (volume01_old < 0) {
    // First reading – initialise history
    volume01_old = volume01;
    volume01_new = volume01;
  } else if (fabs(volume01_old - volume01) > 1000.0f) {
    Serial.println("[Filter] Tank01 outlier rejected.");
    volume01_new = volume01_old; // hold last good value
  } else {
    volume01_new = volume01;
    volume01_old = volume01;
  }

  // Tank 02
  if (volume02_old < 0) {
    volume02_old = volume02;
    volume02_new = volume02;
  } else if (fabs(volume02_old - volume02) > 1000.0f) {
    Serial.println("[Filter] Tank02 outlier rejected.");
    volume02_new = volume02_old;
  } else {
    volume02_new = volume02;
    volume02_old = volume02;
  }

  // ── 8. Publish to Adafruit IO ─────────────────────────────
  volume_caixa01.publish(volume01_new);
  delay(5000); // short pause to avoid MQTT rate-limiting
  volume_caixa02.publish(volume02_new);
  delay(5000);

  // ── 9. WhatsApp alert logic ───────────────────────────────
  // Trigger condition: either tank below its alert threshold.
  // A 120-cycle cooldown (≈ 2 h) prevents alert flooding.
  // When volume recovers, a single "normalised" message is sent.

  bool lowLevel = (volume01_new < 350.0f || volume02_new < 400.0f);

  if (lowLevel) {
    if (alert_counter > 0) {
      alert_counter--; // wait for cooldown to expire
    } else {
      sendMessage("Attention! Check the water tank levels.");
      alert_flag    = 1;       // remember that an alert was sent
      alert_counter = counter; // reset cooldown timer
    }
  } else {
    // Volume is above threshold
    if (alert_counter < 10) {
      alert_counter++; // recover cooldown gradually
    } else if (alert_flag == 1) {
      // Send one recovery notification
      sendMessage("Volume appears to have normalised.");
      alert_flag = 0;
    }
  }
}
