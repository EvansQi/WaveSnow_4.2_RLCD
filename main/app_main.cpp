#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <math.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/usb_serial_jtag.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "nvs_flash.h"
#include "penguin_bitmap.h"
#include "rlcd_st7305.h"

namespace {

// --- Configuration ---
const char *TAG = "codex_usage";
constexpr int kOfflineTimeoutMs = 20000;
constexpr int kWatchdogIntervalMs = 1000;
constexpr int kSensorRefreshMs = 3000;
constexpr int kClockRefreshMs = 1000;
constexpr int kKeyPollIntervalMs = 15;
constexpr int kKeyDebounceMs = 180;
constexpr int kI2cTimeoutMs = 100;
constexpr int kBattReadIntervalMs = 5000;
constexpr int kBattSamples = 48;
constexpr float kBattEmaAlpha = 0.1f;
constexpr gpio_num_t kI2cSda = GPIO_NUM_13;
constexpr gpio_num_t kI2cScl = GPIO_NUM_14;
constexpr gpio_num_t kKeyButton = GPIO_NUM_18;
constexpr gpio_num_t kBattAdcPin = GPIO_NUM_4;
constexpr adc_channel_t kBattAdcChannel = ADC_CHANNEL_3;
constexpr float kBattDividerRatio = 3.0f;
constexpr float kBattEmptyV = 3.20f;
constexpr float kBattFullV = 4.10f;
constexpr float kBattExternalPowerV = 4.35f;
constexpr float kBattAdcApproxFullScaleMv = 2800.0f;
constexpr uint8_t kShtc3Address = 0x70;
constexpr uint16_t kShtc3Wakeup = 0x3517;
constexpr uint16_t kShtc3Sleep = 0xB098;
constexpr uint16_t kShtc3MeasureTFirstNormal = 0x7866;
constexpr char kWifiSsid[] = "Evans";
constexpr char kWifiPassword[] = "Evans12345678";
constexpr char kServerHost[] = "192.168.137.1";
constexpr uint16_t kServerPort = 8766;

EventGroupHandle_t s_wifi_event_group;
constexpr int kWifiConnectedBit = BIT0;

// --- Shared State ---
struct UsageData {
  int five_hour_remaining = 99;
  char five_hour_reset[16] = "16:28";
  int week_remaining = 85;
  char week_reset[16] = "05/31";
  char clock_text[16] = "12:00";
  char temperature_text[16] = "--.-C";
  char humidity_text[16] = "--%";
  char weather_text[16] = "--";
  int battery_percent = -1;
  float battery_voltage = 0.0f;
  bool usb_connected = false;
  bool online = false;
};

RlcdSt7305 *display = nullptr;
SemaphoreHandle_t usage_mutex = nullptr;
SemaphoreHandle_t display_mutex = nullptr;
UsageData usage;
int64_t last_serial_update_ms = 0;
i2c_master_bus_handle_t i2c_bus = nullptr;
i2c_master_dev_handle_t shtc3_dev = nullptr;
bool show_usage_overlay = false;
int64_t last_key_press_ms = 0;
bool sntp_started = false;
adc_oneshot_unit_handle_t adc_handle = nullptr;
adc_cali_handle_t adc_cali_handle = nullptr;

int clamp_percent(int value, int fallback) {
  if (value < 0 || value > 100) {
    return fallback;
  }
  return value;
}

void compact_ascii_copy(char *dest, size_t dest_size, const char *src) {
  size_t j = 0;
  for (size_t i = 0; src[i] != '\0' && j + 1 < dest_size; ++i) {
    unsigned char ch = static_cast<unsigned char>(src[i]);
    if (isalnum(ch) || ch == ':' || ch == '/' || ch == '-' || ch == ' ') {
      dest[j++] = static_cast<char>(ch);
    }
  }
  dest[j] = '\0';
}

bool copy_if_changed(char *dest, size_t dest_size, const char *src) {
  char clean[32];
  compact_ascii_copy(clean, sizeof(clean), src);
  if (strncmp(dest, clean, dest_size) == 0) {
    return false;
  }
  strncpy(dest, clean, dest_size - 1);
  dest[dest_size - 1] = '\0';
  return true;
}

void normalize_week_reset(char *dest, size_t dest_size, const char *src) {
  int numbers[2] = {0, 0};
  int count = 0;
  int current = -1;
  for (size_t i = 0; src[i] != '\0'; ++i) {
    unsigned char ch = static_cast<unsigned char>(src[i]);
    if (isdigit(ch)) {
      if (current < 0) current = 0;
      current = current * 10 + (ch - '0');
    } else if (current >= 0) {
      if (count < 2) numbers[count++] = current;
      current = -1;
    }
  }
  if (current >= 0 && count < 2) numbers[count++] = current;

  if (count >= 2) {
    snprintf(dest, dest_size, "%02d/%02d", numbers[0], numbers[1]);
  } else {
    compact_ascii_copy(dest, dest_size, src);
  }
}

bool normalize_week_reset_if_changed(char *dest, size_t dest_size, const char *src) {
  char normalized[16];
  normalize_week_reset(normalized, sizeof(normalized), src);
  if (strncmp(dest, normalized, dest_size) == 0) {
    return false;
  }
  strncpy(dest, normalized, dest_size - 1);
  dest[dest_size - 1] = '\0';
  return true;
}

void draw_bitmap(int x, int y, int width, int height, int bytes_per_row,
                 const uint8_t *bitmap, bool black) {
  for (int row = 0; row < height; ++row) {
    for (int col = 0; col < width; ++col) {
      int byte_index = row * bytes_per_row + (col >> 3);
      uint8_t mask = static_cast<uint8_t>(0x80 >> (col & 7));
      if (bitmap[byte_index] & mask) {
        display->DrawPixel(x + col, y + row, black);
      }
    }
  }
}

// --- Procedural weather icon drawing ---
void fill_circle(int cx, int cy, int r, bool black) {
  for (int dy = -r; dy <= r; ++dy) {
    for (int dx = -r; dx <= r; ++dx) {
      if (dx * dx + dy * dy <= r * r) {
        display->DrawPixel(cx + dx, cy + dy, black);
      }
    }
  }
}

void draw_hline(int x, int y, int w, bool black) {
  display->FillRect(x, y, w, 1, black);
}

void draw_vline(int x, int y, int h, bool black) {
  display->FillRect(x, y, 1, h, black);
}

void draw_sun_icon(int cx, int cy, int r) {
  fill_circle(cx, cy, r, true);
  draw_vline(cx, cy - r - 11, 8, true);
  draw_vline(cx, cy + r + 4, 8, true);
  draw_hline(cx - r - 11, cy, 8, true);
  draw_hline(cx + r + 4, cy, 8, true);
  for (int d = 0; d < 7; ++d) {
    display->DrawPixel(cx - r - 5 + d, cy - r - 5 + d, true);
    display->DrawPixel(cx + r + 5 - d, cy - r - 5 + d, true);
    display->DrawPixel(cx - r - 5 + d, cy + r + 5 - d, true);
    display->DrawPixel(cx + r + 5 - d, cy + r + 5 - d, true);
  }
}

void draw_cloud_icon(int cx, int cy) {
  fill_circle(cx - 18, cy + 4, 14, true);
  fill_circle(cx + 4, cy - 8, 18, true);
  fill_circle(cx + 22, cy + 2, 13, true);
  display->FillRect(cx - 30, cy + 4, 55, 14, true);
}

void draw_rain_icon(int cx, int cy) {
  draw_cloud_icon(cx, cy - 8);
  for (int i = 0; i < 5; ++i) {
    int dx = -20 + i * 10;
    int dy = 14 + (i % 2) * 6;
    draw_vline(cx + dx, cy + dy, 10, true);
  }
}

void draw_snow_icon(int cx, int cy) {
  draw_cloud_icon(cx, cy - 8);
  for (int i = 0; i < 4; ++i) {
    int x = cx - 18 + i * 12;
    int y = cy + 18 + (i % 2) * 8;
    draw_hline(x - 3, y, 7, true);
    draw_vline(x, y - 3, 7, true);
  }
}

void draw_thunder_icon(int cx, int cy) {
  draw_cloud_icon(cx, cy - 8);
  display->FillRect(cx - 4, cy + 12, 8, 14, true);
  display->FillRect(cx - 12, cy + 22, 12, 8, true);
  display->FillRect(cx - 2, cy + 25, 7, 14, true);
}

void draw_partly_cloudy_icon(int cx, int cy) {
  draw_sun_icon(cx - 20, cy - 12, 10);
  draw_cloud_icon(cx + 6, cy + 4);
}

void normalize_weather_text(const char *weather, char *out, size_t out_size) {
  size_t j = 0;
  for (size_t i = 0; weather[i] != '\0' && j + 1 < out_size; ++i) {
    unsigned char ch = static_cast<unsigned char>(weather[i]);
    if (isalpha(ch) || ch == ' ') {
      out[j++] = static_cast<char>(toupper(ch));
    }
  }
  out[j] = '\0';
}

void draw_big_weather_icon(int cx, int cy, const char *weather) {
  char normalized[24];
  normalize_weather_text(weather, normalized, sizeof(normalized));

  if (strstr(normalized, "THUNDER") || strstr(normalized, "STORM")) {
    draw_thunder_icon(cx, cy);
  } else if (strstr(normalized, "SNOW") || strstr(normalized, "SLEET")) {
    draw_snow_icon(cx, cy);
  } else if (strstr(normalized, "RAIN") || strstr(normalized, "DRIZZLE") ||
             strstr(normalized, "SHOWER")) {
    draw_rain_icon(cx, cy);
  } else if (strstr(normalized, "PARTLY") || strstr(normalized, "CLOUD")) {
    draw_partly_cloudy_icon(cx, cy);
  } else if (strstr(normalized, "CLEAR") || strstr(normalized, "SUNNY") ||
             strstr(normalized, "FAIR")) {
    draw_sun_icon(cx, cy, 14);
  } else if (strstr(normalized, "OVERCAST") || strstr(normalized, "FOG") ||
             strstr(normalized, "MIST") || strstr(normalized, "HAZE")) {
    draw_cloud_icon(cx, cy);
  } else {
    display->DrawText(cx - 18, cy - 10, "--", 3, true);
  }

  if (normalized[0] != '\0') {
    display->DrawText(cx - 45, cy + 45, normalized, 1, true);
  }
}

// --- Battery ADC Reading ---
bool read_battery_voltage(float *out_v) {
  if (adc_handle == nullptr) return false;
  int raw_values[kBattSamples];
  int valid = 0;
  for (int i = 0; i < kBattSamples; ++i) {
    int raw = 0;
    if (adc_oneshot_read(adc_handle, kBattAdcChannel, &raw) != ESP_OK) continue;
    raw_values[valid++] = raw;
  }
  if (valid < 5) return false;

  for (int i = 0; i < valid - 1; ++i) {
    for (int j = i + 1; j < valid; ++j) {
      if (raw_values[j] < raw_values[i]) {
        int tmp = raw_values[i];
        raw_values[i] = raw_values[j];
        raw_values[j] = tmp;
      }
    }
  }

  int trim = valid / 8;
  if (trim < 1) trim = 1;
  if (trim * 2 >= valid) trim = 0;

  int raw_sum = 0;
  int averaged = 0;
  for (int i = trim; i < valid - trim; ++i) {
    raw_sum += raw_values[i];
    ++averaged;
  }
  if (averaged == 0) return false;

  int raw_avg = raw_sum / averaged;
  int mv = 0;
  bool calibrated = false;
  if (adc_cali_handle && adc_cali_raw_to_voltage(adc_cali_handle, raw_avg, &mv) == ESP_OK) {
    calibrated = true;
  } else {
    mv = static_cast<int>((static_cast<float>(raw_avg) / 4095.0f) * kBattAdcApproxFullScaleMv);
  }
  *out_v = mv * kBattDividerRatio / 1000.0f;
  ESP_LOGI(TAG, "Battery sample CH3 raw_avg=%d adc_mv=%d batt_v=%.3f calibrated=%s samples=%d trim=%d",
           raw_avg, mv, *out_v, calibrated ? "yes" : "no", averaged, trim);
  return *out_v > 0.5f;
}

int voltage_to_percent(float v) {
  struct BatteryPoint {
    float voltage;
    int percent;
  };

  static constexpr BatteryPoint kCurve[] = {
      {4.20f, 100}, {4.15f, 95}, {4.11f, 90}, {4.08f, 85}, {4.02f, 80},
      {3.98f, 75},  {3.95f, 70}, {3.91f, 65}, {3.87f, 60}, {3.85f, 55},
      {3.84f, 50},  {3.82f, 45}, {3.80f, 40}, {3.79f, 35}, {3.77f, 30},
      {3.75f, 25},  {3.73f, 20}, {3.71f, 15}, {3.69f, 10}, {3.61f, 5},
      {3.27f, 0},
  };

  if (v >= kCurve[0].voltage) return 100;
  if (v <= kCurve[sizeof(kCurve) / sizeof(kCurve[0]) - 1].voltage) return 0;

  for (size_t i = 0; i + 1 < sizeof(kCurve) / sizeof(kCurve[0]); ++i) {
    const BatteryPoint &high = kCurve[i];
    const BatteryPoint &low = kCurve[i + 1];
    if (v <= high.voltage && v >= low.voltage) {
      float span_v = high.voltage - low.voltage;
      float ratio = span_v > 0.0f ? (v - low.voltage) / span_v : 0.0f;
      float pct = low.percent + ratio * (high.percent - low.percent);
      if (pct < 0.0f) pct = 0.0f;
      if (pct > 100.0f) pct = 100.0f;
      return static_cast<int>(pct + 0.5f);
    }
  }

  return 0;
}

bool update_battery_state(float voltage) {
  static bool ema_initialized = false;
  static float ema_voltage = 0.0f;
  float battery_voltage = voltage;
  if (battery_voltage < kBattEmptyV) battery_voltage = kBattEmptyV;
  if (battery_voltage > kBattFullV) battery_voltage = kBattFullV;

  if (!ema_initialized) {
    ema_voltage = battery_voltage;
    ema_initialized = true;
  } else {
    ema_voltage = kBattEmaAlpha * battery_voltage + (1.0f - kBattEmaAlpha) * ema_voltage;
  }

  // USB Serial/JTAG presence is not a reliable power-source signal during monitor sessions,
  // so treat only abnormally high battery voltage as external power.
  bool usb_connected = voltage >= kBattExternalPowerV;
  int pct = voltage_to_percent(ema_voltage);
  bool should_render = false;
  xSemaphoreTake(usage_mutex, portMAX_DELAY);
  if (usage.battery_percent < 0 || abs(usage.battery_percent - pct) >= 1 ||
      fabsf(usage.battery_voltage - ema_voltage) >= 0.03f ||
      usage.usb_connected != usb_connected) {
    usage.battery_percent = pct;
    usage.battery_voltage = ema_voltage;
    usage.usb_connected = usb_connected;
    should_render = true;
  }
  xSemaphoreGive(usage_mutex);
  return should_render;
}

void draw_battery_icon(int x, int y, int pct) {
  const int w = 30, h = 14;
  display->DrawRect(x, y, w, h, true);
  display->FillRect(x + w, y + 3, 3, 8, true);
  int fill = (pct * (w - 4)) / 100;
  if (fill > 0) {
    display->FillRect(x + 2, y + 2, fill, h - 4, true);
  }
}

void init_time_sync() {
  if (sntp_started) {
    return;
  }

  setenv("TZ", "CST-8", 1);
  tzset();
  esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
  esp_sntp_setservername(0, "ntp.aliyun.com");
  esp_sntp_setservername(1, "ntp.tencent.com");
  esp_sntp_setservername(2, "pool.ntp.org");
  esp_sntp_init();
  sntp_started = true;
  ESP_LOGI(TAG, "SNTP time sync started");
}

// --- SHTC3 Temperature/Humidity Sensor ---
uint8_t shtc3_crc8(const uint8_t *data, size_t len) {
  uint8_t crc = 0xFF;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) {
      if (crc & 0x80) {
        crc = static_cast<uint8_t>((crc << 1) ^ 0x31);
      } else {
        crc <<= 1;
      }
    }
  }
  return crc;
}

