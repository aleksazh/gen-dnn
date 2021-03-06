/*******************************************************************************
* Copyright 2017-2018 Intel Corporation
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

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <float.h>
#include <math.h>
#if defined(_SX)
#include <libgen.h>	/* dirname and strnlen tucked away here! */
#endif

#include "mkldnn.h"

#include "mkldnn_common.hpp"
#include "mkldnn_memory.hpp"

#include "conv/conv.hpp"
#include "conv/conv_test_data.hpp"
// removed in v0.13 #include "conv/input_conv.hpp"          /* default_list of conv tests */

namespace conv {

/* global driver parameters */
const dt_conf_t *cfg = conf_f32;
const char *pattern = NULL;
dir_t dir = FWD_B;
int mb = 0;
alg_t alg = DIRECT;
merge_t merge = NONE;
attr_t attr;
const char *skip_impl = "";
bool allow_unimpl = false;
const char *perf_template = "perf,%n,%d,%GO,%GF,%-t,%-Gp,%0t,%0Gp,%i";

void reset_parameters() {
    cfg = conf_f32;
    pattern = NULL;
    dir = FWD_B;
    mb = 0;
    alg = DIRECT;
    merge = NONE;
    attr = attr_t();
    skip_impl = "";
    attr = attr_t();
    allow_unimpl = false;
}

/** return true if we think mkldnn ought to support this problem. */
static bool check_mkldnn_support( const prb_t *p, res_t *res ) {
    char const *errmsg = nullptr;
    // v0.11 mkl-dnn did not support dilates
    if (errmsg != nullptr){
        printf("\nUNTESTABLE: mkl-dnn probably doesn't handle this case yet\n"
               "          %s\n", errmsg);
        auto &bs = benchdnn_stat;
        res->state = SKIPPED;
        ++bs.skipped;
    }
    return errmsg == nullptr;
}
void check_correctness(const desc_t *c) {
    const prb_t p(*c, dir, cfg, alg, merge, attr, mb);
    char pstr[max_prb_len];
    prb2str(&p, pstr);

    if (pattern && !match_regex(pstr, pattern))
        return;
    print(1, "run: %s", pstr);

    res_t res{};
    // Nicely avoid unsupported things:
    const bool mkldnn_ok = check_mkldnn_support(&p, &res);
    int status = (mkldnn_ok? conv::doit(&p, &res): OK); // <-- run benchmark
    RT_ASSERT( status == OK || status == FAIL );

    bool want_perf_report = (bench_mode & PERF);

    parse_result(res, want_perf_report, allow_unimpl, status, pstr);

    ++benchdnn_stat.tests;
    if(mkldnn_ok && (bench_mode & TEST) && benchdnn_stat.ts){
        benchdnn_stat.ts->prt();
    }
}

int bench(int argc, char **argv, bool main_bench) {
    static bool own_ts = false;
    static unsigned recurse = 0U;

    int const dbg_alloc = 20;
    print(dbg_alloc, "%s recurse=%u\n", "***** conv/bench_conv.cpp *****", recurse);
    if (recurse == 0U){
        if ((benchdnn_stat.ts == nullptr)){
            print(dbg_alloc, "%s", "***** new test_stats\n");
            benchdnn_stat.ts = new test_stats();
            own_ts = true;
        }
    }
    ++recurse;

    for (int arg = 0; arg < argc; ++arg) {
        if (!strncmp("--batch=", argv[arg], 8))
            SAFE(batch(argv[arg] + 8, bench), CRIT);
        else if (!strncmp("--cfg=", argv[arg], 6))
            cfg = str2cfg(argv[arg] + 6);
        else if (!strncmp("--match=", argv[arg], 8))
            pattern = argv[arg] + 8;
        else if (!strncmp("--mb=", argv[arg], 5))
            mb = atoi(argv[arg] + 5);
        else if (!strncmp("--dir=", argv[arg], 6))
            dir = str2dir(argv[arg] + 6);
        else if (!strncmp("--alg=", argv[arg], 6))
            alg = str2alg(argv[arg] + 6);
        else if (!strncmp("--merge=", argv[arg], 8))
            merge = str2merge(argv[arg] + 8);
        else if (!strncmp("--attr=", argv[arg], 7))
            SAFE(str2attr(&attr, argv[arg] + 7), CRIT);
        else if (!strncmp("--skip-impl=", argv[arg], 12))
            skip_impl = argv[arg] + 12;
        else if (!strncmp("--allow-unimpl=", argv[arg], 15))
            allow_unimpl = str2bool(argv[arg] + 15);
        else if (!strncmp("--perf-template=", argv[arg], 16))
            perf_template = argv[arg] + 16;
        else if (!strcmp("--reset", argv[arg]))
            reset_parameters();
        else if (!strncmp("--mode=", argv[0], 7))
            bench_mode = str2bench_mode(argv[0] + 7);
        else if (!strncmp("-v", argv[arg], 2))
            verbose = atoi(argv[arg] + 2);
        else if (!strncmp("--verbose=", argv[arg], 10))
            verbose = atoi(argv[arg] + 10);
        else {
            desc_t c;
            bool is_deconv = 0;
            if (str2desc(&c, argv[arg], is_deconv) == FAIL) {
                fprintf(stderr, "driver: unknown option: `%s`, exiting...\n",
                        argv[arg]);
                exit(2);
            }
            check_correctness(&c);
        }
    }

#if 0
    /* input_conv.hpp has been removed for v 0.13 */
    if (main_bench && benchdnn_stat.tests == 0) {
        const int N = sizeof(default_list) / sizeof(default_list[0]);
        print(0,"/* using default list of %d problems */", N);
        for (int n = 0; n < N; ++n)
            check_correctness(&default_list[n]);
    }
#endif

    --recurse;
    print(dbg_alloc, "%s recurse=%u\n", "*END* conv/bench_conv.cpp *****", recurse);
    if (recurse == 0 && own_ts){
        RT_ASSERT(benchdnn_stat.ts != nullptr);
        print(dbg_alloc, "%s", "***** delete test_stats\n");
        delete benchdnn_stat.ts;
        benchdnn_stat.ts = nullptr;
    }

    return OK;
}

}
// vim: et ts=4 sw=4 cindent cino^=l0,\:0,N-s
