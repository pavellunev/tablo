#!/usr/bin/env bash
# Пересобирает x509_crt_bundle.bin — набор корневых сертификатов, который
# firmware/src/connectors.cpp подключает через WiFiClientSecure::setCACertBundle()
# (см. docs/decisions.md, п.9). Бинарник закоммичен как обычный файл: гонять
# python с внешними зависимостями на каждой сборке прошивки незачем, а
# формат бандла (число сертификатов + для каждого длины/имя/публичный ключ,
# отсортировано по subject name для бинарного поиска) стабилен уже много лет.
#
# cacrt_all.pem — курируемый Espressif набор корневых центров сертификации
# (тот же файл, что использует ESP-IDF для CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL),
# взят из https://github.com/espressif/esp-idf/blob/master/components/mbedtls/esp_crt_bundle/cacrt_all.pem
#
# gen_crt_bundle.py — генератор бандла из того же коммита arduino-esp32, что
# зафиксирован в platformio.ini (framework-arduinoespressif32, см. .piopm),
# взят из https://github.com/espressif/arduino-esp32/blob/dcc1105b/tools/gen_crt_bundle.py
# Формат бинарника, который он производит, разбирает
# libraries/WiFiClientSecure/src/esp_crt_bundle.c из того же core.
#
# Обновить набор сертификатов: скачать свежий cacrt_all.pem, положить сюда и
# перезапустить этот скрипт.

set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

python3 -m pip show cryptography >/dev/null 2>&1 || python3 -m pip install --user cryptography

python3 gen_crt_bundle.py -i cacrt_all.pem
mv x509_crt_bundle x509_crt_bundle.bin
echo "готово: $(pwd)/x509_crt_bundle.bin ($(wc -c < x509_crt_bundle.bin) байт)"