esp_err_t shtc3_send_command(uint16_t command) {
  uint8_t buffer[2] = {
      static_cast<uint8_t>(command >> 8),
      static_cast<uint8_t>(command & 0xFF),
  };
  return i2c_master_transmit(shtc3_dev, buffer, sizeof(buffer), kI2cTimeoutMs);
}

bool read_shtc3(float *temperature_c, float *humidity_percent) {
  uint8_t data[6] = {};

  if (shtc3_send_command(kShtc3Wakeup) != ESP_OK) {
    return false;
  }
  vTaskDelay(pdMS_TO_TICKS(2));

  if (shtc3_send_command(kShtc3MeasureTFirstNormal) != ESP_OK) {
    shtc3_send_command(kShtc3Sleep);
    return false;
  }
  vTaskDelay(pdMS_TO_TICKS(20));

  esp_err_t err = i2c_master_receive(shtc3_dev, data, sizeof(data), kI2cTimeoutMs);
  shtc3_send_command(kShtc3Sleep);
  if (err != ESP_OK) {
    return false;
  }

  if (shtc3_crc8(&data[0], 2) != data[2] || shtc3_crc8(&data[3], 2) != data[5]) {
    ESP_LOGW(TAG, "SHTC3 CRC mismatch");
    return false;
  }

  uint16_t raw_temp = static_cast<uint16_t>((data[0] << 8) | data[1]);
  uint16_t raw_humidity = static_cast<uint16_t>((data[3] << 8) | data[4]);
  *temperature_c = -45.0f + 175.0f * (static_cast<float>(raw_temp) / 65535.0f);
  *humidity_percent = 100.0f * (static_cast<float>(raw_humidity) / 65535.0f);
  return true;
}

