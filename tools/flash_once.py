"""Flash gencord to the Cardputer exactly once, only with the firmware's consent.

usage: python -I flash_once.py <firmware.bin> [--download-mode]

Default: send 'F'; only if the running gencord answers FLASH-OK (menu, not recording)
flash once with esptool's normal USB reset. Any other answer or error: stop, no retry.
--download-mode: the user put the chip into download mode (G0 + reset) by hand;
check that ota_0 is the recorder slot and flash once.
"""
import os
import subprocess
import sys
import time

import serial
import serial.tools.list_ports

FW = sys.argv[1]
MANUAL = "--download-mode" in sys.argv
ESPTOOL = [sys.executable, "-m", "esptool", "--chip", "esp32s3"]


def port_of_cardputer():
    for p in serial.tools.list_ports.comports():
        if p.vid == 0x303A:
            return p.device
    return None


def esptool(*args):
    # esptool prints non-ASCII progress characters; a cp1252 pipe made it crash mid-flash
    env = dict(os.environ, PYTHONIOENCODING="utf-8", PYTHONUTF8="1")
    r = subprocess.run(ESPTOOL + list(args), capture_output=True, text=True, encoding="utf-8", errors="replace",
                       env=env)
    return r.returncode, (r.stdout or "") + (r.stderr or "")


port = port_of_cardputer()
if not port:
    sys.exit("no Cardputer on USB (asleep? press reset)")

if MANUAL:
    code, out = esptool("--port", port, "--before", "no-reset", "--after", "no-reset",
                        "read-flash", "0x8000", "0xC00", "pt_check.bin")
    if code != 0:
        sys.exit("not in download mode or no answer - stopped")
    data = open("pt_check.bin", "rb").read()
    labels = [data[i + 12:i + 28].rstrip(b"\0") for i in range(0, len(data), 32)
              if data[i:i + 2] == b"\xaa\x50" and data[i + 2] == 0 and data[i + 3] == 0x10
              and int.from_bytes(data[i + 4:i + 8], "little") == 0x170000]
    if not labels:
        sys.exit("ota_0 at 0x170000 not found - stopped")
    before = "no-reset"
else:
    answer = b""
    try:
        with serial.Serial(port, 115200, timeout=0.3) as s:
            s.reset_input_buffer()
            s.write(b"F")
            t0 = time.time()
            while time.time() - t0 < 2 and b"FLASH-OK" not in answer and b"BUSY" not in answer:
                answer += s.read(256)
    except (serial.SerialException, OSError) as e:
        sys.exit(f"port not usable ({e}) - stopped")
    if b"BUSY-RECORDING" in answer:
        sys.exit("cardputer is recording - not flashing, try again after stopping")
    if b"FLASH-OK" not in answer:
        sys.exit("no answer from gencord (asleep or other firmware) - stopped, nothing touched")
    print("cardputer shows UPDATING", flush=True)
    time.sleep(0.5)
    before = "default-reset"

code, out = esptool("--port", port, "--before", before, "--after", "hard-reset", "--baud", "921600",
                    "write-flash", "0x170000", FW)
print("\n".join(l for l in out.splitlines() if "Wrote" in l or "verified" in l or "rror" in l)
      .encode("ascii", "replace").decode())
sys.exit(0 if code == 0 else "flash FAILED - stopped, no retry")
