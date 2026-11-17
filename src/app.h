#pragma once
#include <Arduino.h>

#define MIN_INTERVAL_S 5
#define STR_(x) #x
#define STR(x) STR_(x)

struct Settings {
  // WiFi
  String wifiSsid;
  bool enterprise = false;  // false: WPA2-PSK, true: WPA2-Enterprise (PEAP/TTLS)
  String wifiPsk;
  String eapIdentity;  // outer / anonymous identity
  String eapUser;
  String eapPass;
  // MQTT
  String mqttHost;
  uint16_t mqttPort = 1883;
  bool mqttTls = false;
  String mqttCa;  // optional CA certificate (PEM); empty = no server verification
  String mqttUser;
  String mqttPass;
  String mqttTopic;
  uint32_t intervalS = 10;

  bool valid() const {
    return wifiSsid.length() && mqttHost.length() && mqttTopic.length();
  }
};

enum FailReason : uint8_t { FAIL_NONE = 0, FAIL_WIFI = 1, FAIL_MQTT = 2 };

// "indi-cam-xxxxxx", unique per device (last 3 bytes of the MAC address).
// Used as setup access point name suffix and as MQTT client ID.
String deviceId();
// Just the "xxxxxx" part. Appended to the setup access point name and to the MQTT topic.
String deviceSuffix();

void loadSettings(Settings &s);
void saveSettings(const Settings &s);
// Tidies the values and checks them. Returns an error text, or "" if they can be saved.
String validateSettings(Settings &s);
// Stored passwords may only be kept while they go to the same place. If the WiFi network
// or the broker was changed, the password has to be entered again; otherwise someone who
// knows the setup code could redirect the device and collect the stored password.
// Call after validateSettings(). Returns an error text or "".
String checkKeptSecrets(const Settings &old, const Settings &s, bool newWifiSecret, bool newMqttPass);

// Reboot into init (setup) mode, remembering why. Does not return.
void rebootToInit(FailReason reason);

void portalSetup(Settings &s, FailReason lastFail);
void portalLoop();

void opSetup(const Settings &s);
void opLoop();

// Bluetooth (BLE) configuration, available in both modes.
void bleSetup(Settings &s);
void bleLoop();  // call often, also while waiting for something
// Shown live on the Bluetooth config page. state: short keyword, detail: free text.
void bleStatus(const char *state, const String &detail = "");