// --- Display Rendering ---
bool display_initialized = false;

void render_display() {
  xSemaphoreTake(display_mutex, portMAX_DELAY);
  UsageData snapshot;
  bool usage_overlay = false;
  xSemaphoreTake(usage_mutex, portMAX_DELAY);
  snapshot = usage;
  usage_overlay = show_usage_overlay;
  xSemaphoreGive(usage_mutex);

  if (!display_initialized) {
    display->Clear(false);
    display->DrawText(250, 190, "TEMP", 2, true);
    display->DrawText(250, 224, "RH", 2, true);
    display_initialized = true;
  }

  display->FillRect(16, 10, 210, 276, false);

  if (usage_overlay) {
    display->DrawText(16, 14, "USAGE", 3, true);
    display->DrawText(20, 66, "5H", 3, true);
    char percent1[8];
    snprintf(percent1, sizeof(percent1), "%d%%", snapshot.five_hour_remaining);
    display->DrawText(84, 60, percent1, 5, true);
    display->DrawText(20, 110, snapshot.five_hour_reset, 2, true);
    display->DrawRect(20, 134, 188, 20, true);
    display->FillRect(22, 136, (184 * snapshot.five_hour_remaining) / 100, 16, true);

    display->DrawText(20, 176, "1W", 3, true);
    char percent2[8];
    snprintf(percent2, sizeof(percent2), "%d%%", snapshot.week_remaining);
    display->DrawText(84, 170, percent2, 5, true);
    display->DrawText(20, 220, snapshot.week_reset, 2, true);
    display->DrawRect(20, 244, 188, 20, true);
    display->FillRect(22, 246, (184 * snapshot.week_remaining) / 100, 16, true);
  } else {
    const int sprite_x = 16;
    const int sprite_y = 44;
    draw_bitmap(sprite_x, sprite_y, kPenguinWidth, kPenguinHeight, kPenguinBytesPerRow,
                kPenguinBitmapOpen, true);
  }
  display->FillRect(34, 268, 160, 2, true);
  display->FillRect(188, 268, 18, 2, true);

  display->FillRect(234, 44, 166, 232, false);
  display->DrawText(250, 50, snapshot.clock_text, 5, true);
  draw_big_weather_icon(316, 135, snapshot.weather_text);
  display->DrawText(250, 204, snapshot.temperature_text, 3, true);
  display->DrawText(250, 238, snapshot.humidity_text, 3, true);

  display->FillRect(334, 258, 66, 38, false);
  if (snapshot.usb_connected) {
    display->DrawText(352, 260, "USB", 1, true);
  }
  if (snapshot.battery_percent >= 0) {
    draw_battery_icon(340, 279, snapshot.battery_percent);
    char battery_pct_text[8];
    snprintf(battery_pct_text, sizeof(battery_pct_text), "%d%%", snapshot.battery_percent);
    display->DrawText(376, 282, battery_pct_text, 1, true);
  }

  display->FillRect(20, 276, 200, 20, false);
  display->DrawText(20, 276, snapshot.online ? "SYNC OK" : "OFFLINE", 2, true);

  display->Present();
  xSemaphoreGive(display_mutex);
}

