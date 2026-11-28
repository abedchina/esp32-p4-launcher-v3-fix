/* time module hooks (included by extmod/modtime.c): wall clock from the launcher */
#include "papp/papp.h"
#include "shared/timeutils/timeutils.h"

static mp_obj_t mp_time_time_get(void)
{
    int64_t s;
    if (!papp_utc_time(&s)) s = papp_time_us() / 1000000;   /* no clock yet: seconds since boot */
    return mp_obj_new_int_from_ll(s);
}

static mp_obj_t mp_time_localtime_get(void)
{
    papp_datetime_t t;
    timeutils_struct_time_t tm = { .tm_year = 2000, .tm_mon = 1, .tm_mday = 1 };
    if (papp_local_time(&t)) {
        tm.tm_year = t.year; tm.tm_mon = t.month; tm.tm_mday = t.day;
        tm.tm_hour = t.hour; tm.tm_min = t.minute; tm.tm_sec = t.second;
    }
    tm.tm_wday = timeutils_calc_weekday(tm.tm_year, tm.tm_mon, tm.tm_mday);
    tm.tm_yday = timeutils_year_day(tm.tm_year, tm.tm_mon, tm.tm_mday);
    mp_obj_t tuple[8] = {
        mp_obj_new_int(tm.tm_year), mp_obj_new_int(tm.tm_mon), mp_obj_new_int(tm.tm_mday),
        mp_obj_new_int(tm.tm_hour), mp_obj_new_int(tm.tm_min), mp_obj_new_int(tm.tm_sec),
        mp_obj_new_int(tm.tm_wday), mp_obj_new_int(tm.tm_yday),
    };
    return mp_obj_new_tuple(8, tuple);
}
