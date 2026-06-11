#include "rlcd_st7305.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

static const char *TAG = "rlcd";

struct Glyph {
  char c;
  uint8_t rows[7];
};

static const Glyph kGlyphs[] = {
    {' ', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {'%', {0x19, 0x19, 0x02, 0x04, 0x08, 0x13, 0x13}},
    {'-', {0x00, 0x00, 0x00, 0x1f, 0x00, 0x00, 0x00}},
    {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x0c}},
    {'/', {0x01, 0x02, 0x04, 0x08, 0x10, 0x00, 0x00}},
    {'0', {0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e}},
    {'1', {0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e}},
    {'2', {0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f}},
    {'3', {0x1e, 0x01, 0x01, 0x0e, 0x01, 0x01, 0x1e}},
    {'4', {0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02}},
    {'5', {0x1f, 0x10, 0x10, 0x1e, 0x01, 0x01, 0x1e}},
    {'6', {0x06, 0x08, 0x10, 0x1e, 0x11, 0x11, 0x0e}},
    {'7', {0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e}},
    {'9', {0x0e, 0x11, 0x11, 0x0f, 0x01, 0x02, 0x0c}},
    {':', {0x00, 0x0c, 0x0c, 0x00, 0x0c, 0x0c, 0x00}},
    {'A', {0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11}},
    {'C', {0x0e, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0e}},
    {'D', {0x1c, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1c}},
    {'E', {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f}},
    {'F', {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x10}},
    {'G', {0x0e, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0f}},
    {'H', {0x11, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11}},
    {'I', {0x0e, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0e}},
    {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}},
    {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f}},
    {'M', {0x11, 0x1b, 0x15, 0x15, 0x11, 0x11, 0x11}},
    {'N', {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11}},
    {'O', {0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e}},
    {'P', {0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10}},
    {'R', {0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11}},
    {'S', {0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e}},
    {'T', {0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e}},
    {'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0a}},
    {'X', {0x11, 0x11, 0x0a, 0x04, 0x0a, 0x11, 0x11}},
    {'Y', {0x11, 0x11, 0x0a, 0x04, 0x04, 0x04, 0x04}},
};

const Glyph *FindGlyph(char c) {
  char upper = static_cast<char>(toupper(static_cast<unsigned char>(c)));
  for (const auto &glyph : kGlyphs) {
    if (glyph.c == upper) {
      return &glyph;
    }
  }
  return &kGlyphs[0];
}

}  // namespace

RlcdSt7305::RlcdSt7305(const RlcdPins &pins) : pins_(pins) {}

esp_err_t RlcdSt7305::Init() {
  buffer_ = static_cast<uint8_t *>(
      heap_caps_malloc(kBufferSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (buffer_ == nullptr) {
    buffer_ = static_cast<uint8_t *>(heap_caps_malloc(kBufferSize, MALLOC_CAP_8BIT));
  }
  if (buffer_ == nullptr) {
    return ESP_ERR_NO_MEM;
  }

  gpio_config_t io_conf = {};
  io_conf.mode = GPIO_MODE_OUTPUT;
  io_conf.pin_bit_mask = (1ULL << pins_.dc) | (1ULL << pins_.reset);
  ESP_ERROR_CHECK(gpio_config(&io_conf));

  spi_bus_config_t buscfg = {};
  buscfg.sclk_io_num = pins_.clk;
  buscfg.mosi_io_num = pins_.mosi;
  buscfg.miso_io_num = -1;
  buscfg.quadwp_io_num = -1;
  buscfg.quadhd_io_num = -1;
  buscfg.max_transfer_sz = static_cast<int>(kBufferSize + 16);

  spi_device_interface_config_t devcfg = {};
  devcfg.clock_speed_hz = 4 * 1000 * 1000;
  devcfg.mode = 0;
  devcfg.spics_io_num = pins_.cs;
  devcfg.queue_size = 1;

  ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));
  ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &devcfg, &spi_));

  Clear(false);
  HardwareReset();
  InitDisplay();
  Present();
  return ESP_OK;
}

void RlcdSt7305::Clear(bool black) {
  memset(buffer_, black ? 0x00 : 0xFF, kBufferSize);
}

void RlcdSt7305::DrawPixel(int x, int y, bool black) {
  if (x < 0 || y < 0 || x >= kWidth || y >= kHeight) {
    return;
  }

  const int inv_y = kHeight - 1 - y;
  const int block_y = inv_y >> 2;
  const int local_y = inv_y & 3;
  const int byte_x = x >> 1;
  const int local_x = x & 1;
  const size_t buffer_idx = static_cast<size_t>(byte_x) * (kHeight >> 2) + block_y;
  const uint8_t bit = 7 - ((local_y << 1) | local_x);
  const uint8_t mask = static_cast<uint8_t>(1U << bit);

  if (black) {
    buffer_[buffer_idx] &= static_cast<uint8_t>(~mask);
  } else {
    buffer_[buffer_idx] |= mask;
  }
}

void RlcdSt7305::DrawRect(int x, int y, int w, int h, bool black) {
  for (int i = 0; i < w; ++i) {
    DrawPixel(x + i, y, black);
    DrawPixel(x + i, y + h - 1, black);
  }
  for (int i = 0; i < h; ++i) {
    DrawPixel(x, y + i, black);
    DrawPixel(x + w - 1, y + i, black);
  }
}