// --- Data Parsing ---
bool parse_usage_line(char *line) {
  if (strncmp(line, "USAGE|", 6) != 0) {
    return false;
  }

  char *save_ptr = nullptr;
  char *token = strtok_r(line, "|", &save_ptr);
  if (token == nullptr || strcmp(token, "USAGE") != 0) {
    return false;
  }

  char *five_hour_remaining = strtok_r(nullptr, "|", &save_ptr);
  char *five_hour_reset = strtok_r(nullptr, "|", &save_ptr);
  char *week_remaining = strtok_r(nullptr, "|", &save_ptr);
  char *week_reset = strtok_r(nullptr, "|", &save_ptr);
  char *clock_text = strtok_r(nullptr, "|", &save_ptr);
  char *weather_text = strtok_r(nullptr, "|", &save_ptr);
  if (five_hour_remaining == nullptr || five_hour_reset == nullptr ||
      week_remaining == nullptr || week_reset == nullptr ||
      clock_text == nullptr) {
    return false;
  }

  bool should_render = false;
  xSemaphoreTake(usage_mutex, portMAX_DELAY);
  int new_five_hour = clamp_percent(atoi(five_hour_remaining), usage.five_hour_remaining);
  int new_week = clamp_percent(atoi(week_remaining), usage.week_remaining);
  if (usage.five_hour_remaining != new_five_hour) {
    usage.five_hour_remaining = new_five_hour;
    should_render = true;
  }
  should_render |= copy_if_changed(usage.five_hour_reset, sizeof(usage.five_hour_reset), five_hour_reset);
  if (usage.week_remaining != new_week) {
    usage.week_remaining = new_week;
    should_render = true;
  }
  should_render |= normalize_week_reset_if_changed(usage.week_reset, sizeof(usage.week_reset), week_reset);
  should_render |= copy_if_changed(usage.clock_text, sizeof(usage.clock_text), clock_text);
  if (weather_text != nullptr) {
    should_render |= copy_if_changed(usage.weather_text, sizeof(usage.weather_text), weather_text);
  }
  should_render |= !usage.online;
  usage.online = true;
  last_serial_update_ms = esp_timer_get_time() / 1000;
  UsageData snapshot = usage;
  xSemaphoreGive(usage_mutex);

  ESP_LOGI(TAG, "WiFi update: 5H=%d%% reset=%s, 1W=%d%% reset=%s, time=%s, wx=%s",
           snapshot.five_hour_remaining, snapshot.five_hour_reset,
           snapshot.week_remaining, snapshot.week_reset,
           snapshot.clock_text, snapshot.weather_text);
  if (should_render) {
    render_display();
  }
  return true;
}

