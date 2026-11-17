// Op mode: connect to WiFi, connect to the MQTT broker, publish a JPEG every interval.
// Any WiFi or broker failure that does not recover ends in rebootToInit().

#include <PubSubClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <driver/gpio.h>
#include <esp_rom_gpio.h>
#include <soc/gpio_sig_map.h>
#include <esp_camera.h>
#include "app.h"
#include "camera_pins.h"

#define WIFI_CONNECT_TIMEOUT_MS 40000  // initial connect and reconnect after a drop
#define MQTT_CONNECT_ATTEMPTS 3
#define MQTT_RETRY_DELAY_MS 5000
#define PUBLISH_CHUNK 2048

static const Settings *cfg;
static WiFiClient plainClient;
static WiFiClientSecure tlsClient;
static PubSubClient mqtt;
static String clientId;
static String pubTopic;  // "<configured topic>/<device suffix>"
static uint32_t lastPublishMs;
static uint32_t frameCount;

static bool cameraInit() {
  camera_config_t c = {};
  c.ledc_channel = LEDC_CHANNEL_0;
  c.ledc_timer = LEDC_TIMER_0;
  c.pin_d0 = Y2_GPIO_NUM;
  c.pin_d1 = Y3_GPIO_NUM;
  c.pin_d2 = Y4_GPIO_NUM;
  c.pin_d3 = Y5_GPIO_NUM;
  c.pin_d4 = Y6_GPIO_NUM;
  c.pin_d5 = Y7_GPIO_NUM;
  c.pin_d6 = Y8_GPIO_NUM;
  c.pin_d7 = Y9_GPIO_NUM;
  c.pin_xclk = XCLK_GPIO_NUM;
  c.pin_pclk = PCLK_GPIO_NUM;
  c.pin_vsync = VSYNC_GPIO_NUM;
  c.pin_href = HREF_GPIO_NUM;
  c.pin_sccb_sda = SIOD_GPIO_NUM;
  c.pin_sccb_scl = SIOC_GPIO_NUM;
  c.pin_pwdn = PWDN_GPIO_NUM;
  c.pin_reset = RESET_GPIO_NUM;
  c.xclk_freq_hz = 10000000;  // 20 MHz disturbs WiFi on this board (90% packet loss)
  c.pixel_format = PIXFORMAT_JPEG;
  c.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  c.fb_count = 1;
  if (psramFound()) {
    c.frame_size = FRAMESIZE_SVGA;  // 800x600
    c.jpeg_quality = 12;
    c.fb_location = CAMERA_FB_IN_PSRAM;
  } else {
    Serial.println("[cam] no PSRAM found, using a small frame size");
    c.frame_size = FRAMESIZE_QVGA;  // 320x240
    c.jpeg_quality = 15;
    c.fb_location = CAMERA_FB_IN_DRAM;
  }
  esp_err_t err = esp_camera_init(&c);
  if (err != ESP_OK) {
    Serial.printf("[cam] ERROR: camera init failed (0x%x)\n", err);
    return false;
  }
  Serial.println("[cam] camera ready");
  return true;
}

static bool waitForWifi(uint32_t timeoutMs) {
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > timeoutMs) return false;
    for (int i = 0; i < 10; i++) {
      bleLoop();
      delay(50);
    }
    Serial.print('.');
  }
  return true;
}

