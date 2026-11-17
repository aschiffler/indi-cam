# indi-cam

ESP32-S3 camera that publishes a JPEG image to an MQTT topic at a fixed interval.
It is configured over WiFi through a built-in setup page.

## Hardware

- ESP32-S3 with 16 MB flash and 8 MB PSRAM, OV2640 camera
  (Freenove ESP32-S3-WROOM CAM / ESP32-S3-EYE camera pinout)
- Connected and powered through the board's native USB port. On Windows it appears as
  "USB Serial Device (COMx)" with USB ID `303A:1001`. No driver is needed.

## How it works

The device has two modes.

**Init mode (setup).** The device opens its own WiFi access point and serves a config page.
It enters this mode when no configuration is stored, or when the last attempt to connect to
WiFi or to the MQTT broker failed. It stays there until you save a configuration or reset it.

**Op mode (operation).** The device joins the configured WiFi, connects to the MQTT broker and
publishes one 800x600 JPEG per interval. The MQTT payload is the raw JPEG file.

The device appends its ID to the configured topic: with topic `vlab/cam`, the device
`20b868` publishes to `vlab/cam/20b868`. Subscribe to `vlab/cam/+` to receive all devices.

The device falls back from op mode to init mode when:

- WiFi does not connect within 40 seconds (at start, or after the connection was lost), or
- the MQTT broker cannot be reached or refuses the login 3 times in a row.

## First setup

1. Power the device. It starts in init mode.
2. Join the WiFi network `indi-cam-setup-xxxxxx` with the password `indicam-setup`.
   `xxxxxx` is different for every device (the last 6 hex digits of its MAC address), so
   several devices can be in init mode at the same time. The serial log shows the full name.
3. Open `http://192.168.4.1/`. Most phones open the page automatically.
4. Fill in the form and press **Save and reboot**.

| Setting | Notes |
|---|---|
| SSID | WiFi network to join |
| Security | WPA2-PSK (password) or WPA2-Enterprise (username and password, PEAP/TTLS) |
| Anonymous identity | WPA2-Enterprise only, optional. Defaults to the username |
| Broker host / IP, Port | Usually 1883 without TLS, 8883 with TLS |
| Use TLS | Encrypts the broker connection |
| CA certificate | Optional, PEM format. If empty, the broker certificate is not verified |
| User, Password | Optional. Leave both empty for a broker without login |
| Topic | Base topic, no wildcards. Images are published to `<topic>/xxxxxx`, for example `vlab/cam/20b868` |
| Publish interval | Seconds, minimum 5 |

Stored passwords are never shown on the page. Leave a password field empty to keep the
stored value.

The access point name prefix and password are set in `platformio.ini` (`AP_SSID`, `AP_PSK`).
The password is the same for all devices. The same `xxxxxx` is used in the MQTT client ID
(`indi-cam-xxxxxx`) and is shown on the setup page.

## Reset procedures

### The device is in init mode and should try again

This is the case after a WiFi or broker outage. The stored configuration is still there.

- Press the **RST** button on the board, or unplug and replug the power.

The device then tries op mode again with the stored configuration. If the connection fails
again, it returns to init mode.

### Change settings after a failed connection

Join `indi-cam-setup-xxxxxx` and open `http://192.168.4.1/`. The page shows which step failed
(WiFi or broker) and is filled with the stored settings. Correct them and press
**Save and reboot**.

### Change settings while the device is working

Use the Bluetooth configuration (next section). It works in both modes and keeps the
settings you do not change.

If Bluetooth is not an option, erase the device with the web flasher (**Flash / reset**) or
with the commands below. This deletes the stored configuration, including all passwords.
The device then starts in init mode.

```powershell
python -m platformio run -t erase --upload-port COM13
python -m platformio run -t upload --upload-port COM13
```

### The device does not respond over USB

Put the board in download mode by hand: hold **BOOT**, press and release **RST**, release
**BOOT**. Then run the upload command and press **RST** once afterwards.

## Configuration via Bluetooth

Every device is reachable over Bluetooth (BLE) as `indi-cam-xxxxxx`, in init mode and in op
mode. The settings are the same as on the setup page.

### With the web page

1. Open the page from `webflasher/` in Chrome or Edge, on a PC or an Android phone.
   iPhones are not supported. The page must be served via `https://` or `http://localhost`:

   ```powershell
   python -m http.server 8000 --directory webflasher
   ```

2. Press **Configure**, enter the setup code and press **Select device**.
3. Pick the device from the list. The form shows the current settings. Password fields are
   empty; leave them empty to keep the stored passwords.
4. Press **Save and restart**. The page reconnects after the restart and shows what the
   device is doing: connecting to WiFi, connecting to the broker, running, or the reason
   for a failure.

The setup code is `indicam-setup` and is the same for all devices. It is set in
`platformio.ini` (`BLE_CODE`). After a wrong code the device refuses requests for 5 seconds.

There is no Bluetooth pairing. The settings are encrypted between the page and the device,
and every request must contain the setup code. Anyone within Bluetooth range who knows the
code can read the settings (without passwords) and change them. The status line is readable
without the code.

### From the command line

```powershell
python -m pip install bleak cryptography
python tools/ble_config.py scan
python tools/ble_config.py get 2192e4
python tools/ble_config.py set 2192e4 topic=vlab/cam interval=10
python tools/ble_config.py watch 2192e4
```

