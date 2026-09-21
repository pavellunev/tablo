// Закреплённые корневые сертификаты для узлов, у которых общий
// x509_crt_bundle.bin систематически не проходит проверку (docs/decisions.md,
// п.9). Диагноз: esp_crt_bundle.c ищет корень бинарным поиском по DER-имени
// эмитента и сам вызывает mbedtls_pk_verify_ext на распакованном ключе, в
// обход обычного mbedtls_x509_crt_verify_chain(); у корней Google Trust
// Services (R1/R4) этот путь стабильно ломается — на этом уже упал
// CoinGecko (см. Status Log), затем api.anthropic.com/chatgpt.com/
// imap.gmail.com живой проверкой на устройстве. Корень не просрочен и
// закодирован стандартно — разбирать сам механизм бандла дальше дороже,
// чем закрепить корень явно там, где он ломается.
//
// setCACert() ниже — обычный путь mbedTLS (mbedtls_ssl_conf_ca_chain):
// полная проверка цепочки и hostname, просто с одним заранее известным
// доверенным корнем вместо поиска по общему набору из 121 сертификата.
//
// Обязательство, которое это на нас накладывает: когда Google сменит корень
// (не раньше 2036 — см. NotAfter в самих сертификатах и
// firmware/certs/gts_root_r{1,4}.pem), эти узлы перестанут проходить
// проверку и потребуют новой прошивки с обновлённым PEM. Устройство не
// обновляет прошивку по воздуху само (OTA — фаза 4, ещё не сделана), так
// что это ручная операция, а не то, что починится само собой.
#pragma once

// GTS Root R4 — api.anthropic.com и chatgpt.com оба цепляются к нему через
// промежуточный сертификат WE1 (проверено вручную: `openssl s_client -connect
// api.anthropic.com:443 -showcerts`). Исходник — firmware/certs/gts_root_r4.pem.
constexpr const char* kGtsRootR4Pem =
    "-----BEGIN CERTIFICATE-----\n"
    "MIICCTCCAY6gAwIBAgINAgPlwGjvYxqccpBQUjAKBggqhkjOPQQDAzBHMQswCQYDVQQGEwJVUzEi\n"
    "MCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjQw\n"
    "HhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAwMDAwWjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZ\n"
    "R29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjQwdjAQBgcqhkjO\n"
    "PQIBBgUrgQQAIgNiAATzdHOnaItgrkO4NcWBMHtLSZ37wWHO5t5GvWvVYRg1rkDdc/eJkTBa6zzu\n"
    "hXyiQHY7qca4R9gq55KRanPpsXI5nymfopjTX15YhmUPoYRlBtHci8nHc8iMai/lxKvRHYqjQjBA\n"
    "MA4GA1UdDwEB/wQEAwIBhjAPBgNVHRMBAf8EBTADAQH/MB0GA1UdDgQWBBSATNbrdP9JNqPV2Py1\n"
    "PsVq8JQdjDAKBggqhkjOPQQDAwNpADBmAjEA6ED/g94D9J+uHXqnLrmvT/aDHQ4thQEd0dlq7A/C\n"
    "r8deVl5c1RxYIigL9zC2L7F8AjEA8GE8p/SgguMh1YQdc4acLa/KNJvxn7kjNuK8YAOdgLOaVsjh\n"
    "4rsUecrNIdSUtUlD\n"
    "-----END CERTIFICATE-----\n";

// GTS Root R1 — imap.gmail.com цепляется к нему через промежуточный WR2.
// Исходник — firmware/certs/gts_root_r1.pem.
constexpr const char* kGtsRootR1Pem =
    "-----BEGIN CERTIFICATE-----\n"
    "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\n"
    "gm3c\n"
    "-----END CERTIFICATE-----\n";
