/* time.h on the SDK clocks (SYS_CLOCK). The system clock is UTC; there are
 * no time zones, so localtime() is gmtime(). */
#include <time.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "minimalos.h"

clock_t clock(void) { return (clock_t)(mos_clock_monotonic_ns() / 1000u); }

time_t time(time_t* t) {
    time_t v = (time_t)(mos_clock_realtime_ns() / 1000000000u);
    if (t) *t = v;
    return v;
}

double difftime(time_t a, time_t b) { return (double)(a - b); }

int clock_gettime(clockid_t id, struct timespec* ts) {
    uint64_t ns;
    if (id == CLOCK_MONOTONIC) ns = mos_clock_monotonic_ns();
    else if (id == CLOCK_REALTIME) ns = mos_clock_realtime_ns();
    else { errno = EINVAL; return -1; }
    ts->tv_sec = (time_t)(ns / 1000000000u);
    ts->tv_nsec = (long)(ns % 1000000000u);
    return 0;
}

int nanosleep(const struct timespec* req, struct timespec* rem) {
    long ms = (long)req->tv_sec * 1000 + (req->tv_nsec + 999999) / 1000000;
    if (ms > 0) mos_sleep(ms);
    if (rem) { rem->tv_sec = 0; rem->tv_nsec = 0; }
    return 0;
}

int usleep(unsigned usec) {
    if (usec) mos_sleep((long)((usec + 999u) / 1000u));
    return 0;
}

static long days_from_civil(long y, unsigned m, unsigned d) {
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long)doe - 719468;
}

struct tm* gmtime_r(const time_t* t, struct tm* out) {
    long secs = *t;
    long days = secs / 86400, rem = secs % 86400;
    if (rem < 0) { rem += 86400; days--; }
    out->tm_hour = (int)(rem / 3600);
    out->tm_min = (int)(rem % 3600 / 60);
    out->tm_sec = (int)(rem % 60);
    out->tm_wday = (int)((days % 7 + 11) % 7);      /* 1970-01-01 was a Thursday */
    long z = days + 719468;
    long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long y = (long)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mp + 2) / 5 + 1;
    unsigned m = mp < 10 ? mp + 3 : mp - 9;
    y += m <= 2;
    out->tm_year = (int)(y - 1900);
    out->tm_mon = (int)m - 1;
    out->tm_mday = (int)d;
    out->tm_yday = (int)(days - days_from_civil(y, 1, 1));
    out->tm_isdst = 0;
    out->tm_gmtoff = 0;
    out->tm_zone = "UTC";
    return out;
}

static struct tm g_tm;
struct tm* gmtime(const time_t* t) { return gmtime_r(t, &g_tm); }
struct tm* localtime(const time_t* t) { return gmtime_r(t, &g_tm); }
struct tm* localtime_r(const time_t* t, struct tm* out) { return gmtime_r(t, out); }

time_t timegm(struct tm* tm) {
    long y = tm->tm_year + 1900L + tm->tm_mon / 12;
    int mon = tm->tm_mon % 12;
    if (mon < 0) { mon += 12; y--; }
    long days = days_from_civil(y, (unsigned)mon + 1, 1) + (tm->tm_mday - 1);
    time_t t = (time_t)(days * 86400 + tm->tm_hour * 3600L + tm->tm_min * 60L + tm->tm_sec);
    gmtime_r(&t, tm);
    return t;
}
time_t mktime(struct tm* tm) { return timegm(tm); }

size_t strftime(char* buf, size_t n, const char* fmt, const struct tm* tm) {
    static const char* const wd[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
    static const char* const mo[] = { "January", "February", "March", "April", "May", "June", "July",
                                      "August", "September", "October", "November", "December" };
    size_t o = 0;
    char tmp[64];
    for (const char* f = fmt; *f; f++) {
        const char* s = tmp;
        int len = -1;
        if (*f != '%') { tmp[0] = *f; len = 1; }
        else {
            f++;
            switch (*f) {
            case 'Y': len = snprintf(tmp, sizeof tmp, "%d", tm->tm_year + 1900); break;
            case 'y': len = snprintf(tmp, sizeof tmp, "%02d", tm->tm_year % 100); break;
            case 'm': len = snprintf(tmp, sizeof tmp, "%02d", tm->tm_mon + 1); break;
            case 'd': len = snprintf(tmp, sizeof tmp, "%02d", tm->tm_mday); break;
            case 'e': len = snprintf(tmp, sizeof tmp, "%2d", tm->tm_mday); break;
            case 'H': len = snprintf(tmp, sizeof tmp, "%02d", tm->tm_hour); break;
            case 'I': len = snprintf(tmp, sizeof tmp, "%02d", tm->tm_hour % 12 ? tm->tm_hour % 12 : 12); break;
            case 'M': len = snprintf(tmp, sizeof tmp, "%02d", tm->tm_min); break;
            case 'S': len = snprintf(tmp, sizeof tmp, "%02d", tm->tm_sec); break;
            case 'j': len = snprintf(tmp, sizeof tmp, "%03d", tm->tm_yday + 1); break;
            case 'p': len = snprintf(tmp, sizeof tmp, "%s", tm->tm_hour < 12 ? "AM" : "PM"); break;
            case 'a': len = snprintf(tmp, sizeof tmp, "%.3s", wd[tm->tm_wday % 7]); break;
            case 'A': s = wd[tm->tm_wday % 7]; len = (int)strlen(s); break;
            case 'b': case 'h': len = snprintf(tmp, sizeof tmp, "%.3s", mo[tm->tm_mon % 12]); break;
            case 'B': s = mo[tm->tm_mon % 12]; len = (int)strlen(s); break;
            case 'Z': s = "UTC"; len = 3; break;
            case 'z': s = "+0000"; len = 5; break;
            case 'F': len = snprintf(tmp, sizeof tmp, "%d-%02d-%02d", tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday); break;
            case 'T': len = snprintf(tmp, sizeof tmp, "%02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec); break;
            case 'R': len = snprintf(tmp, sizeof tmp, "%02d:%02d", tm->tm_hour, tm->tm_min); break;
            case 'D': len = snprintf(tmp, sizeof tmp, "%02d/%02d/%02d", tm->tm_mon + 1, tm->tm_mday, tm->tm_year % 100); break;
            case 'c': len = snprintf(tmp, sizeof tmp, "%.3s %.3s %2d %02d:%02d:%02d %d", wd[tm->tm_wday % 7], mo[tm->tm_mon % 12],
                                     tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec, tm->tm_year + 1900); break;
            case 'n': tmp[0] = '\n'; len = 1; break;
            case 't': tmp[0] = '\t'; len = 1; break;
            case '%': tmp[0] = '%'; len = 1; break;
            case 0: f--; len = 0; break;
            default: tmp[0] = '%'; tmp[1] = *f; len = 2; break;
            }
        }
        if (o + (size_t)len >= n) return 0;
        memcpy(buf + o, s, (size_t)len);
        o += (size_t)len;
    }
    if (n) buf[o] = 0;
    return o;
}
