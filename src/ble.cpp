// Bluetooth (BLE) configuration, available in init mode and in op mode.
//
// The config page (webflasher/index.html, protocol in webflasher/proto.js) talks to one
// service with four characteristics:
//
//   KEY     read      device public key (P-256, 65 bytes), new after every disconnect
//   RX      write     request, split into frames
//   TX      notify    response, split into frames
//   STATUS  read/notify  what the device is doing, as JSON, not encrypted
//
// Requests and responses are JSON, encrypted with AES-256-GCM. The key is
// SHA-256(ECDH shared secret); the client creates a new key pair for every request.
//   request:  client public key (65) | IV (12) | ciphertext | tag (16)
//   response: IV (12) | ciphertext | tag (16)
// Every request carries the setup code (BLE_CODE). There is no Bluetooth pairing, so a
// re-flashed device never leaves a stale pairing on the phone.
//
// Frames: first byte is a flag (bit 0 = first frame, bit 1 = last frame), then data.
//
// Requests:  {"code":"..","cmd":"get"}             -> {"ok":true,"cfg":{..}} (no passwords)
//            {"code":"..","cmd":"set","cfg":{..}}  -> {"ok":true}, then the device reboots
// Errors:    {"ok":false,"err":"text"}

#include <ArduinoJson.h>
#include <NimBLEDevice.h>
#include <esp_random.h>
#include <mbedtls/ecdh.h>
#include <mbedtls/gcm.h>
#include <mbedtls/sha256.h>
#include "app.h"

#define UUID_SERVICE "3406ec00-b055-4c3d-9eac-0a98b1d12d54"
#define UUID_KEY "3406ec01-b055-4c3d-9eac-0a98b1d12d54"
#define UUID_RX "3406ec02-b055-4c3d-9eac-0a98b1d12d54"
#define UUID_TX "3406ec03-b055-4c3d-9eac-0a98b1d12d54"
#define UUID_STATUS "3406ec04-b055-4c3d-9eac-0a98b1d12d54"

#define PUBKEY_LEN 65
#define IV_LEN 12
#define TAG_LEN 16
#define MAX_REQUEST 7000        // encrypted request, large enough for a CA certificate
#define WRONG_CODE_LOCK_MS 5000 // pause after a wrong code, slows down guessing

static Settings *cfg;
static NimBLEServer *server;
static NimBLECharacteristic *chKey, *chTx, *chStatus;

static mbedtls_ecp_group grp;
static mbedtls_mpi devPriv;

// Written by the BLE task, read by the main task once rxReady is set.
static std::string rxBuf;
static volatile bool rxReady = false;
static volatile bool newKeyWanted = false;
static volatile uint16_t connHandle = BLE_HS_CONN_HANDLE_NONE;

static uint32_t lockedUntil = 0;
static uint32_t rebootAt = 0;

static int rng(void *, unsigned char *out, size_t len) {
  esp_fill_random(out, len);
  return 0;
}

static void newDeviceKey() {
  uint8_t pub[PUBKEY_LEN];
  size_t len = 0;
  mbedtls_ecp_point q;
  mbedtls_ecp_point_init(&q);
  if (mbedtls_ecp_gen_keypair(&grp, &devPriv, &q, rng, nullptr) == 0 &&
      mbedtls_ecp_point_write_binary(&grp, &q, MBEDTLS_ECP_PF_UNCOMPRESSED, &len, pub, sizeof(pub)) == 0)
    chKey->setValue(pub, len);
  mbedtls_ecp_point_free(&q);
}

// AES key shared with the client that owns clientPub.
static bool sessionKey(const uint8_t *clientPub, uint8_t key[32]) {
  mbedtls_ecp_point p;
  mbedtls_mpi z;
  mbedtls_ecp_point_init(&p);
  mbedtls_mpi_init(&z);
  uint8_t secret[32];
  bool ok = mbedtls_ecp_point_read_binary(&grp, &p, clientPub, PUBKEY_LEN) == 0 &&
            mbedtls_ecp_check_pubkey(&grp, &p) == 0 &&
            mbedtls_ecdh_compute_shared(&grp, &z, &p, &devPriv, rng, nullptr) == 0 &&
            mbedtls_mpi_write_binary(&z, secret, sizeof(secret)) == 0 &&
            mbedtls_sha256_ret(secret, sizeof(secret), key, 0) == 0;
  mbedtls_ecp_point_free(&p);
  mbedtls_mpi_free(&z);
  return ok;
}

