// FILETIME (centaines de ns depuis le 1er janvier 1601, UTC) <-> TIME_FIELDS (RtlTimeToTimeFields / RtlTimeFieldsToTime).
#pragma once
#include <cstdint>

namespace wk {

struct TimeFields { int16_t year, month, day, hour, minute, second, milliseconds, weekday; };   // weekday : 0 = dimanche

// Jours depuis le 1er janvier 1601 -> date grégorienne (algorithme « civil from days », ère de 400 ans).
inline TimeFields FileTimeToFields(uint64_t t) {
    const uint64_t ms = t / 10000, days = ms / 86400000;
    const uint32_t inDay = uint32_t(ms % 86400000);
    TimeFields f{};
    f.milliseconds = int16_t(inDay % 1000);
    f.second = int16_t(inDay / 1000 % 60);
    f.minute = int16_t(inDay / 60000 % 60);
    f.hour = int16_t(inDay / 3600000);
    f.weekday = int16_t((days + 1) % 7);   // 1er janvier 1601 : lundi
    // Décalage vers le 1er mars 1600 (début d'ère) : 306 jours avant le 1er janvier 1601.
    const uint64_t z = days + 306;
    const uint64_t era = z / 146097, doe = z % 146097;
    const uint64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const uint64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    const uint32_t d = uint32_t(doy - (153 * mp + 2) / 5 + 1), m = uint32_t(mp < 10 ? mp + 3 : mp - 9);
    f.year = int16_t(1600 + era * 400 + yoe + (m <= 2 ? 1 : 0));
    f.month = int16_t(m);
    f.day = int16_t(d);
    return f;
}

// Faux si un champ est hors limites (année < 1601 comprise), comme RtlTimeFieldsToTime.
inline bool FieldsToFileTime(const TimeFields& f, uint64_t* out) {
    if (f.year < 1601 || f.month < 1 || f.month > 12 || f.hour < 0 || f.hour > 23 || f.minute < 0 || f.minute > 59 ||
        f.second < 0 || f.second > 59 || f.milliseconds < 0 || f.milliseconds > 999 || f.day < 1)
        return false;
    const bool leap = (f.year % 4 == 0 && f.year % 100 != 0) || f.year % 400 == 0;
    static const int kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (f.day > kDays[f.month - 1] + (f.month == 2 && leap ? 1 : 0)) return false;
    const int64_t y = f.year - (f.month <= 2 ? 1 : 0), m = f.month;
    const int64_t era = y / 400, yoe = y - era * 400;
    const int64_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + f.day - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const int64_t days = era * 146097 + doe - (146097 * 4 + 306);   // jours depuis le 1er janvier 1601
    const int64_t ms = ((days * 24 + f.hour) * 60 + f.minute) * 60000LL + f.second * 1000LL + f.milliseconds;
    *out = uint64_t(ms) * 10000;
    return true;
}

}  // namespace wk
