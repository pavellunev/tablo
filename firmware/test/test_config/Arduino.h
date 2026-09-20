// Минимальная замена Arduino String для сборки config.cpp под native.
//
// Копия шима из test_slots/Arduino.h (см. комментарий там про то, почему это
// не подключает прошивку устройства) плюс isEmpty() — from_json/to_json
// используют его на каждом поле, чтобы отличить «прислали пусто» от «не
// прислали вовсе».
#pragma once

#include <cstdint>
#include <string>

class String {
   public:
    String() = default;
    String(const char* s) : value_(s ? s : "") {}
    String(const std::string& s) : value_(s) {}

    const char* c_str() const { return value_.c_str(); }
    size_t length() const { return value_.size(); }
    bool isEmpty() const { return value_.empty(); }

    bool operator==(const String& other) const { return value_ == other.value_; }
    bool operator!=(const String& other) const { return value_ != other.value_; }
    bool operator<(const String& other) const { return value_ < other.value_; }

   private:
    std::string value_;
};