static void sendResponse(const uint8_t key[32], const String &json) {
  size_t len = json.length();
  std::string msg(IV_LEN + len + TAG_LEN, '\0');
  uint8_t *m = (uint8_t *)&msg[0];
  esp_fill_random(m, IV_LEN);
  mbedtls_gcm_context gcm;
  mbedtls_gcm_init(&gcm);
  bool ok = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256) == 0 &&
            mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, len, m, IV_LEN, nullptr, 0,
                                      (const uint8_t *)json.c_str(), m + IV_LEN, TAG_LEN,
                                      m + IV_LEN + len) == 0;
  mbedtls_gcm_free(&gcm);
  if (!ok || connHandle == BLE_HS_CONN_HANDLE_NONE) return;

  // A notification carries at most MTU - 3 bytes; one of them is the frame flag.
  uint16_t mtu = server->getPeerMTU(connHandle);
  size_t chunk = (mtu > 27 ? min<int>(mtu, 247) : 23) - 4;
  std::string frame;
  for (size_t off = 0; off < msg.size(); off += chunk) {
    size_t n = min(chunk, msg.size() - off);
    uint8_t flag = (off == 0 ? 1 : 0) | (off + n == msg.size() ? 2 : 0);
    frame.assign(1, (char)flag);
    frame.append(msg, off, n);
    chTx->setValue((const uint8_t *)frame.data(), frame.size());
    chTx->notify();
    delay(20);  // let the BLE stack send it before the next one is queued
  }
}

static String errorJson(const String &text) {
  JsonDocument doc;
  doc["ok"] = false;
  doc["err"] = text;
  String out;
  serializeJson(doc, out);
  return out;
}

static String handleRequest(JsonDocument &req) {
  if (millis() < lockedUntil) return errorJson(F("Too many wrong codes. Wait a few seconds."));
  if (strcmp(req["code"] | "", BLE_CODE) != 0) {
    lockedUntil = millis() + WRONG_CODE_LOCK_MS;
    Serial.println("[ble] request with wrong code rejected");
    return errorJson(F("Wrong code."));
  }

  const char *cmd = req["cmd"] | "";
  if (!strcmp(cmd, "get")) {
    const Settings &s = *cfg;
    JsonDocument doc;
    doc["ok"] = true;
    JsonObject c = doc["cfg"].to<JsonObject>();
    c["ssid"] = s.wifiSsid;
    c["ent"] = s.enterprise;
    c["hasPsk"] = s.wifiPsk.length() > 0;
    c["eapUser"] = s.eapUser;
    c["eapId"] = s.eapIdentity;
    c["hasEapPass"] = s.eapPass.length() > 0;
    c["host"] = s.mqttHost;
    c["port"] = s.mqttPort;
    c["tls"] = s.mqttTls;
    c["ca"] = s.mqttCa;
    c["user"] = s.mqttUser;
    c["hasPass"] = s.mqttPass.length() > 0;
    c["topic"] = s.mqttTopic;
    c["interval"] = s.intervalS;
    c["b64"] = s.base64;
    String out;
    serializeJson(doc, out);
    return out;
  }

  if (!strcmp(cmd, "set")) {
    JsonObject c = req["cfg"];
    if (c.isNull()) return errorJson(F("No settings in request."));
    Settings n = *cfg;  // secrets that are not sent keep their stored value
    const char *v;
    n.wifiSsid = c["ssid"] | "";
    n.enterprise = c["ent"] | false;
    if (*(v = c["psk"] | "")) n.wifiPsk = v;
    n.eapUser = c["eapUser"] | "";
    n.eapIdentity = c["eapId"] | "";
    if (*(v = c["eapPass"] | "")) n.eapPass = v;
    n.mqttHost = c["host"] | "";
    long port = c["port"] | 0L;
    n.mqttPort = (port < 1 || port > 65535) ? 0 : (uint16_t)port;
    n.mqttTls = c["tls"] | false;
    n.mqttCa = c["ca"] | "";
    n.mqttUser = c["user"] | "";
    if (*(v = c["pass"] | "")) n.mqttPass = v;
    long interval = c["interval"] | 0L;
    n.intervalS = interval < 0 ? 0 : (uint32_t)interval;
    n.base64 = c["b64"] | cfg->base64;  // pages that do not know the setting leave it as it is
    n.mqttTopic = c["topic"] | "";

    bool newWifiSecret = *(c[n.enterprise ? "eapPass" : "psk"] | "");
    bool newMqttPass = *(c["pass"] | "");
    String error = validateSettings(n);
    if (!error.length()) error = checkKeptSecrets(*cfg, n, newWifiSecret, newMqttPass);
    if (error.length()) return errorJson(error);
    saveSettings(n);
    Serial.println("[ble] configuration saved, rebooting");
    bleStatus("saved", "Settings saved, restarting");
    rebootAt = millis() + 1500;  // leave time to deliver the response
    return F("{\"ok\":true}");
  }
  return errorJson(F("Unknown command."));
}

