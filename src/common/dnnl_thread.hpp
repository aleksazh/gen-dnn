/*******************************************************************************
* Copyright 2017-2019 Intel Corporation
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

#ifndef DNNL_THREAD_HPP
#define DNNL_THREAD_HPP

#include "utils.hpp"

#define ENABLE_OMP_MACROS (DNNL_CPU_THREADING_RUNTIME == DNNL_RUNTIME_OMP)
#include "dnnl_omp.h"       // omp portable macro definitions, if needed

#if DNNL_CPU_THREADING_RUNTIME == DNNL_RUNTIME_SEQ
#define DNNL_THR_SYNC 1
inline int dnnl_get_max_threads() {
    return 1;
}
inline int dnnl_get_num_threads() {
    return 1;
}
inline int dnnl_get_thread_num() {
    return 0;
}
inline int dnnl_in_parallel() {
    return 0;
}
inline void dnnl_thr_barrier() {}

#elif DNNL_CPU_THREADING_RUNTIME == DNNL_RUNTIME_OMP
#include <omp.h>
#include "dnnl_omp.h"
#define DNNL_THR_SYNC 1

inline int dnnl_get_max_threads() {
    return omp_get_max_threads();
}
inline int dnnl_get_num_threads() {
    return omp_get_num_threads();
}
inline int dnnl_get_thread_num() {
    return omp_get_thread_num();
}
inline int dnnl_in_parallel() {
    return omp_in_parallel();
}
inline void dnnl_thr_barrier() {
#pragma omp barrier
}

#elif DNNL_CPU_THREADING_RUNTIME == DNNL_RUNTIME_TBB
#include "tbb/parallel_for.h"
#include "tbb/task_arena.h"
#define DNNL_THR_SYNC 0

inline int dnnl_get_max_threads() {
    return tbb::this_task_arena::max_concurrency();
}
inline int dnnl_get_num_threads() {
    return dnnl_get_max_threads();
}
inline int dnnl_get_thread_num() {
    return tbb::this_task_arena::current_thread_index();
}
inline int dnnl_in_parallel() {
    return 0;
}
inline void dnnl_thr_barrier() {
    assert(!"no barrier in TBB");
}
inline tbb::static_partitioner dnnl_tbb_partitioner() {
    return tbb::static_partitioner();
}

#else
#error "unsupported DNNL_CPU_THREADING_RUNTIME!"

#endif

//
// --------------- reintroduce here, and move away include/dnnl_os.h
// --------------- into src/opt_pragmas.h for other stuff.
// src/common/opt_pragmas.h is controlled by separate
//     ENABLE_OPT_PRAGMAS and ENABLE_OMP_PRAGMAS flags (set here!)
//
// [ejk] pragma macros --> include/mkldnn_os.h, handles more cases
namespace dnnl {
namespace impl {

inline bool dnnl_thr_syncable() {
    return DNNL_THR_SYNC == 1;
}

template <typename T, typename U>
inline void balance211(T n, U team, U tid, T &n_start, T &n_end) {
    T n_min = 1;
    T &n_my = n_end;
    if (team <= 1 || n == 0) {
        n_start = 0;
        n_my = n;
    } else if (n_min == 1) {
        // team = T1 + T2
        // n = T1*n1 + T2*n2  (n1 - n2 = 1)
        T n1 = utils::div_up(n, (T)team);
        T n2 = n1 - 1;
        T T1 = n - n2 * (T)team;
        n_my = (T)tid < T1 ? n1 : n2;
        n_start = (T)tid <= T1 ? tid * n1 : T1 * n1 + ((T)tid - T1) * n2;
    }

    n_end += n_start;
}

template <typename T, typename U>
void balance2D(U nthr, U ithr, T ny, T &ny_start, T &ny_end, T nx, T &nx_start,
        T &nx_end, T nx_divider) {
    const int grp_count = nstl::min(nx_divider, nthr);
    const int grp_size_big = nthr / grp_count + 1;
    const int grp_size_small = nthr / grp_count;
    const int n_grp_big = nthr % grp_count;
    const int threads_in_big_groups = n_grp_big * grp_size_big;

    const int ithr_bound_distance = ithr - threads_in_big_groups;
    T grp, grp_ithr, grp_nthr;
    if (ithr_bound_distance < 0) { // ithr in first groups
        grp = ithr / grp_size_big;
        grp_ithr = ithr % grp_size_big;
        grp_nthr = grp_size_big;
    } else { // ithr in last groups
        grp = n_grp_big + ithr_bound_distance / grp_size_small;
        grp_ithr = ithr_bound_distance % grp_size_small;
        grp_nthr = grp_size_small;
    }

    balance211(nx, grp_count, grp, nx_start, nx_end);
    balance211(ny, grp_nthr, grp_ithr, ny_start, ny_end);
}

} // namespace impl
} // namespace dnnl

#include "dnnl_thread_parallel_nd.hpp"

#endif

// vim: et ts=4 sw=4 cindent cino=+2s,^=l0,\:0,N-s