void RlcdSt7305::FillRect(int x, int y, int w, int h, bool black) {
  for (int yy = 0; yy < h; ++yy) {
    for (int xx = 0; xx < w; ++xx) {
      DrawPixel(x + xx, y + yy, black);
    }
  }
}

void RlcdSt7305::DrawText(int x, int y, const char *text, int scale, bool black) {
  int cursor_x = x;
  for (size_t i = 0; text[i] != '\0'; ++i) {
    DrawChar(cursor_x, y, text[i], scale, black);
    cursor_x += 6 * scale;
  }
}

void RlcdSt7305::Present() {
  SendCommand(0x38);
  SendCommand(0x29);
  SendCommand(0x2A);
  SendData(0x12);
  SendData(0x2A);
  SendCommand(0x2B);
  SendData(0x00);
  SendData(0xC7);
  SendCommand(0x2C);
  SendDataBuffer(buffer_, kBufferSize);
}

void RlcdSt7305::HardwareReset() {
  gpio_set_level(pins_.reset, 1);
  vTaskDelay(pdMS_TO_TICKS(50));
  gpio_set_level(pins_.reset, 0);
  vTaskDelay(pdMS_TO_TICKS(20));
  gpio_set_level(pins_.reset, 1);
  vTaskDelay(pdMS_TO_TICKS(50));
}

void RlcdSt7305::InitDisplay() {
  const uint8_t init_seq[][2] = {
      {0xD6, 0x17}, {0xD1, 0x01}, {0xC0, 0x11}, {0xB2, 0x02}, {0xB7, 0x13},
      {0xB0, 0x64}, {0xC9, 0x00}, {0x36, 0x48}, {0x3A, 0x11}, {0xB9, 0x20},
      {0xB8, 0x29}, {0x21, 0x00}, {0x35, 0x00}, {0xD0, 0xFF}, {0x38, 0x00},
      {0x29, 0x00},
  };

  SendCommand(0xD6);
  SendData(0x17);
  SendData(0x02);
  SendCommand(0xD1);
  SendData(0x01);
  SendCommand(0xC0);
  SendData(0x11);
  SendData(0x04);
  SendCommand(0xC1);
  for (int i = 0; i < 4; ++i) SendData(0x69);
  SendCommand(0xC2);
  for (int i = 0; i < 4; ++i) SendData(0x19);
  SendCommand(0xC4);
  for (int i = 0; i < 4; ++i) SendData(0x4B);
  SendCommand(0xC5);
  for (int i = 0; i < 4; ++i) SendData(0x19);
  SendCommand(0xD8);
  SendData(0x80);
  SendData(0xE9);
  SendCommand(0xB2);
  SendData(0x02);
  SendCommand(0xB3);
  const uint8_t b3[] = {0xE5, 0xF6, 0x05, 0x46, 0x77, 0x77, 0x77, 0x77, 0x76, 0x45};
  for (uint8_t value : b3) SendData(value);
  SendCommand(0xB4);
  const uint8_t b4[] = {0x05, 0x46, 0x77, 0x77, 0x77, 0x77, 0x76, 0x45};
  for (uint8_t value : b4) SendData(value);
  SendCommand(0x62);
  SendData(0x32);
  SendData(0x03);
  SendData(0x1F);
  SendCommand(0xB7);
  SendData(0x13);
  SendCommand(0xB0);
  SendData(0x64);
  SendCommand(0x11);
  vTaskDelay(pdMS_TO_TICKS(200));
  for (const auto &item : init_seq) {
    SendCommand(item[0]);
    if (item[0] != 0x21 && item[0] != 0x29 && item[0] != 0x38) {
      SendData(item[1]);
    }
  }
  SendCommand(0x2A);
  SendData(0x12);
  SendData(0x2A);
  SendCommand(0x2B);
  SendData(0x00);
  SendData(0xC7);
}

void RlcdSt7305::SendCommand(uint8_t cmd) {
  gpio_set_level(pins_.dc, 0);
  spi_transaction_t t = {};
  t.length = 8;
  t.tx_buffer = &cmd;
  ESP_ERROR_CHECK(spi_device_polling_transmit(spi_, &t));
}

void RlcdSt7305::SendData(uint8_t data) {
  gpio_set_level(pins_.dc, 1);
  spi_transaction_t t = {};
  t.length = 8;
  t.tx_buffer = &data;
  ESP_ERROR_CHECK(spi_device_polling_transmit(spi_, &t));
}

void RlcdSt7305::SendDataBuffer(const uint8_t *data, size_t len) {
  gpio_set_level(pins_.dc, 1);
  spi_transaction_t t = {};
  t.length = static_cast<uint32_t>(len * 8);
  t.tx_buffer = data;
  ESP_ERROR_CHECK(spi_device_polling_transmit(spi_, &t));
}

void RlcdSt7305::DrawChar(int x, int y, char c, int scale, bool black) {
  const Glyph *glyph = FindGlyph(c);
  for (int row = 0; row < 7; ++row) {
    for (int col = 0; col < 5; ++col) {
      if ((glyph->rows[row] >> (4 - col)) & 0x01) {
        FillRect(x + col * scale, y + row * scale, scale, scale, black);
      }
    }
  }
}
