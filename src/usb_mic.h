// USB microphone (e.g. DJI Mic Mini 2 receiver on the USB-C port) via the ESP32-S3 USB host.
//
// The Cardputer has a single USB-C port: while the host runs, USB serial is gone. usbMicStart()
// therefore only probes when no PC is attached, and usbMicStop() hands the port back to
// USB serial/JTAG. The Cardputer does not supply 5 V on USB-C, so the receiver needs its own
// battery (or 5 V from the Grove port).
#pragma once
#include <Arduino.h>
#include <freertos/queue.h>

struct UsbMicInfo {
  uint16_t vid = 0, pid = 0;
  uint32_t rate = 0;     // device sample rate, resampled to 16 kHz
  uint8_t channels = 0;  // device channels (first two are used)
  uint8_t bits = 0;
  char product[33] = "";
};

// Installs the USB host and waits up to waitMs for an audio input device. On success a task
// fills buffers from freeQ with bufSamples int16 (L/R interleaved, 16 kHz) into filledQ, like
// the PDM mic task. On failure everything is torn down again.
bool usbMicStart(QueueHandle_t freeQ, QueueHandle_t filledQ, size_t bufSamples, uint32_t waitMs);
void usbMicStop();
bool usbMicActive();      // streaming
bool usbMicLost();        // the device was unplugged while streaming
uint32_t usbMicDrops();   // 250 ms blocks lost (no free buffer)
const UsbMicInfo& usbMicInfo();