static void wifiConnect() {
  const Settings &s = *cfg;
  Serial.printf("[wifi] connecting to \"%s\" (%s)", s.wifiSsid.c_str(),
                s.enterprise ? "WPA2-Enterprise" : "WPA2-PSK");
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  // With several access points for one SSID, join the strongest instead of the first found.
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
  bleStatus("wifi_connecting", s.wifiSsid);
  WiFi.setHostname(clientId.c_str());
  if (s.enterprise) {
    const String &identity = s.eapIdentity.length() ? s.eapIdentity : s.eapUser;
    // The inner method (PEAP/MSCHAPv2 or TTLS) is negotiated with the RADIUS server.
    WiFi.begin(s.wifiSsid.c_str(), WPA2_AUTH_PEAP, identity.c_str(), s.eapUser.c_str(),
               s.eapPass.c_str());
  } else {
    WiFi.begin(s.wifiSsid.c_str(), s.wifiPsk.c_str());
  }
  if (!waitForWifi(WIFI_CONNECT_TIMEOUT_MS)) {
    Serial.printf("\n[wifi] ERROR: not connected after %d s (status %d)\n",
                  WIFI_CONNECT_TIMEOUT_MS / 1000, WiFi.status());
    rebootToInit(FAIL_WIFI);
  }
  bleStatus("wifi_ok", WiFi.localIP().toString());
  Serial.printf("\n[wifi] connected, IP %s, gateway %s, RSSI %d dBm\n",
                WiFi.localIP().toString().c_str(), WiFi.gatewayIP().toString().c_str(),
                WiFi.RSSI());
}

// Certificate validation needs the current date.
static void syncClock() {
  configTime(0, 0, "pool.ntp.org", "time.cloudflare.com");
  Serial.print("[time] waiting for NTP");
  uint32_t start = millis();
  while (time(nullptr) < 1700000000 && millis() - start < 15000) {
    delay(500);
    Serial.print('.');
  }
  if (time(nullptr) < 1700000000)
    Serial.println("\n[time] WARNING: no NTP time, TLS certificate validation will likely fail");
  else
    Serial.println(" ok");
}

static bool mqttConnectOnce() {
  const Settings &s = *cfg;
  Serial.printf("[mqtt] connecting to %s:%u%s as \"%s\"\n", s.mqttHost.c_str(), s.mqttPort,
                s.mqttTls ? " (TLS)" : "", clientId.c_str());
  bleStatus("mqtt_connecting", s.mqttHost + ":" + s.mqttPort);
  const char *user = s.mqttUser.length() ? s.mqttUser.c_str() : nullptr;
  const char *pass = s.mqttUser.length() ? s.mqttPass.c_str() : nullptr;
  if (mqtt.connect(clientId.c_str(), user, pass)) {
    Serial.println("[mqtt] connected");
    return true;
  }
  // state: -4 timeout, -2 TCP/TLS connect failed, 4 bad credentials, 5 not authorized
  Serial.printf("[mqtt] ERROR: connect failed, state %d\n", mqtt.state());
  if (s.mqttTls) {
    char buf[120];
    if (tlsClient.lastError(buf, sizeof(buf)) != 0) Serial.printf("[mqtt] TLS: %s\n", buf);
  }
  return false;
}

static void mqttConnect() {
  for (int i = 1; i <= MQTT_CONNECT_ATTEMPTS; i++) {
    if (mqttConnectOnce()) return;
    for (uint32_t t = millis(); i < MQTT_CONNECT_ATTEMPTS && millis() - t < MQTT_RETRY_DELAY_MS;) {
      bleLoop();
      delay(50);
    }
  }
  Serial.printf("[mqtt] ERROR: broker not reachable after %d attempts\n", MQTT_CONNECT_ATTEMPTS);
  rebootToInit(FAIL_MQTT);
}

// The camera clock disturbs WiFi reception on this board, which shows up as lost packets and
// uploads that stall for seconds. So the clock only runs while a picture is taken.
static void cameraClock(bool on) {
  // Connect the clock pin to the camera clock generator, or hold it low.
  if (!on) gpio_set_level((gpio_num_t)XCLK_GPIO_NUM, 0);
  esp_rom_gpio_connect_out_signal(XCLK_GPIO_NUM, on ? CAM_CLK_IDX : SIG_GPIO_OUT_IDX, false, false);
}

