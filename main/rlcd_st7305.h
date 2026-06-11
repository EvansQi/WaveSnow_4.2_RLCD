#pragma once

#include <stdint.h>
#include <stddef.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"

struct RlcdPins {
  gpio_num_t mosi;
  gpio_num_t clk;
  gpio_num_t cs;
  gpio_num_t dc;
  gpio_num_t reset;
};

class RlcdSt7305 {
 public:
  static constexpr int kWidth = 400;
  static constexpr int kHeight = 300;
  static constexpr size_t kBufferSize = (kWidth * kHeight) / 8;

  explicit RlcdSt7305(const RlcdPins &pins);
  esp_err_t Init();
  void Clear(bool black);
  void DrawPixel(int x, int y, bool black);
  void DrawRect(int x, int y, int w, int h, bool black);
  void FillRect(int x, int y, int w, int h, bool black);
  void DrawText(int x, int y, const char *text, int scale, bool black);
  void Present();

 private:
  void HardwareReset();
  void InitDisplay();
  void SendCommand(uint8_t cmd);
  void SendData(uint8_t data);
  void SendDataBuffer(const uint8_t *data, size_t len);
  void DrawChar(int x, int y, char c, int scale, bool black);

  RlcdPins pins_;
  spi_device_handle_t spi_{nullptr};
  uint8_t *buffer_{nullptr};
};
