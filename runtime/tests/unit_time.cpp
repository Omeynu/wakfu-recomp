// Test des conversions FILETIME <-> TIME_FIELDS (kernel/time_fields.h).
#include <cstdio>
#include "../src/kernel/time_fields.h"
static int g_fail = 0;
static void Expect(const char* what, long long got, long long want) {
    if (got != want) { printf("ÉCHEC %s : %lld au lieu de %lld\n", what, got, want); g_fail++; }
}
int main() {
    const uint64_t kUnixEpoch = 116444736000000000ull;   // 1970-01-01 00:00:00
    wk::TimeFields f = wk::FileTimeToFields(0);
    Expect("1601 année", f.year, 1601); Expect("1601 mois", f.month, 1); Expect("1601 jour", f.day, 1);
    Expect("1601 jour de semaine (lundi)", f.weekday, 1);
    f = wk::FileTimeToFields(kUnixEpoch);
    Expect("1970 année", f.year, 1970); Expect("1970 mois", f.month, 1); Expect("1970 jour", f.day, 1);
    Expect("1970 jour de semaine (jeudi)", f.weekday, 4);
    // 2024-02-29 13:45:30.250 (bissextile) = 1709214330 s Unix
    const uint64_t t = kUnixEpoch + 1709214330ull * 10000000 + 2500000;
    f = wk::FileTimeToFields(t);
    Expect("2024 année", f.year, 2024); Expect("2024 mois", f.month, 2); Expect("2024 jour", f.day, 29);
    Expect("heure", f.hour, 13); Expect("minute", f.minute, 45); Expect("seconde", f.second, 30);
    Expect("ms", f.milliseconds, 250); Expect("2024-02-29 jeudi", f.weekday, 4);
    uint64_t back = 0;
    Expect("aller-retour accepté", wk::FieldsToFileTime(f, &back), 1);
    Expect("aller-retour", (long long)back, (long long)t);
    // 2000-12-31 (fin d'année bissextile séculaire)
    wk::TimeFields g{2000, 12, 31, 23, 59, 59, 999, 0};
    Expect("2000-12-31 accepté", wk::FieldsToFileTime(g, &back), 1);
    const wk::TimeFields h = wk::FileTimeToFields(back);
    Expect("2000-12-31 jour", h.day, 31); Expect("2000-12-31 mois", h.month, 12); Expect("2000 année", h.year, 2000);
    Expect("2000-12-31 dimanche", h.weekday, 0);
    wk::TimeFields bad{2023, 2, 29, 0, 0, 0, 0, 0};
    Expect("2023-02-29 refusé", wk::FieldsToFileTime(bad, &back), 0);
    wk::TimeFields old{1600, 12, 31, 0, 0, 0, 0, 0};
    Expect("1600 refusé", wk::FieldsToFileTime(old, &back), 0);
    if (g_fail == 0) printf("unit_time RÉUSSI\n");
    return g_fail ? 1 : 0;
}