static void processRequest() {
  uint8_t key[32];
  String response;
  bool haveKey = false;
  const size_t overhead = PUBKEY_LEN + IV_LEN + TAG_LEN;
  if (rxBuf.size() > overhead && sessionKey((const uint8_t *)rxBuf.data(), key)) {
    const uint8_t *iv = (const uint8_t *)rxBuf.data() + PUBKEY_LEN;
    size_t len = rxBuf.size() - overhead;
    std::string plain(len, '\0');
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    haveKey = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256) == 0 &&
              mbedtls_gcm_auth_decrypt(&gcm, len, iv, IV_LEN, nullptr, 0, iv + IV_LEN + len,
                                       TAG_LEN, iv + IV_LEN, (uint8_t *)&plain[0]) == 0;
    mbedtls_gcm_free(&gcm);
    if (haveKey) {
      JsonDocument req;
      if (deserializeJson(req, plain)) response = errorJson(F("Request is not valid JSON."));
      else response = handleRequest(req);
    }
  }
  // Without a working key there is nobody we could answer to.
  if (haveKey) sendResponse(key, response);
  else Serial.println("[ble] undecodable request ignored");
}

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *, ble_gap_conn_desc *desc) override {
    connHandle = desc->conn_handle;
    Serial.println("[ble] client connected");
  }
  void onDisconnect(NimBLEServer *, ble_gap_conn_desc *) override {
    connHandle = BLE_HS_CONN_HANDLE_NONE;
    newKeyWanted = true;  // a recorded session cannot be replayed to the next client
    Serial.println("[ble] client disconnected");
  }
};

class RxCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *c) override {
    if (rxReady) return;  // previous request still being processed
    std::string v = c->getValue();
    if (v.empty()) return;
    uint8_t flag = v[0];
    if (flag & 1) rxBuf.clear();
    if (rxBuf.size() + v.size() > MAX_REQUEST) {
      rxBuf.clear();
      return;
    }
    rxBuf.append(v, 1, std::string::npos);
    if (flag & 2) rxReady = true;
  }
};

void bleStatus(const char *state, const String &detail) {
  if (!chStatus) return;
  JsonDocument doc;
  doc["id"] = deviceSuffix();
  doc["state"] = state;
  doc["detail"] = detail;
  String out;
  serializeJson(doc, out);
  chStatus->setValue((const uint8_t *)out.c_str(), out.length());
  chStatus->notify();
}

void bleSetup(Settings &s) {
  cfg = &s;
  mbedtls_ecp_group_init(&grp);
  mbedtls_mpi_init(&devPriv);
  mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1);

  String name = deviceId();
  NimBLEDevice::init(name.c_str());
  NimBLEDevice::setMTU(247);
  server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());
  server->advertiseOnDisconnect(true);

  NimBLEService *svc = server->createService(UUID_SERVICE);
  chKey = svc->createCharacteristic(UUID_KEY, NIMBLE_PROPERTY::READ);
  NimBLECharacteristic *chRx = svc->createCharacteristic(UUID_RX, NIMBLE_PROPERTY::WRITE);
  chRx->setCallbacks(new RxCallbacks());
  chTx = svc->createCharacteristic(UUID_TX, NIMBLE_PROPERTY::NOTIFY);
  chStatus = svc->createCharacteristic(UUID_STATUS, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  newDeviceKey();
  svc->start();

  // The 128-bit service ID fills the advertising packet, so the name goes into the
  // scan response.
  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(UUID_SERVICE);
  NimBLEAdvertisementData scanResponse;
  scanResponse.setName(name.c_str());
  adv->setScanResponseData(scanResponse);
  adv->start();

  bleStatus("starting");
  Serial.printf("[ble] Bluetooth config active as \"%s\"\n", name.c_str());
}

void bleLoop() {
  if (rebootAt && millis() > rebootAt) {
    Serial.flush();
    ESP.restart();
  }
  if (newKeyWanted) {
    newKeyWanted = false;
    newDeviceKey();
  }
  if (rxReady) {
    processRequest();
    rxBuf.clear();
    rxReady = false;
  }
}