// --- Background Tasks ---
void offline_watchdog_task(void *) {
  while (true) {
    int64_t now_ms = esp_timer_get_time() / 1000;

    xSemaphoreTake(usage_mutex, portMAX_DELAY);
    bool timed_out = usage.online && (now_ms - last_serial_update_ms) > kOfflineTimeoutMs;
    if (timed_out) {
      usage.online = false;
      ESP_LOGW(TAG, "WiFi updates timed out");
    }
    xSemaphoreGive(usage_mutex);

    if (timed_out) {
      render_display();
    }

    vTaskDelay(pdMS_TO_TICKS(kWatchdogIntervalMs));
  }
}

void shtc3_task(void *) {
  while (true) {
    float temperature_c = 0.0f;
    float humidity_percent = 0.0f;
    if (read_shtc3(&temperature_c, &humidity_percent)) {
      bool should_render = false;
      xSemaphoreTake(usage_mutex, portMAX_DELAY);
      char temperature_text[16];
      char humidity_text[16];
      snprintf(temperature_text, sizeof(temperature_text), "%.1fC", temperature_c);
      snprintf(humidity_text, sizeof(humidity_text), "%.0f%%", humidity_percent);
      if (strcmp(usage.temperature_text, temperature_text) != 0) {
        strncpy(usage.temperature_text, temperature_text, sizeof(usage.temperature_text) - 1);
        usage.temperature_text[sizeof(usage.temperature_text) - 1] = '\0';
        should_render = true;
      }
      if (strcmp(usage.humidity_text, humidity_text) != 0) {
        strncpy(usage.humidity_text, humidity_text, sizeof(usage.humidity_text) - 1);
        usage.humidity_text[sizeof(usage.humidity_text) - 1] = '\0';
        should_render = true;
      }
      UsageData snapshot = usage;
      xSemaphoreGive(usage_mutex);

      ESP_LOGI(TAG, "SHTC3 update: temp=%s rh=%s", snapshot.temperature_text, snapshot.humidity_text);
      if (should_render) {
        render_display();
      }
    } else {
      ESP_LOGW(TAG, "SHTC3 read failed");
    }
    vTaskDelay(pdMS_TO_TICKS(kSensorRefreshMs));
  }
}

