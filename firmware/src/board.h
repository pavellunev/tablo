// Распиновка TRMNL 7.5" DIY Kit.
//
// Значения сняты с живого устройства, а не из документации кита: заявленный
// там пин батареи читает ноль, потому что делитель обесточен, пока не поднят
// BATTERY_ENABLE. Проверено замером — 4.136 В соответствует полному заряду.
#pragma once

#include <cstdint>

namespace board {

// ── панель ──
constexpr int8_t EPD_CS = 44;
constexpr int8_t EPD_DC = 10;
constexpr int8_t EPD_RESET = 38;
constexpr int8_t EPD_BUSY = 4;  // инвертирован: панель занята при HIGH

constexpr int8_t SPI_CLK = 7;
constexpr int8_t SPI_MOSI = 9;

constexpr uint16_t SCREEN_WIDTH = 800;
constexpr uint16_t SCREEN_HEIGHT = 480;

// ── батарея ──
// Делитель обесточен, пока BATTERY_ENABLE не поднят: без этого ADC читает
// ноль, а вычисленное по нему напряжение уходит в абсурд (в предшественнике
// на экране было «−363 В»).
constexpr int8_t BATTERY_ENABLE = 6;
constexpr int8_t BATTERY_ADC = 1;

// Замеренная точка калибровки: полный заряд Li-Po этого кита.
constexpr float BATTERY_FULL_VOLTS = 4.136f;
constexpr float BATTERY_EMPTY_VOLTS = 3.30f;

}  // namespace board
