// Init mode: WPA2 access point with a config page and captive-portal DNS.

#include <DNSServer.h>
#include <WebServer.h>
#include <WiFi.h>
#include "app.h"

static const IPAddress AP_IP(192, 168, 4, 1);

static WebServer server(80);
static DNSServer dns;
static Settings *cfg;
static FailReason lastFail;

static String esc(const String &in) {
  String out;
  out.reserve(in.length() + 16);
  for (char c : in) {
    switch (c) {
      case '&': out += F("&amp;"); break;
      case '<': out += F("&lt;"); break;
      case '>': out += F("&gt;"); break;
      case '"': out += F("&quot;"); break;
      case '\'': out += F("&#39;"); break;
      default: out += c;
    }
  }
  return out;
}

static const char PAGE_HEAD[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>indi-cam setup - THWS Dr. Schiffler</title>
<style>
body{font-family:system-ui,sans-serif;margin:0;background:#f2f4f7;color:#1c2430}
main{max-width:30rem;margin:0 auto;padding:1rem}
h1{font-size:1.4rem;margin-bottom:.2rem}
.sub{margin:0 0 1rem;color:#5d6878}
fieldset{background:#fff;border:1px solid #d5dae1;border-radius:8px;margin:0 0 1rem;padding:.8rem 1rem 1rem}
legend{font-weight:600;padding:0 .4rem}
label{display:block;margin-top:.7rem;font-size:.9rem}
input[type=text],input[type=password],input[type=number],select,textarea{width:100%;box-sizing:border-box;
padding:.5rem;margin-top:.2rem;border:1px solid #b6bdc8;border-radius:6px;font:inherit}
textarea{font-family:monospace;font-size:.75rem;height:7rem}
label.cb{display:flex;align-items:center;gap:.5rem}
small{color:#5d6878}
button{width:100%;padding:.8rem;font:inherit;font-weight:600;color:#fff;background:#1463c4;border:0;border-radius:8px}
.msg{padding:.7rem 1rem;border-radius:8px;margin-bottom:1rem}
.err{background:#fde8e6;border:1px solid #e0988f}
.ok{background:#e4f5e6;border:1px solid #8cc795}
</style></head><body><main><h1>indi-cam setup</h1><p class="sub">THWS Dr. Schiffler &middot; device )HTML";

static String pageHead() {
  String h = FPSTR(PAGE_HEAD);
  h += deviceId();
  h += F("</p>");
  return h;
}

// Setup access point name: AP_SSID plus the device-unique suffix, e.g. "indi-cam-setup-20b868"
static String apSsid() { return String(AP_SSID) + "-" + deviceSuffix(); }

static const char PAGE_SCRIPT[] PROGMEM = R"HTML(
<script>
var sec=document.getElementById('sec'),tls=document.getElementById('tls'),port=document.getElementById('port');
function upd(){
  var e=sec.value=='ent';
  document.getElementById('f_psk').style.display=e?'none':'';
  document.getElementById('f_ent').style.display=e?'':'none';
  document.getElementById('f_ca').style.display=tls.checked?'':'none';
}
sec.onchange=upd;
tls.onchange=function(){
  if(tls.checked&&port.value=='1883')port.value='8883';
  if(!tls.checked&&port.value=='8883')port.value='1883';
  upd();
};
upd();
</script>
)HTML";

static void textInput(String &h, const char *label, const char *name, const String &value,
                      int maxLen, const char *extra = "") {
  h += F("<label>");
  h += label;
  h += F("<input type=\"text\" name=\"");
  h += name;
  h += F("\" maxlength=\"");
  h += maxLen;
  h += F("\" value=\"");
  h += esc(value);
  h += F("\" autocapitalize=\"off\" autocorrect=\"off\" spellcheck=\"false\" ");
  h += extra;
  h += F("></label>");
}

// Stored secrets are never sent back to the browser; an empty field keeps the stored value.
static void secretInput(String &h, const char *label, const char *name, bool isSet, int maxLen) {
  h += F("<label>");
  h += label;
  h += F("<input type=\"password\" name=\"");
  h += name;
  h += F("\" maxlength=\"");
  h += maxLen;
  h += F("\" autocomplete=\"new-password\" placeholder=\"");
  h += isSet ? F("(stored - leave empty to keep)") : F("");
  h += F("\"></label>");
}

static void sendPage(const String &error) {
  const Settings &s = *cfg;
  String h;
  h.reserve(6000 + s.mqttCa.length());
  h += pageHead();

  if (error.length()) {
    h += F("<div class=\"msg err\">");
    h += esc(error);
    h += F("</div>");
  } else if (lastFail == FAIL_WIFI) {
    h += F("<div class=\"msg err\">Last start failed: could not connect to WiFi &quot;");
    h += esc(s.wifiSsid);
    h += F("&quot;. Check the settings and save again.</div>");
  } else if (lastFail == FAIL_MQTT) {
    h += F("<div class=\"msg err\">Last start failed: WiFi was OK, but the MQTT broker ");
    h += esc(s.mqttHost);
    h += ':';
    h += s.mqttPort;
    h += F(" could not be reached or refused the login. Check the settings and save again.</div>");
  }

  h += F("<form method=\"post\" action=\"/save\"><fieldset><legend>WiFi</legend>");
  textInput(h, "SSID", "ssid", s.wifiSsid, 32, "required");
  h += F("<label>Security<select name=\"sec\" id=\"sec\"><option value=\"psk\">WPA2-PSK</option>"
         "<option value=\"ent\"");
  if (s.enterprise) h += F(" selected");
  h += F(">WPA2-Enterprise (PEAP / TTLS)</option></select></label><div id=\"f_psk\">");
  secretInput(h, "Password (PSK)", "psk", s.wifiPsk.length(), 63);
  h += F("</div><div id=\"f_ent\">");
  textInput(h, "Username", "eapuser", s.eapUser, 100);
  secretInput(h, "Password", "eappass", s.eapPass.length(), 100);
  textInput(h, "Anonymous identity <small>(optional, defaults to username)</small>", "eapid",
            s.eapIdentity, 100);
  h += F("</div></fieldset><fieldset><legend>MQTT</legend>");
  textInput(h, "Broker host / IP", "host", s.mqttHost, 100, "required");
  h += F("<label>Port<input type=\"number\" name=\"port\" id=\"port\" min=\"1\" max=\"65535\" required value=\"");
  h += s.mqttPort;
  h += F("\"></label><label class=\"cb\"><input type=\"checkbox\" name=\"tls\" id=\"tls\"");
  if (s.mqttTls) h += F(" checked");
  h += F(">Use TLS</label><div id=\"f_ca\"><label>CA certificate, PEM <small>(optional - if empty, "
         "the broker certificate is not verified)</small><textarea name=\"ca\" maxlength=\"3800\" "
         "spellcheck=\"false\">");
  h += esc(s.mqttCa);
  h += F("</textarea></label></div>");
  textInput(h, "User <small>(optional)</small>", "muser", s.mqttUser, 100);
  secretInput(h, "Password", "mpass", s.mqttPass.length(), 100);
  String topicLabel = F("Topic <small>(images are published to &lt;topic&gt;/");
  topicLabel += deviceSuffix();
  topicLabel += F(")</small>");
  textInput(h, topicLabel.c_str(), "topic", s.mqttTopic, 200, "required");
  h += F("<label>Publish interval, seconds <small>(min. " STR(MIN_INTERVAL_S) ")</small>"
         "<input type=\"number\" name=\"interval\" min=\"" STR(MIN_INTERVAL_S) "\" max=\"86400\" required value=\"");
  h += s.intervalS;
  h += F("\"></label><label>Payload<select name=\"payload\"><option value=\"raw\">JPEG file (binary)"
         "</option><option value=\"b64\"");
  if (s.base64) h += F(" selected");
  h += F(">JPEG as base64 text</option></select></label></fieldset>"
         "<button type=\"submit\">Save and reboot</button></form>");
  h += FPSTR(PAGE_SCRIPT);
  h += F("</main></body></html>");
  server.send(error.length() ? 400 : 200, "text/html", h);
}

static void handleSave() {
  Settings n = *cfg;  // start from stored values so empty secret fields keep them
  static Settings stored;  // what is saved on the device (cfg may hold rejected input)
  static bool haveStored = false;
  if (!haveStored) {
    loadSettings(stored);
    haveStored = true;
  }
  String v;

  n.wifiSsid = server.arg("ssid");
  n.enterprise = server.arg("sec") == "ent";
  if ((v = server.arg("psk")).length()) n.wifiPsk = v;
  n.eapUser = server.arg("eapuser");
  n.eapIdentity = server.arg("eapid");
  if ((v = server.arg("eappass")).length()) n.eapPass = v;

  n.mqttHost = server.arg("host");
  long port = server.arg("port").toInt();
  n.mqttTls = server.hasArg("tls");
  n.mqttCa = server.arg("ca");
  n.mqttUser = server.arg("muser");
  if ((v = server.arg("mpass")).length()) n.mqttPass = v;
  n.mqttTopic = server.arg("topic");
  long interval = server.arg("interval").toInt();
  n.mqttPort = (port < 1 || port > 65535) ? 0 : (uint16_t)port;
  n.intervalS = interval < 0 ? 0 : (uint32_t)interval;
  n.base64 = server.arg("payload") == "b64";

  bool newWifiSecret = server.arg(n.enterprise ? "eappass" : "psk").length();
  bool newMqttPass = server.arg("mpass").length();
  String error = validateSettings(n);
  if (!error.length()) error = checkKeptSecrets(stored, n, newWifiSecret, newMqttPass);
  *cfg = n;  // keep the user's input for the re-rendered form
  if (error.length()) {
    sendPage(error);
    return;
  }

  saveSettings(n);
  Serial.println("[cfg] configuration saved, rebooting into op mode");
  String h = pageHead();
  h += F("<div class=\"msg ok\">Saved. The device is rebooting and will connect to &quot;");
  h += esc(n.wifiSsid);
  h += F("&quot;. If the connection fails, this access point comes back.</div></main></body></html>");
  server.send(200, "text/html", h);
  server.client().flush();
  delay(1000);
  ESP.restart();
}

void portalSetup(Settings &s, FailReason fail) {
  cfg = &s;
  lastFail = fail;

  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(AP_IP, AP_IP, IPAddress(255, 255, 255, 0));
  if (!WiFi.softAP(apSsid().c_str(), AP_PSK)) Serial.println("[init] ERROR: could not start access point");
  dns.start(53, "*", AP_IP);  // captive portal: resolve every name to us

  server.on("/", HTTP_GET, []() { sendPage(""); });
  server.on("/save", HTTP_POST, handleSave);
  server.onNotFound([]() {
    server.sendHeader("Location", "http://" + AP_IP.toString() + "/");
    server.send(302, "text/plain", "");
  });
  server.begin();

  bleStatus("setup", fail == FAIL_WIFI   ? "WiFi connection failed"
                     : fail == FAIL_MQTT ? "MQTT broker connection failed"
                                         : "Waiting for configuration");
  Serial.printf("[init] init mode: AP \"%s\" (WPA2), config page at http://%s/\n", apSsid().c_str(),
                AP_IP.toString().c_str());
  if (fail == FAIL_WIFI) Serial.println("[init] reason: WiFi connection failed");
  if (fail == FAIL_MQTT) Serial.println("[init] reason: MQTT broker connection failed");
  Serial.println("[init] waiting for configuration (reset or power cycle retries op mode)");
}

void portalLoop() {
  dns.processNextRequest();
  server.handleClient();
  delay(2);
}
