// indi-cam
//
// Init mode: WPA2 access point + config page (http://192.168.4.1).
// Op mode:   WiFi client, publishes a JPEG to MQTT every N seconds.
//
// Op mode is entered when a configuration is stored. If WiFi or the broker
// cannot be reached, the device reboots into init mode and waits there.
// A power cycle / reset button press retries op mode.

#include <Preferences.h>
#include "app.h"

// Survives a software reset, but not a power cycle.
#define BOOT_MAGIC 0x1D1CA400u
RTC_NOINIT_ATTR static uint32_t s_bootFlag;

static Settings cfg;
static bool opMode = false;

String deviceId() {
  uint64_t mac = ESP.getEfuseMac();
  char id[24];
  snprintf(id, sizeof(id), "indi-cam-%02x%02x%02x", (uint8_t)(mac >> 24), (uint8_t)(mac >> 32),
           (uint8_t)(mac >> 40));
  return id;
}

String deviceSuffix() {
  String id = deviceId();
  return id.substring(id.lastIndexOf('-') + 1);
}

void loadSettings(Settings &s) {
  Preferences p;
  p.begin("indicam", true);
  s.wifiSsid = p.getString("ssid", "");
  s.enterprise = p.getBool("ent", false);
  s.wifiPsk = p.getString("psk", "");
  s.eapIdentity = p.getString("eapid", "");
  s.eapUser = p.getString("eapuser", "");
  s.eapPass = p.getString("eappass", "");
  s.mqttHost = p.getString("host", "");
  s.mqttPort = p.getUShort("port", 1883);
  s.mqttTls = p.getBool("tls", false);
  s.mqttCa = p.getString("ca", "");
  s.mqttUser = p.getString("muser", "");
  s.mqttPass = p.getString("mpass", "");
  s.mqttTopic = p.getString("topic", "");
  s.intervalS = max<uint32_t>(p.getUInt("interval", 10), MIN_INTERVAL_S);
  p.end();
}

void saveSettings(const Settings &s) {
  Preferences p;
  p.begin("indicam", false);
  p.putString("ssid", s.wifiSsid);
  p.putBool("ent", s.enterprise);
  p.putString("psk", s.wifiPsk);
  p.putString("eapid", s.eapIdentity);
  p.putString("eapuser", s.eapUser);
  p.putString("eappass", s.eapPass);
  p.putString("host", s.mqttHost);
  p.putUShort("port", s.mqttPort);
  p.putBool("tls", s.mqttTls);
  p.putString("ca", s.mqttCa);
  p.putString("muser", s.mqttUser);
  p.putString("mpass", s.mqttPass);
  p.putString("topic", s.mqttTopic);
  p.putUInt("interval", s.intervalS);
  p.end();
}

String validateSettings(Settings &n) {
  n.wifiSsid.trim();
  n.mqttHost.trim();
  n.mqttCa.trim();
  n.mqttTopic.trim();
  while (n.mqttTopic.endsWith("/")) n.mqttTopic.remove(n.mqttTopic.length() - 1);
  if (!n.mqttUser.length()) n.mqttPass = "";

  if (!n.wifiSsid.length() || n.wifiSsid.length() > 32) return F("WiFi SSID is required (max. 32 characters).");
  if (!n.enterprise && (n.wifiPsk.length() < 8 || n.wifiPsk.length() > 63))
    return F("The WiFi password (PSK) must have 8 to 63 characters.");
  if (n.enterprise && (!n.eapUser.length() || !n.eapPass.length()))
    return F("WPA2-Enterprise needs a username and a password.");
  if (!n.mqttHost.length()) return F("MQTT broker host is required.");
  if (n.mqttPort == 0) return F("MQTT port must be between 1 and 65535.");
  if (n.mqttCa.length() > 3800) return F("The CA certificate is too long.");
  if (n.mqttCa.length() && n.mqttCa.indexOf("-----BEGIN CERTIFICATE-----") < 0)
    return F("The CA certificate must be in PEM format.");
  if (!n.mqttTopic.length() || n.mqttTopic.length() > 200 || n.mqttTopic.indexOf('#') >= 0 ||
      n.mqttTopic.indexOf('+') >= 0)
    return F("A topic without wildcards (# or +) is required.");
  if (n.intervalS < MIN_INTERVAL_S || n.intervalS > 86400)
    return F("The interval must be between " STR(MIN_INTERVAL_S) " and 86400 seconds.");
  return "";
}

String checkKeptSecrets(const Settings &old, const Settings &n, bool newWifiSecret, bool newMqttPass) {
  bool wifiChanged = old.wifiSsid != n.wifiSsid || old.enterprise != n.enterprise ||
                     (n.enterprise && old.eapUser != n.eapUser);
  if (wifiChanged && !newWifiSecret && (old.wifiPsk.length() || old.eapPass.length()))
    return F("The WiFi network or user was changed. Enter the WiFi password again.");
  bool mqttChanged = old.mqttHost != n.mqttHost || old.mqttPort != n.mqttPort ||
                     old.mqttTls != n.mqttTls || old.mqttCa != n.mqttCa || old.mqttUser != n.mqttUser;
  if (mqttChanged && !newMqttPass && n.mqttUser.length() && old.mqttPass.length())
    return F("The broker settings were changed. Enter the MQTT password again.");
  return "";
}

void rebootToInit(FailReason reason) {
  Serial.println("[sys] rebooting into init mode");
  bleStatus("failed", reason == FAIL_WIFI ? "WiFi connection failed" : "MQTT broker connection failed");
  s_bootFlag = BOOT_MAGIC | reason;
  Serial.flush();
  delay(200);
  ESP.restart();
  for (;;) {}
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n[sys] indi-cam starting");

  FailReason lastFail = FAIL_NONE;
  if ((s_bootFlag & 0xFFFFFF00u) == BOOT_MAGIC) lastFail = (FailReason)(s_bootFlag & 0xFF);
  s_bootFlag = 0;

  loadSettings(cfg);
  bleSetup(cfg);

  if (lastFail == FAIL_NONE && cfg.valid()) {
    opMode = true;
    opSetup(cfg);
  } else {
    if (!cfg.valid()) Serial.println("[sys] no configuration stored");
    portalSetup(cfg, lastFail);
  }
}

void loop() {
  bleLoop();
  if (opMode) opLoop();
  else portalLoop();
}