void clock_task(void *) {
  char last_clock_text[16] = "";

  while (true) {
    time_t now = 0;
    struct tm timeinfo = {};
    time(&now);
    localtime_r(&now, &timeinfo);

    if (timeinfo.tm_year >= (2024 - 1900)) {
      char clock_text[16];
      snprintf(clock_text, sizeof(clock_text), "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);

      bool should_render = false;
      xSemaphoreTake(usage_mutex, portMAX_DELAY);
      if (strcmp(usage.clock_text, clock_text) != 0) {
        strncpy(usage.clock_text, clock_text, sizeof(usage.clock_text) - 1);
        usage.clock_text[sizeof(usage.clock_text) - 1] = '\0';
        should_render = true;
      }
      xSemaphoreGive(usage_mutex);

      if (should_render && strcmp(last_clock_text, clock_text) != 0) {
        strncpy(last_clock_text, clock_text, sizeof(last_clock_text) - 1);
        last_clock_text[sizeof(last_clock_text) - 1] = '\0';
        ESP_LOGI(TAG, "Local clock updated: %s", last_clock_text);
        render_display();
      }
    }

    vTaskDelay(pdMS_TO_TICKS(kClockRefreshMs));
  }
}

void key_task(void *) {
  bool last_level = true;
  while (true) {
    bool level = gpio_get_level(kKeyButton);
    int64_t now_ms = esp_timer_get_time() / 1000;
    bool should_render = false;

    xSemaphoreTake(usage_mutex, portMAX_DELAY);
    if (last_level && !level && (now_ms - last_key_press_ms) >= kKeyDebounceMs) {
      show_usage_overlay = !show_usage_overlay;
      last_key_press_ms = now_ms;
      should_render = true;
    }
    xSemaphoreGive(usage_mutex);

    if (should_render) {
      render_display();
    }

    last_level = level;
    vTaskDelay(pdMS_TO_TICKS(kKeyPollIntervalMs));
  }
}

void battery_task(void *) {
  while (true) {
    float v = 0.0f;
    if (read_battery_voltage(&v)) {
      bool should_render = update_battery_state(v);
      UsageData snapshot;
      xSemaphoreTake(usage_mutex, portMAX_DELAY);
      snapshot = usage;
      xSemaphoreGive(usage_mutex);
      ESP_LOGI(TAG, "Battery: raw=%.2fV ema=%.2fV %d%% usb=%d", v,
               snapshot.battery_voltage, snapshot.battery_percent, snapshot.usb_connected);
      if (should_render) {
        render_display();
      }
    } else {
      ESP_LOGW(TAG, "Battery ADC read failed");
    }
    vTaskDelay(pdMS_TO_TICKS(kBattReadIntervalMs));
  }
}

// --- Initialization ---
void init_i2c_shtc3() {
  ESP_LOGI(TAG, "Creating I2C bus for SHTC3");
  i2c_master_bus_config_t bus_config = {};
  bus_config.i2c_port = I2C_NUM_0;
  bus_config.sda_io_num = kI2cSda;
  bus_config.scl_io_num = kI2cScl;
  bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
  bus_config.glitch_ignore_cnt = 7;
  bus_config.trans_queue_depth = 0;
  bus_config.flags.enable_internal_pullup = 1;
  if (i2c_new_master_bus(&bus_config, &i2c_bus) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to create I2C bus");
    return;
  }

  i2c_device_config_t dev_config = {};
  dev_config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  dev_config.device_address = kShtc3Address;
  dev_config.scl_speed_hz = 100000;
  if (i2c_master_bus_add_device(i2c_bus, &dev_config, &shtc3_dev) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to add SHTC3 device to I2C bus");
    shtc3_dev = nullptr;
  }
}

void init_key_button() {
  gpio_config_t io_conf = {};
  io_conf.mode = GPIO_MODE_INPUT;
  io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
  io_conf.pin_bit_mask = (1ULL << kKeyButton);
  ESP_ERROR_CHECK(gpio_config(&io_conf));
}

void init_battery_adc() {
  adc_oneshot_unit_init_cfg_t init_cfg = {};
  init_cfg.unit_id = ADC_UNIT_1;
  esp_err_t err = adc_oneshot_new_unit(&init_cfg, &adc_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to init ADC unit: %s", esp_err_to_name(err));
    adc_handle = nullptr;
    return;
  }

  adc_oneshot_chan_cfg_t chan_cfg = {};
  chan_cfg.bitwidth = ADC_BITWIDTH_12;
  chan_cfg.atten = ADC_ATTEN_DB_12;
  err = adc_oneshot_config_channel(adc_handle, kBattAdcChannel, &chan_cfg);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to config battery ADC channel: %s", esp_err_to_name(err));
    adc_oneshot_del_unit(adc_handle);
    adc_handle = nullptr;
    return;
  }

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
  adc_cali_curve_fitting_config_t cali_cfg = {};
  cali_cfg.unit_id = ADC_UNIT_1;
  cali_cfg.atten = ADC_ATTEN_DB_12;
  cali_cfg.bitwidth = ADC_BITWIDTH_12;
  if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &adc_cali_handle) != ESP_OK) {
    ESP_LOGW(TAG, "ADC calibration failed, using approximate conversion");
  }
