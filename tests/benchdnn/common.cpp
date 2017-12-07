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

#include <chrono>
#define HAVE_REGEX
#if defined(HAVE_REGEX)
#ifdef _WIN32
#include <regex>
#else
#include <sys/types.h>
#include <regex.h>
#endif /* _WIN32 */
#endif /* HAVE_REGEX */

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
    /* corr perf all test : c p a t each occur in exactly 1 position (good) */
    bench_mode_t mode = MODE_UNDEF;
    if (strchr(str, 'c') || strchr(str, 'C'))
        mode = (bench_mode_t)((int)mode | (int)CORR);
    if (strchr(str, 'p') || strchr(str, 'P'))
        mode = (bench_mode_t)((int)mode | (int)PERF);
    if (strchr(str, 'a') || strchr(str, 'A'))
        mode = (bench_mode_t)((int)mode | (int)ALL);
    if (strchr(str, 't') || strchr(str, 'T') )
        mode = (bench_mode_t)((int)mode | (int)TEST);
    if (mode == MODE_UNDEF){
        print(0," bad mode: %s\n", str);
        []() { SAFE(FAIL, CRIT); return 0; }();
    }
    /* ALL implies CORR if neither CORR nor PERF is specified */
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
#ifdef _WIN32
/* NOTE: this should be supported on linux as well, but currently
 * having issues for ICC170 and Clang*/
bool match_regex(const char *str, const char *pattern) {
    std::regex re(pattern);
    return std::regex_search(str, re);
}
#else
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
#endif /* _WIN32 */
#else
bool match_regex(const char *str, const char *pattern) { return true; }
#endif

/* perf */

static inline double ms_now() {
    auto timePointTmp
        = std::chrono::high_resolution_clock::now().time_since_epoch();
    return std::chrono::duration<double, std::milli>(timePointTmp).count();
}

#if !defined(BENCHDNN_USE_RDPMC) || defined(_WIN32)
unsigned long long ticks_now() {
    return (unsigned long long)0;
}
#else
unsigned long long ticks_now() {
    unsigned eax, edx, ecx;

    ecx = (1 << 30) + 1;
    __asm__ volatile("rdpmc" : "=a" (eax), "=d" (edx) : "c" (ecx));

    return (unsigned long long)eax | (unsigned long long)edx << 32;
}
#endif

void benchdnn_timer_t::reset() {
    times_ = 0;
    for (int i = 0; i < n_modes; ++i) ticks_[i] = 0;
    ticks_start_ = 0;
    for (int i = 0; i < n_modes; ++i) ms_[i] = 0;
    ms_start_ = 0;

    start();
}

void benchdnn_timer_t::start() {
    ticks_start_ = ticks_now();
    ms_start_ = ms_now();
}

void benchdnn_timer_t::stop() {
    long long d_ticks = ticks_now() - ticks_start_; /* FIXME: overflow? */
    double d_ms = ms_now() - ms_start_;

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
