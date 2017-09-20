/*******************************************************************************
* Copyright 2017 Intel Corporation
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*******************************************************************************/

#include "common.hpp"

#ifdef _WIN32
#include <chrono>
#else
#define HAVE_REGEX
#if defined(HAVE_REGEX)
#include <sys/types.h>
#include <regex.h>
#endif /* HAVE_REGEX */
#endif /* _WIN32 */

const char *bench_mode2str(bench_mode_t mode) {
    const char *modes[] = {
        "MODE_UNDEF", "CORR", "PERF", "CORR+PERF"
    };
    assert((int)mode < 4);
    return modes[(int)mode];
}

bench_mode_t str2bench_mode(const char *str) {
    bench_mode_t mode = MODE_UNDEF;
    if (strchr(str, 'c') || strchr(str, 'C'))
        mode = (bench_mode_t)((int)mode | (int)CORR);
    if (strchr(str, 'p') || strchr(str, 'P'))
        mode = (bench_mode_t)((int)mode | (int)PERF);
    if (mode == MODE_UNDEF)
        []() { SAFE(FAIL, CRIT); return 0; }();
    return mode;
}

dir_t str2dir(const char *str) {
#define CASE(x) if (!strcasecmp(STRINGIFY(x), str)) return x
    CASE(FWD_D);
    CASE(FWD_B);
    CASE(BWD_D);
    CASE(BWD_W);
    CASE(BWD_WB);
#undef CASE
    assert(!"unknown dir");
    return DIR_UNDEF;
}

const char *dir2str(dir_t dir) {
#define CASE(x) if (dir == x) return STRINGIFY(x)
    CASE(FWD_D);
    CASE(FWD_B);
    CASE(BWD_D);
    CASE(BWD_W);
    CASE(BWD_WB);
#undef CASE
    assert(!"unknown dir");
    return "DIR_UNDEF";
}

const char *state2str(res_state_t state) {
#define CASE(x) if (state == x) return STRINGIFY(x)
    CASE(UNTESTED);
    CASE(PASSED);
    CASE(SKIPPED);
    CASE(MISTRUSTED);
    CASE(UNIMPLEMENTED);
    CASE(FAILED);
#undef CASE
    assert(!"unknown res state");
    return "STATE_UNDEF";
}

bool str2bool(const char *str) {
    return !strcasecmp("true", str) || !strcasecmp("1", str);
}

const char *bool2str(bool value) {
    return value ? "true" : "false";
}

#if defined(HAVE_REGEX)
bool match_regex(const char *str, const char *pattern) {
    static regex_t regex;
    static const char *prev_pattern = NULL;

    if (pattern != prev_pattern) {
        if (prev_pattern)
            regfree(&regex);

        if (regcomp(&regex, pattern, 0)) {
            fprintf(stderr, "could not create regex\n");
            return true;
        }

        prev_pattern = pattern;
    }

    return !regexec(&regex, str, 0, NULL, 0);
}
#else
bool match_regex(const char *str, const char *pattern) { return true; }
#endif

/* perf */

#include <sys/types.h>
#include <time.h>

#ifdef _WIN32
unsigned long long ticks_now() {
    /* Not allowed on user-mode, privileged instruction */
    return (unsigned long long)0;
}

static inline double ms_now() {
    auto timePointTmp
        = std::chrono::high_resolution_clock::now().time_since_epoch();
    return std::chrono::duration<double, std::micro>(timePointTmp).count();
}

#elif defined(_SX)
// I only found one method, for SX (so making ticks_now equiv to ms_now())
#include <second.h>
//unsigned long long ticks_now() { // remove, since equiv to ms_now
//    return static_cast<unsigned long long>( second()*1000000.0/*double, microseconds*/ );
//}

#else

#include <unistd.h>

// [ejk] this segfaulted for me until I added perf_begin and perf_end
//       that actually open a perf_event fd and mmap it.
// Note: if perf_event "can_user_time", you could also get the conversion to nanoseconds
//       in a single call!
unsigned long long ticks_now() {
    unsigned eax, edx, ecx;

    ecx = (1 << 30) + 1;
    __asm__ volatile("rdpmc" : "=a" (eax), "=d" (edx) : "c" (ecx));

    return (unsigned long long)eax | (unsigned long long)edx << 32;
}

static inline double ms_now() {
#if ! defined(_SX)
    struct timespec tv;
    clock_gettime(CLOCK_MONOTONIC, &tv);
    return (1000000000ll * tv.tv_sec + tv.tv_nsec) / 1e6;
#else
    return second() * 1000.0/* ms / second */; // SX has microsecond resolutions
#endif
}
#endif /* _WIN32 */

void benchdnn_timer_t::reset() {
    times_ = 0;
    for (int i = 0; i < n_modes; ++i) ticks_[i] = 0;
    ticks_start_ = 0;
    for (int i = 0; i < n_modes; ++i) ms_[i] = 0;
    ms_start_ = 0;

    start();
}

void benchdnn_timer_t::start() {
#if ! defined(_SX)
    ticks_start_ = ticks_now();
    ms_start_ = ms_now();
#else // ok, just make them equiv
    ms_start_ = ms_now();
    ticks_start_ = static_cast<typeof(ticks_start_)>(ms_start_ * 1000.0);
#endif
}

void benchdnn_timer_t::stop() {
#if ! defined(_SX)
    long long d_ticks = ticks_now() - ticks_start_; /* FIXME: overflow? */
    double d_ms = ms_now() - ms_start_;
#else
    double d_ms = ms_now() - ms_start_;
    long long d_ticks = static_cast<long long>( d_ms*1000.0 );
#endif

    ticks_start_ += d_ticks;
    ms_start_ += d_ms;

    ms_[benchdnn_timer_t::min] = times_
        ? MIN2(ms_[benchdnn_timer_t::min], d_ms) : d_ms;
    ms_[benchdnn_timer_t::avg] += d_ms;
    ms_[benchdnn_timer_t::max] = times_
        ? MAX2(ms_[benchdnn_timer_t::min], d_ms) : d_ms;

    ticks_[benchdnn_timer_t::min] = times_
        ? MIN2(ticks_[benchdnn_timer_t::min], d_ticks) : d_ticks;
    ticks_[benchdnn_timer_t::avg] += d_ticks;
    ticks_[benchdnn_timer_t::max] = times_
        ? MAX2(ticks_[benchdnn_timer_t::min], d_ticks) : d_ticks;

    times_++;
}

benchdnn_timer_t &benchdnn_timer_t::operator=(const benchdnn_timer_t &rhs) {
    if (this == &rhs) return *this;
    times_ = rhs.times_;
    for (int i = 0; i < n_modes; ++i) ticks_[i] = rhs.ticks_[i];
    ticks_start_ = rhs.ticks_start_;
    for (int i = 0; i < n_modes; ++i) ms_[i] = rhs.ms_[i];
    ms_start_ = rhs.ms_start_;
    return *this;
}
