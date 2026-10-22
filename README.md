# gencord

A pocket voice recorder for the **original M5Stack Cardputer**, with a fully local AI pipeline:
record on the device, upload over your home Wi-Fi to your own server (WebDAV), and get a
transcribed, summarised Markdown note in your **Obsidian** vault. No cloud, no subscription.

```
Cardputer ──WebDAV──▶ NAS / Nextcloud / any WebDAV ──▶ pipeline (Docker, NVIDIA GPU) ──▶ Obsidian note
 records WAV            only on your home Wi-Fi          Whisper + local LLM + speakers
```

## Device

- **Record with one button:** G0 starts a recording, from sleep too. Double-click G0 stops it.
- **Retro cassette screen:** spinning reels, an LED VU meter with peak hold and an AGC pointer,
  a coloured gain badge (`x1` green … `x64` red) and a recording timer.
- **Automatic gain (AGC):** tracks the noise floor, so quiet voices get loud and background
  hiss is not pumped up.
- **Name your recording** by typing while recording, or right after stopping.
- **Safe on the SD card:** a save point every 10 s. Cut-off recordings are repaired as
  `…_recovered.wav`.
- **Upload only at home:** the device scans briefly for your Wi-Fi in the background. It
  uploads to WebDAV, checks the size, renames the file and only then deletes it locally.
  Away from home the radio stays off.
- **Clock:** NTP when at home. The last known time is remembered; red `~` means not synced yet.
- **Battery:** beeps when low and saves the recording before the battery is empty.
  Light sleep between uses.
- **USB microphone (experimental):** a USB audio device on the USB-C port, such as the DJI
  Mic Mini 2 receiver, replaces the built-in mic. When a recording starts on battery, gencord
  looks for one for about 1 s; a cyan mic icon shows it is in use. Two transmitters are mixed
  to mono. If the device goes away, the recording continues on the built-in mic.
  **The Cardputer does not supply 5 V on USB-C**, and the DJI receiver only connects when the
  port has 5 V, even the model with a battery. Use a USB-C OTG splitter with a charging input
  (power bank → splitter), or feed 5 V from the Grove port. gencord does not search while a
  PC is connected, so USB serial and flashing keep working. `usb_mic=0` turns it off.
- **Keys:** `(G0)rec (u)pload (l)ist (o)ff (t)ime (a)gc` and `c` for setup.

### Install

gencord runs from an app slot of [M5Launcher](https://github.com/bmorcelli/Launcher), or stand-alone.
Both images are on the [Releases page](../../releases).

- **M5Launcher:** copy `gencord.bin` to the SD card and install it from the Launcher
  (SD → Install), or upload it through the Launcher WebUI (OTA).
- **M5Burner:** search for "gencord" (once listed). It flashes the full image.
- **esptool, launcher layout:** write `gencord.bin` to the app slot. In the stock layout
  that is ota_0 at `0x170000`; check your partition table, the Launcher can move slots.
  ```
  esptool --chip esp32s3 --port COM5 --baud 921600 write-flash 0x170000 gencord.bin
  ```
- **esptool, full install** (replaces everything, the Launcher included):
  ```
  esptool --chip esp32s3 --port COM5 --baud 921600 write-flash 0x0 gencord.full.bin
  ```
- **Build yourself:** `pio run -e cardputer` (PlatformIO, pioarduino platform; see `platformio.ini`).
  `tools/flash_once.py` flashes only when the running gencord answers `FLASH-OK`, so it never
  cuts off a recording.

For download mode, hold G0, press reset, then release G0.

### First setup

Press **`c`** on the device and enter:
- Wi-Fi name and password
- WebDAV URL, user and password

The settings are stored in `recorder.cfg` on the SD card. You can also edit that file on a PC:

```
wifi_ssid=MyHomeWifi
wifi_pass=secret
dav_url=http://192.168.1.10:5005/Recordings/inbox
dav_user=recorder
dav_pass=secret
tz=CET-1CEST,M3.5.0,M10.5.0/3
ntp=pool.ntp.org
agc_max=64
wifi_check_min=20
usb_mic=1
```

WebDAV examples:
- Synology: package "WebDAV Server", port 5005 (http) or 5006 (https).
- Nextcloud: `https://cloud.example.org/remote.php/dav/files/USER/Recordings`.

Without any setup gencord is simply an offline recorder; nothing leaves the SD card.

Hardware: Cardputer v1 (StampS3, ESP32-S3FN8, 8 MB flash), microSD card (FAT32).
Not tested on the Cardputer ADV.

## Pipeline (optional)

[`pipeline/`](pipeline/) is a Docker stack (Portainer or `docker compose`) for a PC with an NVIDIA GPU.
For every new recording in the inbox it does the following:

- transcribes with **faster-whisper large-v3-turbo**, using hints from your word list and a repetition guard;
- separates **speakers** with pyannote. This needs a free Hugging Face token. A speaker is named
  automatically when someone addresses them by name;
- summarises with a **local LLM** (LM Studio or Ollama), using a template per recording type:
  meeting, call, conversation, memo or talk. The note gets decisions, tasks as checkboxes and open questions;
- writes a **Markdown note** into your vault, with frontmatter, people as `[[links]]` and a
  transcript in paragraphs. On Windows, a timestamp click opens the recording in **VLC** at
  that second (`tools/install-gencord-links.ps1`);
- archives the audio as small Opus/OGG files (about 14 MB per hour).

See [pipeline/README.md](pipeline/README.md) for setup.

## Licence

MIT, see [LICENSE](LICENSE). `lib/usb_host_uac` is Espressif's USB Host UAC driver (Apache-2.0,
[lib/usb_host_uac/LICENSE](lib/usb_host_uac/LICENSE)). The pyannote models have their own terms on Hugging Face.
