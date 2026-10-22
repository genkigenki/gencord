// USB microphone input, see usb_mic.h.
#include "usb_mic.h"
#include <math.h>
#include <usb/usb_host.h>
#include <usb/uac_host.h>
#include <esp_private/usb_phy.h>

static UsbMicInfo info;
static QueueHandle_t qFree, qFilled;
static size_t bufLen;  // int16 per buffer (L/R interleaved)

static volatile bool hostRun = false, hostQuit = false;
static volatile TaskHandle_t hostTask = nullptr, readTask = nullptr;
static volatile bool readRun = false, lost = false, active = false;
static volatile uint8_t newAddr = 0, newIface = 0;  // RX device reported by the class driver
static volatile uint32_t drops = 0;
static uac_host_device_handle_t dev = nullptr;
static usb_phy_handle_t jtagPhy = nullptr;  // USB serial/JTAG PHY, re-created after host use
static int16_t* dropBuf = nullptr;

// ---------------------------------------------------------------- USB host library
static void hostTaskFn(void*) {
  while (hostRun) {
    uint32_t flags = 0;
    usb_host_lib_handle_events(pdMS_TO_TICKS(20), &flags);
    if (hostQuit) {
      if (flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) break;
      if (usb_host_device_free_all() == ESP_OK) break;  // nothing was attached
    }
  }
  hostTask = nullptr;
  vTaskDelete(nullptr);
}

static void driverCb(uint8_t addr, uint8_t iface, const uac_host_driver_event_t ev, void*) {
  if (ev == UAC_HOST_DRIVER_EVENT_RX_CONNECTED && !newAddr) {
    newIface = iface;
    newAddr = addr;
  }
}

static void deviceCb(uac_host_device_handle_t, const uac_host_device_event_t ev, void*) {
  if (ev == UAC_HOST_DRIVER_EVENT_DISCONNECTED) lost = true;
  TaskHandle_t t = readTask;
  if (t) xTaskNotifyGive(t);
}

// ---------------------------------------------------------------- resampling to 16 kHz
// Any rate -> 16 kHz: 31-tap windowed-sinc low-pass (7.2 kHz) at the input rate, evaluated
// only at the output instants, with linear interpolation between input samples (exact
// decimation for 48/32 kHz). Phase in Q32 input samples.
static constexpr int TAPS = 31, HIST = 64;  // HIST: power of two > TAPS + 1
static float taps[TAPS];
static float hist[2][HIST];
static uint32_t inCount;
static uint64_t outPos, step;
static bool filter;
static int16_t* cur;
static size_t curFill;

static void resetResampler() {
  memset(hist, 0, sizeof(hist));
  inCount = 0;
  outPos = 0;
  step = ((uint64_t)info.rate << 32) / 16000;
  filter = info.rate > 16000;
  if (filter) {
    const float fc = 7200.0f / info.rate;
    float sum = 0;
    for (int k = 0; k < TAPS; k++) {
      const float m = k - (TAPS - 1) / 2.0f;
      const float s = m == 0 ? 2 * fc : sinf(2 * (float)M_PI * fc * m) / ((float)M_PI * m);
      taps[k] = s * (0.54f - 0.46f * cosf(2 * (float)M_PI * k / (TAPS - 1)));
      sum += taps[k];
    }
    for (float& t : taps) t /= sum;
  }
}

static float filtered(int ch, uint32_t n) {  // low-passed input sample n
  if (!filter) return hist[ch][n & (HIST - 1)];
  float y = 0;
  for (int k = 0; k < TAPS; k++) y += taps[k] * hist[ch][(n - k) & (HIST - 1)];
  return y;
}

static void emit(float l, float r) {
  if (!cur) {
    int16_t* b = nullptr;
    if (xQueueReceive(qFree, &b, 0) == pdTRUE) cur = b;
    else {
      cur = dropBuf;  // every buffer waits for the SD card: this block is lost
      drops++;
    }
    curFill = 0;
  }
  cur[curFill++] = (int16_t)constrain(l, -32768.0f, 32767.0f);
  cur[curFill++] = (int16_t)constrain(r, -32768.0f, 32767.0f);
  if (curFill >= bufLen) {
    if (cur != dropBuf) xQueueSend(qFilled, &cur, 0);
    cur = nullptr;
  }
}

