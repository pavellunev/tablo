#include "battery.h"

#include <Arduino.h>

#include "board.h"

namespace battery {

void begin() {
    pinMode(board::BATTERY_ENABLE, OUTPUT);
    digitalWrite(board::BATTERY_ENABLE, LOW);  // делитель обесточен по умолчанию — см. board.h
    // ADC_11db (~2.5 В полной шкалы на старых SDK, у современных соответствует
    // тому же диапазону, что в trmnl-ink называют 12 дБ) — без него дефолтное
    // затухание не покрывает делённое напряжение полного заряда.
    analogSetPinAttenuation(board::BATTERY_ADC, ADC_11db);
}

int8_t percent() {
    // Делитель на этом ките — 1:2, подтверждено на живом устройстве в
    // предшественнике (trmnl-ink/firmware/trmnl-ink.yaml: "делитель 1:2 на
    // плате кита", тот же физический кит). Включаем на время замера — держать
    // цепь постоянно открытой тратит ток без нужды (тот же приём, что там же).
    digitalWrite(board::BATTERY_ENABLE, HIGH);
    delay(120);  // тому же значению следует и предшественник — делителю нужно
                 // время устояться после включения
    uint32_t millivolts = analogReadMilliVolts(board::BATTERY_ADC);
    digitalWrite(board::BATTERY_ENABLE, LOW);

    float battery_volts = (millivolts / 1000.0f) * 2.0f;

    float span = board::BATTERY_FULL_VOLTS - board::BATTERY_EMPTY_VOLTS;
    float pct = (battery_volts - board::BATTERY_EMPTY_VOLTS) / span * 100.0f;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return static_cast<int8_t>(pct + 0.5f);
}

}  // namespace battery