#endif
  ESP_LOGI(TAG, "Battery ADC initialized on GPIO%d (ADC1_CH3, x%.2f)", kBattAdcPin,
           kBattDividerRatio);
}

// --- WiFi & TCP Client ---
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
    ESP_LOGW(TAG, "WiFi disconnected, reconnecting...");
    xEventGroupClearBits(s_wifi_event_group, kWifiConnectedBit);
    esp_wifi_connect();
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    ESP_LOGI(TAG, "WiFi connected, IP: " IPSTR, IP2STR(&event->ip_info.ip));
    init_time_sync();
    xEventGroupSetBits(s_wifi_event_group, kWifiConnectedBit);
  }
}

void init_wifi() {
  s_wifi_event_group = xEventGroupCreate();

  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_create_default_wifi_sta();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  esp_event_handler_instance_t instance_any_id;
  esp_event_handler_instance_t instance_got_ip;
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, nullptr, &instance_any_id));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, nullptr, &instance_got_ip));

  wifi_config_t wifi_config = {};
  strncpy((char *)wifi_config.sta.ssid, kWifiSsid, sizeof(wifi_config.sta.ssid) - 1);
  strncpy((char *)wifi_config.sta.password, kWifiPassword, sizeof(wifi_config.sta.password) - 1);
  wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
  ESP_ERROR_CHECK(esp_wifi_start());

  ESP_LOGI(TAG, "WiFi connecting to %s ...", kWifiSsid);
}