static void captureAndPublish() {
  cameraClock(true);
  // Drop the frames from before and just after the pause, so the picture is current and
  // the exposure has settled.
  camera_fb_t *fb = nullptr;
  for (int i = 0; i < 3; i++) {
    fb = esp_camera_fb_get();
    if (fb) esp_camera_fb_return(fb);
  }
  fb = esp_camera_fb_get();
  cameraClock(false);
  if (!fb) {
    Serial.println("[cam] ERROR: capture failed");
    return;
  }

  uint32_t start = millis();
  // The PUBLISH packet (QoS 0) is written directly to the connection. PubSubClient cannot
  // send it: it keeps the packet length in 16 bits, so images above 64 KB go out with a
  // wrong length and the broker drops the connection ("malformed packet").
  Client &net = cfg->mqttTls ? (Client &)tlsClient : (Client &)plainClient;
  const size_t topicLen = pubTopic.length();
  uint8_t head[8 + topicLen];
  size_t h = 0;
  head[h++] = 0x30;  // PUBLISH, QoS 0, not retained
  uint32_t remaining = 2 + topicLen + fb->len;
  do {
    uint8_t b = remaining % 128;
    remaining /= 128;
    head[h++] = remaining ? (b | 0x80) : b;
  } while (remaining);
  head[h++] = topicLen >> 8;
  head[h++] = topicLen & 0xFF;
  memcpy(head + h, pubTopic.c_str(), topicLen);
  h += topicLen;
  bool ok = net.write(head, h) == h;
  for (size_t off = 0; ok && off < fb->len;) {
    size_t n = min<size_t>(PUBLISH_CHUNK, fb->len - off);
    ok = net.write(fb->buf + off, n) == n;
    off += n;
  }

  if (ok) {
    bleStatus("running", String("Image ") + (frameCount + 1) + ", " + (fb->len / 1024) + " KB, WiFi " +
                             WiFi.RSSI() + " dBm");
    Serial.printf("[pub] #%u: %ux%u JPEG, %u bytes -> %s (%u ms, RSSI %d dBm)\n", ++frameCount,
                  fb->width, fb->height, fb->len, pubTopic.c_str(), millis() - start,
                  WiFi.RSSI());
  } else {
    Serial.printf("[pub] ERROR: publishing %u bytes failed, reconnecting\n", fb->len);
    mqtt.disconnect();  // a half-written packet would corrupt the stream
  }
  esp_camera_fb_return(fb);
}

void opSetup(const Settings &s) {
  cfg = &s;
  clientId = deviceId();
  pubTopic = s.mqttTopic + "/" + deviceSuffix();
  Serial.printf("[sys] op mode: topic \"%s\", interval %u s\n", pubTopic.c_str(), s.intervalS);

  if (!cameraInit()) {
    // Not a configuration problem, so the setup portal would not help.
    Serial.println("[cam] restarting in 10 s");
    delay(10000);
    ESP.restart();
  }
  cameraClock(false);

  wifiConnect();

  if (s.mqttTls) {
    if (s.mqttCa.length()) {
      syncClock();
      tlsClient.setCACert(s.mqttCa.c_str());
    } else {
      Serial.println("[mqtt] WARNING: no CA certificate configured, broker certificate is NOT verified");
      tlsClient.setInsecure();
    }
    mqtt.setClient(tlsClient);
  } else {
    mqtt.setClient(plainClient);
  }
  mqtt.setServer(s.mqttHost.c_str(), s.mqttPort);
  mqtt.setBufferSize(512);  // only header + topic are buffered, the image is streamed
  mqtt.setKeepAlive(60);  // opLoop() services the connection between publishes
  mqtt.setSocketTimeout(15);

  mqttConnect();
  lastPublishMs = millis() - s.intervalS * 1000UL;  // first image right away
}

void opLoop() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.print("[wifi] connection lost, waiting for reconnect");
    if (!waitForWifi(WIFI_CONNECT_TIMEOUT_MS)) {
      Serial.println("\n[wifi] ERROR: reconnect failed");
      rebootToInit(FAIL_WIFI);
    }
    Serial.printf("\n[wifi] reconnected, IP %s\n", WiFi.localIP().toString().c_str());
  }
  if (!mqtt.connected()) {
    Serial.printf("[mqtt] not connected (state %d)\n", mqtt.state());
    mqttConnect();
  }
  mqtt.loop();

  if (millis() - lastPublishMs >= cfg->intervalS * 1000UL) {
    lastPublishMs = millis();
    captureAndPublish();
  }
  delay(10);
}
