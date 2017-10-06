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

#if defined(_WIN32) || defined(_SX)
#include <chrono> // new: SX timer now follow the WIN32 approach
#if defined(_SX)
//#include <second.h> // old ?
#endif

#else
#define HAVE_REGEX
#if defined(HAVE_REGEX)
#include <sys/types.h>
#include <regex.h>
#endif /* HAVE_REGEX */
#endif /* _WIN32 */

#include <iostream> // tmp XXX
using std::cout; using std::endl;

static const char *modes[] = { "CORR", "PERF", "TEST", "ALL" };
static int const lenmax = sizeof "CORR+PERF+TEST+ALL"; // max-length result
static char bench_mode_string[lenmax];

const char *bench_mode2str(bench_mode_t mode) {
    assert( (int)mode < 2*(int)ALL );
    if(mode==MODE_UNDEF)
        return "MODE_UNDEF";
    bench_mode_string[0] = '\0';

    char *b = &bench_mode_string[0];
    int len = lenmax;
    char const* sep = "";
    auto add_mode = [&](char const* m) {
        int const n = snprintf(b, len, "%s%s",sep,m);
        //cout<<" lenmax"<<lenmax<<" len"<<len<<" sep"<<sep<<" m"<<m<<" b"<<b<<" n"<<n<<endl;
        assert(n>0);
        if( n>len ){ /*b[len-1]='\0';*/ len=0;}
        else {b+=n; len-=n;}
        sep="+";
    };
    if ((mode & CORR)) {add_mode(modes[0]); }
    if ((mode & PERF)) {add_mode(modes[1]); }
    if ((mode & TEST)) {add_mode(modes[2]); }
    if ((mode & ALL )) {add_mode(modes[3]); }
    return const_cast<const char*>(&bench_mode_string[0]);
}

bench_mode_t str2bench_mode(const char *str) {
    bench_mode_t mode = MODE_UNDEF;
    if (strchr(str, 'c') || strchr(str, 'C'))
        mode = (bench_mode_t)((int)mode | (int)CORR);
    if (strchr(str, 'p') || strchr(str, 'P'))
        mode = (bench_mode_t)((int)mode | (int)PERF);
    if (strchr(str, 'a') || strchr(str, 'A'))
        mode = (bench_mode_t)((int)mode | (int)ALL);
    if (strchr(str, 't') || strchr(str, 'T') )
        mode = (bench_mode_t)((int)mode | (int)TEST);
    if (mode == MODE_UNDEF)
        []() { SAFE(FAIL, CRIT); return 0; }();

    if ( (mode & ALL) && ! (mode & PERF || mode & CORR) )
        mode = (bench_mode_t)((int)mode | (int)CORR);
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

// NEW: _SX timer treatment can follow _WIND32 approach ...
#if defined(_WIN32) || defined(_SX)

#if USE_RDPMC || USE_RDTSC
unsigned long long ticks_now() {
    /* Not allowed on user-mode, privileged instruction */
    return (unsigned long long)0;
}
#endif

#if 0 && defined(_SX)
#include <second.h>
//unsigned long long ticks_now() { // remove, since equiv to ms_now
//    return static_cast<unsigned long long>( second()*1000000.0/*double, microseconds*/ );
//}
static inline double ms_now() {
    return second() * 1000.0 /* ms / second */;
}
#else
static inline double ms_now() {
    auto timePointTmp
        = std::chrono::high_resolution_clock::now().time_since_epoch();
    return std::chrono::duration<double, std::milli>(timePointTmp).count();
    // Fix: conv/perf_report.cpp 'F' and 'p' outputs make it clear that
    //      ms_now returns ms *milli* seconds (not us or microseconds)
    // This also agrees with existing linux code.
}
#endif

#else // linux ...

#include <unistd.h>

#if USE_RDPMC
// [ejk] this segfaulted for me until I added perf_begin and perf_end
//       that actually open a perf_event fd and mmap it.
// Note: if perf_event "can_user_time", you could also get the conversion to nanoseconds
//       in a single call!
// rdbutso REMOVED the function entirely!
unsigned long long ticks_now() {
    unsigned eax, edx, ecx;

    ecx = (1 << 30) + 1;
    __asm__ volatile("rdpmc" : "=a" (eax), "=d" (edx) : "c" (ecx));

    return (unsigned long long)eax | (unsigned long long)edx << 32;
}
#endif

static inline double ms_now() {
//#if ! defined(_SX)
    struct timespec tv;
    clock_gettime(CLOCK_MONOTONIC, &tv);
    return (1000000000ll * tv.tv_sec + tv.tv_nsec) / 1e6;
//#else
//    return second() * 1000.0/* ms / second */; // SX has microsecond resolutions
//#endif
}
#endif /* _WIN32 */

void benchdnn_timer_t::reset() {
    times_ = 0;
#if USE_RDPMC || USE_RDTSC
    for (int i = 0; i < n_modes; ++i) ticks_[i] = 0;
    ticks_start_ = 0;
#endif
    for (int i = 0; i < n_modes; ++i) ms_[i] = 0;
    ms_start_ = 0;

    start();
}

void benchdnn_timer_t::start() {
#if USE_RDPMC || USE_RDTSC
    ticks_start_ = ticks_now();
#endif
    ms_start_ = ms_now();
}

void benchdnn_timer_t::stop() {
    double d_ms = ms_now() - ms_start_;

    ms_start_ += d_ms;

    ms_[benchdnn_timer_t::min] = times_
        ? MIN2(ms_[benchdnn_timer_t::min], d_ms) : d_ms;
    ms_[benchdnn_timer_t::avg] += d_ms;
    ms_[benchdnn_timer_t::max] = times_
        ? MAX2(ms_[benchdnn_timer_t::min], d_ms) : d_ms;

#if USE_RDPMC || USE_RDTSC
    long long d_ticks = ticks_now() - ticks_start_; /* FIXME: overflow? */
    ticks_start_ += d_ticks;
    ticks_[benchdnn_timer_t::min] = times_
        ? MIN2(ticks_[benchdnn_timer_t::min], d_ticks) : d_ticks;
    ticks_[benchdnn_timer_t::avg] += d_ticks;
    ticks_[benchdnn_timer_t::max] = times_
        ? MAX2(ticks_[benchdnn_timer_t::min], d_ticks) : d_ticks;
#endif

    ++times_;
}

benchdnn_timer_t &benchdnn_timer_t::operator=(const benchdnn_timer_t &rhs) {
    if (this == &rhs) return *this;
    times_ = rhs.times_;
#if USE_RDPMC || USE_RDTSC
    for (int i = 0; i < n_modes; ++i) ticks_[i] = rhs.ticks_[i];
    ticks_start_ = rhs.ticks_start_;
#endif
    for (int i = 0; i < n_modes; ++i) ms_[i] = rhs.ms_[i];
    ms_start_ = rhs.ms_start_;
    return *this;
}
