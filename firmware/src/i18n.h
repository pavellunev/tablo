// Строки, которые устройство рисует на панели, и причины отказов источников —
// одна таблица на язык. Без зависимостей от Arduino (только <cstdint>): модуль
// собирается и в прошивке, и в хостовых тестах, и в render_frame.
//
// Язык — один на всё устройство (config::Settings::lang), по умолчанию en.
// Строки с форматом (%d, %u, %s) — это format-строки: подставляет snprintf на
// месте вызова.
#pragma once

#include <cstdint>

namespace i18n {

enum class Lang : uint8_t { kEn = 0, kRu = 1 };

enum class Str : uint8_t {
    // заголовки блоков и подписи
    kMarkets,
    kLimitsRemaining,
    kOfficeAir,
    kMail,
    kToday,
    kUnreadFmt,     // "%d UNREAD"
    kFiveHours,     // подпись строки лимита
    kWeek,
    kLast24h,
    kVentilate,
    kFresh,
    kNormal,
    kNoData,
    kYesterday,
    kPerHourFmt,    // "%s%.0f/h": стрелка, величина
    // сброс лимитов
    kFiveHourReset,  // префикс: "5h in " + таймер
    kWeekReset,      // префикс: "week in " + таймер
    kCodexResetFmt,  // "week · reset %02d:%02d · in %s"
    kDaysHoursFmt,   // "%ud %uh"
    // погода (WMO)
    kWmoClear,
    kWmoCloudy,
    kWmoPartlyCloudy,
    kWmoFog,
    kWmoRain,
    kWmoSnow,
    kWmoStorm,
    // экраны включения и точки доступа
    kBootStatus,
    kTagline,
    kApSetup,
    kNetwork,
    kPassword,
    kApHint1,
    kApHint2,
    // причины отказов
    kSourceNoAnswer,
    kSourceNoAnswerCodeFmt,  // "... (code %d)"
    kNoSensorAnswer,
    kParseFailed,
    kRateLimited,
    kRegionBlocked,
    kTokenRejected,
    kTokenRefreshPostponed,
    kTokenNoRefresh,
    kTokenRefreshFailed,
    kMailNoAnswer,
    kNoMessage,
    kImapNoTls,
    kImapLoginRejected,
    kImapSelectFailed,
    kImapSearchFailed,
    kCityNotSet,
    kCount
};

void set_lang(Lang lang);
Lang lang();
// "ru" -> kRu, всё остальное -> kEn.
Lang from_code(const char* code);
const char* code(Lang lang);

const char* tr(Str s);
// 0 = ПН/MON.
const char* weekday(int wday);
// 0 = ЯНВАРЯ/JANUARY.
const char* month(int month0);

}  // namespace i18n
