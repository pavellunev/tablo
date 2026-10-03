#include "i18n.h"

#include <cstring>

namespace i18n {

namespace {

Lang g_lang = Lang::kEn;

constexpr int kStrCount = static_cast<int>(Str::kCount);

// Порядок строго как в enum Str.
const char* const kEn[kStrCount] = {
    "MARKETS",
    "LIMITS · REMAINING",
    "OFFICE · AIR",
    "MAIL",
    "TODAY",
    "%d UNREAD",
    "5H",
    "WEEK",
    "LAST 24H",
    "VENTILATE",
    "FRESH",
    "OK",
    "no data",
    "yesterday",
    "%s%.0f/h",
    "5h in ",
    "week in ",
    "week · reset %02d:%02d · in %s",
    "%ud %uh",
    "CLEAR",
    "CLOUDY",
    "PARTLY CLOUDY",
    "FOG",
    "RAIN",
    "SNOW",
    "STORM",
    "starting · looking for a saved network…",
    "SELF-CONTAINED E-INK DASHBOARD",
    "TABLO SETUP",
    "NETWORK",
    "PASSWORD",
    "scan the QR with your phone camera to join the network,",
    "if it fails, enter the network and password manually.",
    "source did not respond",
    "source did not respond (code %d)",
    "no sensor responded",
    "could not parse response",
    "server asks to wait (429)",
    "not available from this region",
    "token rejected — sign in to Codex again",
    "token expired, refresh postponed",
    "token expired — set a refresh token on the setup page",
    "token expired — refresh failed, retry in an hour",
    "mail server did not respond",
    "no message",
    "imap: no TLS connection (network, certificate or timeout)",
    "imap: LOGIN rejected — check the address and app password",
    "imap: SELECT INBOX failed",
    "imap: SEARCH UNSEEN failed",
    "city not set",
};

const char* const kRu[kStrCount] = {
    "РЫНКИ",
    "ЛИМИТЫ · ОСТАТОК",
    "КАБИНЕТ · ВОЗДУХ",
    "ПОЧТА",
    "СЕГОДНЯ",
    "%d НЕПРОЧИТАННЫХ",
    "5 Ч",
    "НЕДЕЛЯ",
    "ЗА 24 Ч",
    "ПРОВЕТРИТЬ",
    "СВЕЖО",
    "НОРМА",
    "нет данных",
    "Вчера",
    "%s%.0f/ч",
    "5 ч через ",
    "неделя через ",
    "неделя · сброс %02d:%02d · через %s",
    "%u дн %u ч",
    "ЯСНО",
    "ОБЛАЧНО",
    "ПЕР. ОБЛ.",
    "ТУМАН",
    "ДОЖДЬ",
    "СНЕГ",
    "ГРОЗА",
    "включение · ищем сохранённую сеть…",
    "АВТОНОМНЫЙ E-INK ДАШБОРД",
    "НАСТРОЙКА TABLO",
    "СЕТЬ",
    "ПАРОЛЬ",
    "наведите камеру телефона на QR — сеть добавится сама,",
    "не считалось — введите сеть и пароль вручную.",
    "источник не ответил",
    "источник не ответил (код %d)",
    "ни один датчик не ответил",
    "не удалось разобрать ответ",
    "сервер просит подождать (429)",
    "недоступен из этого региона",
    "токен отвергнут — войдите в Codex заново",
    "токен протух, обновление отложено",
    "токен протух — задайте refresh-токен на странице настройки",
    "токен протух — обновить не удалось, повтор через час",
    "почтовый сервер не ответил",
    "нет письма",
    "imap: нет TLS-соединения (сеть, сертификат или таймаут)",
    "imap: LOGIN отвергнут — проверьте адрес и пароль приложения",
    "imap: SELECT INBOX не удался",
    "imap: SEARCH UNSEEN не удался",
    "город не задан",
};

const char* const kWeekdaysEn[7] = {"MON", "TUE", "WED", "THU", "FRI", "SAT", "SUN"};
const char* const kWeekdaysRu[7] = {"ПН", "ВТ", "СР", "ЧТ", "ПТ", "СБ", "ВС"};

const char* const kMonthsEn[12] = {"JANUARY", "FEBRUARY", "MARCH",     "APRIL",   "MAY",      "JUNE",
                                   "JULY",    "AUGUST",   "SEPTEMBER", "OCTOBER", "NOVEMBER", "DECEMBER"};
const char* const kMonthsRu[12] = {"ЯНВАРЯ", "ФЕВРАЛЯ", "МАРТА",    "АПРЕЛЯ", "МАЯ",    "ИЮНЯ",
                                   "ИЮЛЯ",   "АВГУСТА", "СЕНТЯБРЯ", "ОКТЯБРЯ", "НОЯБРЯ", "ДЕКАБРЯ"};

}  // namespace

void set_lang(Lang lang) { g_lang = lang; }

Lang lang() { return g_lang; }

Lang from_code(const char* code) {
    return code != nullptr && std::strcmp(code, "ru") == 0 ? Lang::kRu : Lang::kEn;
}

const char* code(Lang lang) { return lang == Lang::kRu ? "ru" : "en"; }

const char* tr(Str s) {
    const int i = static_cast<int>(s);
    if (i < 0 || i >= kStrCount) return "";
    return g_lang == Lang::kRu ? kRu[i] : kEn[i];
}

const char* weekday(int wday) {
    if (wday < 0 || wday > 6) return "";
    return g_lang == Lang::kRu ? kWeekdaysRu[wday] : kWeekdaysEn[wday];
}

const char* month(int month0) {
    if (month0 < 0 || month0 > 11) return "";
    return g_lang == Lang::kRu ? kMonthsRu[month0] : kMonthsEn[month0];
}

}  // namespace i18n
