## gencord v0.11

Pocket voice recorder for the original M5Stack Cardputer: records WAV to microSD and
uploads it to your own WebDAV server (Synology, Nextcloud, …) only on your home Wi-Fi.
No cloud.

### New
- **USB microphone input (experimental):** a USB audio device on the USB-C port, such as
  the DJI Mic Mini 2 receiver, is used instead of the built-in mic. A cyan mic icon shows it.
  The Cardputer does not supply 5 V on USB-C, and the DJI receiver only connects with 5 V
  on the port. A USB-C OTG splitter with a charging input (or 5 V from the Grove port) is
  needed. `usb_mic=0` in `recorder.cfg` turns the search off.
- The recording log notes which mic was used.

### Files
- `gencord.bin`: app image for [M5Launcher](https://github.com/bmorcelli/Launcher)
  (SD → Install, or WebUI OTA) or esptool at the app slot (`0x170000` in the stock launcher layout).
- `gencord.full.bin`: complete image for a bare Cardputer. Flash it to `0x0`; it replaces
  everything, the launcher included.