static void pushFrame(float l, float r) {
  hist[0][inCount & (HIST - 1)] = l;
  hist[1][inCount & (HIST - 1)] = r;
  // emit every output instant t with floor(t) + 1 <= newest input sample
  while ((outPos >> 32) + 1 <= inCount) {
    const uint32_t i = outPos >> 32;
    const float f = (uint32_t)outPos * (1.0f / 4294967296.0f);
    float ol = filtered(0, i), orr = filtered(1, i);
    if (f > 1e-6f) {
      ol += f * (filtered(0, i + 1) - ol);
      orr += f * (filtered(1, i + 1) - orr);
    }
    emit(ol, orr);
    outPos += step;
  }
  inCount++;
  if (inCount >= 0x40000000u) {  // keep the counters small (every ~6 h at 48 kHz); a multiple
    const uint32_t shift = (inCount - 2 * HIST) & ~(uint32_t)(HIST - 1);  // of HIST keeps hist[] valid
    inCount -= shift;
    outPos -= (uint64_t)shift << 32;
  }
}

// ---------------------------------------------------------------- reader task
static uint8_t subframe = 2;  // bytes per sample on the wire

static void readTaskFn(void*) {
  uint8_t raw[1536];
  size_t have = 0;
  const int sf = subframe;
  const int frame = sf * info.channels;
  while (readRun) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
    if (lost) break;
    for (;;) {
      uint32_t got = 0;
      if (uac_host_device_read(dev, raw + have, sizeof(raw) - have, &got, 0) != ESP_OK || !got) break;
      have += got;
      size_t pos = 0;
      for (; pos + frame <= have; pos += frame) {
        float s[2];
        for (int c = 0; c < 2; c++) {
          const uint8_t* p = raw + pos + (c < info.channels ? c : 0) * sf;
          int32_t v;
          if (sf == 2) v = (int16_t)(p[0] | p[1] << 8);
          else if (sf == 3) v = (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24) >> 16;
          else v = (int32_t)((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24) >> 16;
          s[c] = (float)v;
        }
        pushFrame(s[0], s[1]);
      }
      memmove(raw, raw + pos, have - pos);
      have -= pos;
    }
  }
  readTask = nullptr;
  vTaskDelete(nullptr);
}


// ---------------------------------------------------------------- start / stop
// Preference: exactly 16 kHz, then 48/32 kHz (integer decimation), then whatever is offered;
// 16-bit before 24-bit, stereo before mono (two DJI transmitters = left/right).
static int rateScore(uint32_t r) {
  switch (r) {
    case 16000: return 100;
    case 48000: return 80;
    case 32000: return 70;
    case 96000: return 50;
    case 44100: return 40;
    default: return r >= 8000 ? 20 : -1000;
  }
}

static bool pickFormat(uac_host_dev_alt_param_t& best, uint32_t& bestRate) {
  uac_host_dev_info_t di;
  if (uac_host_get_device_info(dev, &di) != ESP_OK) return false;
  info.vid = di.VID;
  info.pid = di.PID;
  int i = 0;
  for (; i < 32 && di.iProduct[i]; i++) info.product[i] = di.iProduct[i] < 128 ? (char)di.iProduct[i] : '?';
  info.product[i] = 0;
  int bestScore = -1;
  for (uint8_t alt = 1; alt <= di.iface_alt_num; alt++) {
    uac_host_dev_alt_param_t p;
    if (uac_host_get_device_alt_param(dev, alt, &p) != ESP_OK || p.format != 1 || !p.channels) continue;
    if (p.subframe_size < 2 || p.subframe_size > 4) continue;
    uint32_t rate = 0;
    int rs = -1000;
    if (p.sample_freq_type == 0) {  // continuous range
      for (uint32_t r : {16000u, 48000u, 32000u})
        if (r >= p.sample_freq_lower && r <= p.sample_freq_upper && rateScore(r) > rs) { rs = rateScore(r); rate = r; }
    } else {
      for (int k = 0; k < p.sample_freq_type && k < UAC_FREQ_NUM_MAX; k++)
        if (rateScore(p.sample_freq[k]) > rs) { rs = rateScore(p.sample_freq[k]); rate = p.sample_freq[k]; }
    }
    if (rs < 0) continue;
    const int score = rs * 100 + (p.bit_resolution == 16 ? 10 : 5) + (p.channels >= 2 ? 2 : 1);
    if (score > bestScore) { bestScore = score; best = p; bestRate = rate; }
  }
  return bestScore >= 0;
}