## Web flasher

The same page also offers **Show log** and **Flash / reset** over USB (desktop Chrome or
Edge only). **Flash / reset** installs the firmware and deletes all stored settings. Run
`python tools/build_webflasher.py` after firmware changes to update the image it installs.

### Hosted page

The page is deployed at <https://thws.education/indi-cam/> (Kubernetes namespace `vlab`,
served by the `nginx` deployment from the configmap `indi-cam-web`). On a PC it offers
**Show log**, **Configure** and **Flash / reset**; on a phone only **Configure**.

To publish a changed page:

```powershell
kubectl -n vlab create configmap indi-cam-web --from-file=index.html=webflasher/index.html --from-file=proto.js=webflasher/proto.js --dry-run=client -o yaml | kubectl apply -f -
```

The pod picks up the new files within about a minute, no restart needed. The nginx changes
that were made for the path `/indi-cam` are in `k8s/`. Backups of the previous state are kept locally in
`k8s/backup/` (not in the repository).

### Firmware releases

The source is at <https://github.com/aschiffler/indi-cam>. Pushing a version tag builds the
firmware, creates a GitHub release with the image attached, and publishes the image to
GitHub Pages:

```powershell
git tag v1.0.1
git push origin v1.0.1
```

The hosted page downloads the image from GitHub Pages
(<https://aschiffler.github.io/indi-cam/>), so **Flash / reset** there always installs the
latest released version. The page shows which version that is.

The setup code and the setup WiFi password are part of the public firmware. They do not
protect stored passwords on their own; the firmware does: passwords cannot be read back,
and a stored password is only kept while the WiFi network or broker it belongs to is
unchanged. Someone with USB access to a device can always read its flash memory.

## Viewing the console log on Windows 11

The log is sent over the same USB cable at 115200 baud. Nothing has to be installed;
Windows PowerShell is enough.

1. Find the COM port of the board:

   ```powershell
   Get-CimInstance Win32_SerialPort | Where-Object PNPDeviceID -match 'VID_303A' | Select-Object DeviceID
   ```

   You can also look in Device Manager under "Ports (COM & LPT)" for "USB Serial Device".

2. Show the log. Replace `COM13` with your port. Stop with Ctrl+C.

   ```powershell
   $p = New-Object System.IO.Ports.SerialPort COM13,115200
   $p.Open()
   try { while ($true) { if ($p.BytesToRead) { Write-Host -NoNewline $p.ReadExisting() } else { Start-Sleep -Milliseconds 100 } } } finally { $p.Close() }
   ```

Opening the port does not reset the device. To see the start-up messages, press **RST** while
the log is running. If the command ends with an error at that moment, run it again.

Only one program can use the COM port at a time. Close the log before flashing.

If PlatformIO is installed, these also work:

```powershell
python -m platformio device monitor --port COM13
python tools/serial_log.py COM13 60    # prints the log for 60 seconds
```

### Reading the log

| Line | Meaning |
|---|---|
| `[init] init mode: AP "indi-cam-setup-20b868" ...` | Init mode, waiting for configuration |
| `[init] reason: WiFi connection failed` | Why the device fell back to init mode |
| `[init] reason: MQTT broker connection failed` | Same, for the broker |
| `[wifi] connected, IP ..., RSSI -45 dBm` | WiFi joined. RSSI closer to 0 is a stronger signal |
| `[mqtt] connected` | Broker login OK |
| `[mqtt] ERROR: connect failed, state N` | -4 timeout, -2 network or TLS failure, 4 bad credentials, 5 not authorized |
| `[pub] #12: 800x600 JPEG, 27037 bytes -> cam (34 ms, ...)` | Image published |

## Build and flash

Requires Python and PlatformIO (`python -m pip install platformio`).

```powershell
python -m platformio run                                   # build
python -m platformio run -t upload --upload-port COM13     # build and flash
```

Flashing keeps the stored configuration.

## Checking the images

Any MQTT client can receive the images. With mosquitto installed:

```powershell
mosquitto_sub -h <broker> -t "vlab/cam/+" -C 1 > image.jpg
```

## Notes

- The camera clock disturbs WiFi reception on this board. It is set to 10 MHz (at 20 MHz most
  packets are lost) and it only runs while a picture is taken. With the clock running all the
  time, uploads on a weak WiFi signal stall for many seconds. See `cameraClock()` in
  `src/opmode.cpp`.
- If several access points share the SSID, the device joins the strongest one.
- Without a CA certificate, a TLS connection is encrypted but the broker's identity is not
  checked.
- WPA2-Enterprise does not verify the certificate of the authentication server.

## Source layout

| File | Content |
|---|---|
| `src/main.cpp` | Mode selection, stored settings |
| `src/portal.cpp` | Init mode: access point and config page |
| `src/opmode.cpp` | Op mode: WiFi, camera, MQTT |
| `src/ble.cpp` | Bluetooth configuration, protocol description |
| `src/camera_pins.h` | Camera wiring |
| `tools/serial_log.py` | Serial log helper |
| `tools/ble_config.py` | Bluetooth configuration from the command line |
| `tools/build_webflasher.py` | Packages the firmware for the web flasher |
| `tools/test_proto.mjs` | Test of the page's Bluetooth protocol code |
| `webflasher/` | Web page: log, Bluetooth configuration, flash / reset |
