// Минимальная замена Arduino String для сборки под native.
//
// slots.h и config.h подключают <Arduino.h> безусловно — на устройстве это
// настоящий фреймворк, на хосте такого заголовка нет вовсе. Класс String
// здесь — это всё, что реально используется в коде, разобранном тестами:
// сравнение, c_str(), ключ std::map. Конкатенация (+) сюда намеренно не
// добавлена — там, где она нужна (connectors.cpp), строки собираются через
// std::string и оборачиваются в String один раз на границе.
//
// PlatformIO подключает этот файл только при сборке теста test_slots: каталог
// теста добавляется в путь поиска заголовков исключительно для его сборки, на
// прошивку устройства (env:xiao-esp32s3) это никак не влияет — проверено
// эмпирически (см. Status Log плана).
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

    bool operator==(const String& other) const { return value_ == other.value_; }
    bool operator!=(const String& other) const { return value_ != other.value_; }
    bool operator<(const String& other) const { return value_ < other.value_; }

   private:
    std::string value_;
};