static void hostDown() {
  if (dev) {
    uac_host_device_stop(dev);
    uac_host_device_close(dev);
    dev = nullptr;
  }
  uac_host_uninstall();
  hostQuit = true;
  for (int i = 0; i < 200 && hostTask; i++) vTaskDelay(pdMS_TO_TICKS(10));
  hostRun = false;
  for (int i = 0; i < 50 && hostTask; i++) vTaskDelay(pdMS_TO_TICKS(10));
  usb_host_uninstall();
  // Back to USB serial/JTAG, so a PC (and flashing) works again without a reset.
  usb_phy_config_t pc = {};
  pc.controller = USB_PHY_CTRL_SERIAL_JTAG;
  pc.target = USB_PHY_TARGET_INT;
  if (usb_new_phy(&pc, &jtagPhy) != ESP_OK) jtagPhy = nullptr;
}

bool usbMicStart(QueueHandle_t freeQ, QueueHandle_t filledQ, size_t bufSamples, uint32_t waitMs) {
  if (active) return true;
  qFree = freeQ;
  qFilled = filledQ;
  bufLen = bufSamples;
  if (!dropBuf) dropBuf = (int16_t*)malloc(bufLen * sizeof(int16_t));
  if (!dropBuf) return false;
  if (jtagPhy) {
    usb_del_phy(jtagPhy);
    jtagPhy = nullptr;
  }
  usb_host_config_t hc = {};
  hc.intr_flags = ESP_INTR_FLAG_LEVEL1;
  if (usb_host_install(&hc) != ESP_OK) {
    usb_phy_config_t pc = {};
    pc.controller = USB_PHY_CTRL_SERIAL_JTAG;
    pc.target = USB_PHY_TARGET_INT;
    usb_new_phy(&pc, &jtagPhy);
    return false;
  }
  hostRun = true;
  hostQuit = false;
  TaskHandle_t h;
  xTaskCreatePinnedToCore(hostTaskFn, "usbhost", 4096, nullptr, 6, &h, 0);
  hostTask = h;
  newAddr = 0;
  lost = false;
  uac_host_driver_config_t dc = {};
  dc.create_background_task = true;
  dc.task_priority = 6;
  dc.stack_size = 4096;
  dc.core_id = 0;
  dc.callback = driverCb;
  if (uac_host_install(&dc) != ESP_OK) {
    hostDown();
    return false;
  }
  const uint32_t t0 = millis();
  while (!newAddr && millis() - t0 < waitMs) vTaskDelay(pdMS_TO_TICKS(10));
  if (!newAddr) {
    hostDown();
    return false;
  }
  uac_host_device_config_t oc = {};
  oc.addr = newAddr;
  oc.iface_num = newIface;
  oc.buffer_size = 24576;  // ~125 ms of 48 kHz stereo 16-bit
  oc.buffer_threshold = 3072;
  oc.callback = deviceCb;
  uac_host_dev_alt_param_t fmt;
  uint32_t rate = 0;
  if (uac_host_device_open(&oc, &dev) != ESP_OK || !pickFormat(fmt, rate)) {
    hostDown();
    return false;
  }
  uac_host_stream_config_t sc = {};
  sc.channels = fmt.channels;
  sc.bit_resolution = fmt.bit_resolution;
  sc.sample_freq = rate;
  if (uac_host_device_start(dev, &sc) != ESP_OK) {
    hostDown();
    return false;
  }
  info.rate = rate;
  info.channels = fmt.channels;
  info.bits = fmt.bit_resolution;
  subframe = fmt.subframe_size;
  resetResampler();
  cur = nullptr;
  drops = 0;
  readRun = true;
  xTaskCreatePinnedToCore(readTaskFn, "usbmic", 4096, nullptr, 5, &h, 0);
  readTask = h;
  active = true;
  return true;
}

void usbMicStop() {
  if (!active) return;
  readRun = false;
  TaskHandle_t t = readTask;
  if (t) xTaskNotifyGive(t);
  while (readTask) vTaskDelay(pdMS_TO_TICKS(5));
  if (cur && cur != dropBuf) xQueueSend(qFree, &cur, 0);  // half-filled block is dropped
  cur = nullptr;
  hostDown();
  active = false;
}

bool usbMicActive() { return active && !lost; }
bool usbMicLost() { return active && lost; }
uint32_t usbMicDrops() { return drops; }
const UsbMicInfo& usbMicInfo() { return info; }