void tcp_client_task(void *) {
  char line[128];
  size_t index = 0;
  char rx_buf[256];

  while (true) {
    xEventGroupWaitBits(s_wifi_event_group, kWifiConnectedBit, pdFALSE, pdTRUE, portMAX_DELAY);

    char server_ip_str[INET_ADDRSTRLEN];
    struct addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *result = nullptr;
    if (getaddrinfo(kServerHost, nullptr, &hints, &result) == 0 && result != nullptr) {
      inet_ntoa_r(((struct sockaddr_in *)result->ai_addr)->sin_addr, server_ip_str, sizeof(server_ip_str));
      freeaddrinfo(result);
    } else {
      strncpy(server_ip_str, kServerHost, sizeof(server_ip_str) - 1);
      server_ip_str[sizeof(server_ip_str) - 1] = '\0';
    }

    ESP_LOGI(TAG, "TCP connecting to %s:%d...", server_ip_str, kServerPort);

    struct sockaddr_in dest_addr = {};
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(kServerPort);
    inet_pton(AF_INET, server_ip_str, &dest_addr.sin_addr);

    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sock < 0) {
      ESP_LOGE(TAG, "Socket create failed: errno %d", errno);
      vTaskDelay(pdMS_TO_TICKS(5000));
      continue;
    }

    if (connect(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) != 0) {
      ESP_LOGW(TAG, "TCP connect failed: errno %d, retrying...", errno);
      close(sock);
      vTaskDelay(pdMS_TO_TICKS(5000));
      continue;
    }

    ESP_LOGI(TAG, "TCP connected to %s:%d", server_ip_str, kServerPort);
    index = 0;

    while (true) {
      int len = recv(sock, rx_buf, sizeof(rx_buf) - 1, 0);
      if (len <= 0) {
        ESP_LOGW(TAG, "TCP disconnected (len=%d, errno=%d)", len, errno);
        break;
      }

      for (int i = 0; i < len; ++i) {
        char ch = rx_buf[i];
        if (ch == '\r' || ch == '\n') {
          if (index == 0) continue;
          line[index] = '\0';
          parse_usage_line(line);
          index = 0;
        } else if (index + 1 < sizeof(line)) {
          line[index++] = ch;
        } else {
          index = 0;
          ESP_LOGW(TAG, "Dropped oversized line");
        }
      }
    }

    close(sock);
    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}

}  // namespace

extern "C" void app_main(void) {
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  usage_mutex = xSemaphoreCreateMutex();
  display_mutex = xSemaphoreCreateMutex();
  if (usage_mutex == nullptr || display_mutex == nullptr) {
    abort();
  }

  static RlcdSt7305 rlcd({
      .mosi = GPIO_NUM_12,
      .clk = GPIO_NUM_11,
      .cs = GPIO_NUM_40,
      .dc = GPIO_NUM_5,
      .reset = GPIO_NUM_41,
  });
  display = &rlcd;

  ESP_ERROR_CHECK(display->Init());
  render_display();
  init_i2c_shtc3();
  init_key_button();
  init_battery_adc();
  init_wifi();

  xTaskCreate(tcp_client_task, "tcp_client_task", 8192, nullptr, 5, nullptr);
  xTaskCreate(offline_watchdog_task, "offline_watchdog_task", 4096, nullptr, 4, nullptr);
  xTaskCreate(shtc3_task, "shtc3_task", 4096, nullptr, 4, nullptr);
  xTaskCreate(clock_task, "clock_task", 4096, nullptr, 4, nullptr);
  xTaskCreate(key_task, "key_task", 4096, nullptr, 3, nullptr);
  xTaskCreate(battery_task, "battery_task", 4096, nullptr, 3, nullptr);
  ESP_LOGI(TAG, "WiFi mode starting...");
}
