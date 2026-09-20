// Минимальная замена Arduino String для сборки wifi_qr.cpp под native.
//
// Копия шима из test_config/Arduino.h (см. комментарий там) — wifi_qr.cpp
// использует только конструктор из const char*/std::string и c_str(),
// конкатенации нет: строка собирается через std::string и оборачивается в
// String один раз на границе (см. комментарий в самом wifi_qr.cpp).
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
