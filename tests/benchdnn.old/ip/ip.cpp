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
#include <stdio.h>
#include <float.h>
#include <math.h>

#include "mkldnn.h"

#include "mkldnn_common.hpp"
#include "mkldnn_memory.hpp"

#include "ip/ip.hpp"

namespace ip {
inline bool is_3d(const prb_t *p) {
    return p->id > 1;
}

inline int init_pd(const prb_t *p, mkldnn_inner_product_desc_t &ipd,
        mkldnn_primitive_desc_t &ippd, res_t *r) {
    mkldnn_memory_desc_t src_d, wei_d, bia_d, dst_d;

    int ndims = is_3d(p) ? 5 : 4;
    mkldnn_dims_t src_dims = {p->mb, p->ic, p->ih, p->iw};
    mkldnn_dims_t src_3d_dims = {p->mb, p->ic, p->id, p->ih, p->iw};
    mkldnn_dims_t wei_dims = {p->oc, p->ic, p->ih, p->iw};
    mkldnn_dims_t wei_3d_dims = {p->oc, p->ic, p->id, p->ih, p->iw};
    mkldnn_dims_t bia_dims = {p->oc};
    mkldnn_dims_t dst_dims = {p->mb, p->oc};

    DNN_SAFE(mkldnn_memory_desc_init(&src_d, ndims, is_3d(p) ? src_3d_dims : src_dims,
            p->cfg[SRC].dt, mkldnn_any), WARN);
    DNN_SAFE(mkldnn_memory_desc_init(&wei_d, ndims, is_3d(p) ? wei_3d_dims : wei_dims,
            p->cfg[WEI].dt, mkldnn_any), WARN);
    DNN_SAFE(mkldnn_memory_desc_init(&bia_d, 1, bia_dims, p->cfg[BIA].dt, mkldnn_any), WARN);
    DNN_SAFE(mkldnn_memory_desc_init(&dst_d, 2, dst_dims, p->cfg[DST].dt, mkldnn_any), WARN);

    switch (p->dir) {
    case FWD_D: case FWD_B:
        DNN_SAFE(mkldnn_inner_product_forward_desc_init(&ipd, mkldnn_forward,
                    &src_d, &wei_d, p->dir == FWD_D ? NULL : &bia_d, &dst_d),
                WARN);
        break;
    case BWD_D:
        DNN_SAFE(mkldnn_inner_product_backward_data_desc_init(&ipd, &src_d,
                    &wei_d, &dst_d), WARN);
        break;
    case BWD_W: case BWD_WB:
        DNN_SAFE(mkldnn_inner_product_backward_weights_desc_init(&ipd, &src_d,
                    &wei_d, p->dir == BWD_W ? NULL : &bia_d, &dst_d), WARN);
        break;
    default: DNN_SAFE(mkldnn_invalid_arguments, CRIT);
    }

    DNN_SAFE(ipd.accum_data_type == p->cfg[ACC].dt
            ? mkldnn_success : mkldnn_unimplemented, CRIT);

    auto mkldnn_attr = create_mkldnn_attr(p->attr, p->oc, p->scales);

    mkldnn_status_t init_status = mkldnn_success;
    init_status = mkldnn_primitive_desc_create_v2(&ippd, &ipd, mkldnn_attr,
            engine, NULL);

    mkldnn_primitive_attr_destroy(mkldnn_attr);

    if (init_status == mkldnn_unimplemented)
        return r->state = UNIMPLEMENTED, OK;
    else
        SAFE(init_status, WARN);

    auto q = [=](mkldnn_query_t query, int index = 0) {
        return *mkldnn_primitive_desc_query_memory_d(
                mkldnn_primitive_desc_query_pd(ippd, query, index));
    };

    if (p->dir == BWD_D)
        ipd.diff_src_desc = q(mkldnn_query_diff_src_pd);
    else
        ipd.src_desc = q(mkldnn_query_src_pd);

    if (p->dir & FLAG_WEI)
        ipd.diff_weights_desc = q(mkldnn_query_diff_weights_pd);
    else
        ipd.weights_desc = q(mkldnn_query_weights_pd);

    if (p->dir & FLAG_BIA) {
        if (p->dir & FLAG_BWD)
            ipd.diff_bias_desc = q(mkldnn_query_diff_weights_pd, 1);
        else
            ipd.bias_desc = q(mkldnn_query_weights_pd, 1);
    }

    if (p->dir & FLAG_BWD)
        ipd.diff_dst_desc = q(mkldnn_query_diff_dst_pd);
    else
        ipd.dst_desc = q(mkldnn_query_dst_pd);

    return OK;
}

inline int compare_dat(const prb_t *p, data_kind_t kind, dnn_mem_t &mem_dt,
        dnn_mem_t &mem_fp, res_t *r) {
    size_t nelems = mem_dt.nelems();
    int non_zero = 0;
    const char *skind = data_kind2str(kind);

    r->errors = 0;
    r->total = static_cast<int>(nelems);

    for (size_t i = 0; i < nelems; ++i) {
        float dt = ((float*)mem_dt)[i];
        float fp0 = ((float *)mem_fp)[i];

        float fp = fp0;
        if (p->cfg[kind].dt != mkldnn_f32) {
            using R = attr_t::round_mode_t;
            switch (p->attr.irmode) {
                case R::DOWN: fp = floorf(fp0); break;
                case R::NEAREST: fp = nearbyintf(fp0); break;
                default:
                    return UNTESTED;
            }
        }

        float diff = fabsf(fp - dt);
        float rel_diff = diff / (fabsf(fp) > FLT_MIN ? fabsf(fp) : 1);

        bool ok = true;
        if (fp < p->cfg[kind].min)
            ok = dt == p->cfg[kind].min;
        else if (fp > p->cfg[kind].max)
            ok = dt == p->cfg[kind].max;
        else
            ok = (fabs(fp) > 1e-5 ? rel_diff : diff) <= p->cfg[kind].eps;

        if (!ok) {
            r->errors++;
            if (r->errors < 10 || verbose >= 10) {
                print(0, "[%4lu][%s]"
                         "fp:%8g fp0:%8g dt:%8g diff:%8g rdiff:%8g\n",
                        (unsigned long)i, skind, fp, fp0, dt, diff, rel_diff);
            }
        }
        non_zero += fp != 0;
    }

    const double trust_nz = (double)non_zero / r->total;
    bool no_trust = trust_nz < 0.1;
    if (no_trust) {
        r->state = MISTRUSTED;
        const char *skind = data_kind2str(kind);
        print(0, "@@@ [%s] test-bug: trust is too low."
                 " Nonzeros in output: %.2f\n",
                skind, trust_nz);
    }

    if (r->errors)
        r->state = FAILED;

    if (r->state == UNTESTED)
        r->state = PASSED; /* optimism */

    return r->state == FAILED ? FAIL : OK;
}

#if 0
inline void fill_src(dnn_mem_t &mem_dt, dnn_mem_t &mem_fp, res_t *r) {
    dnn_mem_t mem_00(mem_dt.md_, mkldnn_f32, mkldnn_nchw);
    const size_t sz = mem_00.nelems();
    OMP(parallel for)//;
    for (size_t i = 0; i < sz; ++i)
        ((float*)mem_00)[i] = (float)(1 + (i % 3)); // 1 + sin(0.2* (i % 17));

    mem_dt.reorder(mem_00);
    mem_fp.reorder(mem_dt);

    int sanity_err = compare_dat(mem_fp, mem_00, r);
    if (sanity_err != OK) {
        printf("@@@ sanity failed: %s:%d\n", __func__, __LINE__);
    }
}
#endif

#if 0
inline void fill_wei(dnn_mem_t &mem_dt, dnn_mem_t &mem_fp, res_t *r) {
    dnn_mem_t mem_00(mem_dt.md_, mkldnn_f32, mkldnn_oihw);
    const size_t sz = mem_00.nelems();
    OMP(parallel for)//;
    for (size_t i = 0; i < sz; ++i)
        ((float*)mem_00)[i] = (float)((i % 4) - 1) ; // 1 + sin(0.2* (i % 17));

    mem_dt.reorder(mem_00);
    mem_fp.reorder(mem_dt);

    int sanity_err = compare_dat(mem_fp, mem_00, r);
    if (sanity_err != OK) {
        printf("@@@ sanity failed: %s:%d\n", __func__, __LINE__);
    }
}
#endif

int fill_src(const prb_t *p, dnn_mem_t &mem_dt, dnn_mem_t &mem_fp, res_t *r) {
    dnn_mem_t mem_00(
            mem_dt.md_, mkldnn_f32, is_3d(p) ? mkldnn_ncdhw : mkldnn_nchw);

    const auto &c = p->cfg[SRC];
    const int range = c.f_max - c.f_min + 1;

    OMP(parallel for collapse(5))//;
    for (int mb = 0; mb < p->mb; ++mb)
        for (int ic = 0; ic < p->ic; ++ic)
            for (int id = 0; id < p->id; ++id)
                for (int ih = 0; ih < p->ih; ++ih)
                    for (int iw = 0; iw < p->iw; ++iw) {
                        const int gen = 5 * id + 17 * ih + 13 * iw + 13 * mb
                                + 19 * ic + 1637;
                        const bool non_base = flip_coin(gen, c.f_sparsity);
                        const float value = non_base ?
                                c.f_min + gen * c.f_step % range :
                                c.f_base;

                        ((float *)mem_00)[src_off_f(p, mb, ic, id, ih, iw)]
                                = value;
                    }

    SAFE(mem_dt.reorder(mem_00), WARN);
    SAFE(mem_fp.reorder(mem_dt), WARN);
    return OK;
}

int fill_wei(const prb_t *p, dnn_mem_t &mem_dt, dnn_mem_t &mem_fp, res_t *r) {
    dnn_mem_t mem_00(
            mem_dt.md_, mkldnn_f32, is_3d(p) ? mkldnn_goihw : mkldnn_oihw);

    const auto &c = p->cfg[WEI];
    const int range = c.f_max - c.f_min + 1;
    OMP(parallel for collapse(5))//;
    for (int oc = 0; oc < p->oc; ++oc)
        for (int ic = 0; ic < p->ic; ++ic)
            for (int id = 0; id < p->id; ++id)
                for (int ih = 0; ih < p->ih; ++ih)
                    for (int iw = 0; iw < p->iw; ++iw) {
                        const int gen = 5 * id + 17 * ih + 13 * iw + 13 * oc
                                + 19 * ic + 38;
                        const bool non_base = flip_coin(gen, c.f_sparsity);
                        const float value = non_base ?
                                c.f_min + gen * c.f_step % range :
                                c.f_base;

                        ((float *)mem_00)[wei_off_f(p, oc, ic, id, ih, iw)]
                                = value;
                    }

    SAFE(mem_dt.reorder(mem_00), WARN);
    SAFE(mem_fp.reorder(mem_dt), WARN);
    return OK;
}

int fill_bia(const prb_t *p, dnn_mem_t &mem_dt, dnn_mem_t &mem_fp, res_t *r) {
    dnn_mem_t mem_00(mem_dt.md_, mkldnn_f32, mkldnn_x);

    const auto &c = p->cfg[BIA];
    const int range = c.f_max - c.f_min + 1;

    const size_t sz = mem_00.nelems();
    for (size_t i = 0; i < sz; ++i) {
        const int gen = (int)(19 * i);
        const bool non_base = flip_coin(gen, c.f_sparsity);
        const float value
                = non_base ? c.f_min + gen * c.f_step % range : c.f_base;

        ((float *)mem_00)[i] = value;
    }

    SAFE(mem_dt.reorder(mem_00), WARN);
    SAFE(mem_fp.reorder(mem_dt), WARN);
    return OK;
}

int fill_dst(const prb_t *p, dnn_mem_t &mem_dt, dnn_mem_t &mem_fp, res_t *r) {
    dnn_mem_t mem_00(mem_dt.md_, mkldnn_f32, mkldnn_nc);
#if 0 // contiguous?
    const size_t sz = mem_00.nelems();
    OMP(parallel for)//;
    for (size_t i = 0; i < sz; ++i)
        ((float*)mem_00)[i] = (float)(1 + (i % 3)); // 1 + sin(0.2* (i % 17));
#else // safer?
    const auto &c = p->cfg[DST];
    const int range = c.f_max - c.f_min + 1;

    OMP(parallel for collapse(2))
    for (int mb = 0; mb < p->mb; ++mb)
        for (int oc = 0; oc < p->oc; ++oc) {
            const int gen = 17 * mb + 13 * oc + 12;
            const bool non_base = flip_coin(gen, c.f_sparsity);
            const float value
                    = non_base ? c.f_min + gen * c.f_step % range : c.f_base;

            ((float *)mem_00)[dst_off_f(p, mb, oc)] = value;
        }
#endif

    mem_dt.reorder(mem_00);
    mem_fp.reorder(mem_dt);

    SAFE(mem_dt.reorder(mem_00), WARN);
    SAFE(mem_fp.reorder(mem_dt), WARN);

    return OK;
}

int doit(const prb_t *p, res_t *r) {
    mkldnn_inner_product_desc_t ipd;
    mkldnn_primitive_desc_t ippd;
    mkldnn_primitive_t ip;
    bool const use_bia = p->dir & FLAG_BIA;

    SAFE(init_pd(p, ipd, ippd, r), WARN);
    if (r->state == SKIPPED || r->state == UNIMPLEMENTED)
        return OK;

    auto &src_dt_d = p->dir == BWD_D ? ipd.diff_src_desc : ipd.src_desc;
    auto &wei_dt_d = p->dir & FLAG_WEI ? ipd.diff_weights_desc : ipd.weights_desc;
    auto &bia_dt_d = p->dir & FLAG_BWD ? ipd.diff_bias_desc : ipd.bias_desc;
    auto &dst_dt_d = p->dir & FLAG_BWD ? ipd.diff_dst_desc: ipd.dst_desc;

    const auto fp = mkldnn_f32;
#if 0 //mine
    dnn_mem_t src_dt(src_dt_d, p->src_dt);
    dnn_mem_t wei_dt(wei_dt_d, p->wei_dt);
    dnn_mem_t dst_dt(dst_dt_d, p->dst_dt);
    dnn_mem_t bia_dt = use_bia? dnn_mem_t(bia_dt_d, p->dst_dt) : dnn_mem_t();
    //dnn_mem_t bia_dt = p->dir & FLAG_BIA
    //        ? dnn_mem_t(bia_dt_d, p->dst_dt) : dnn_mem_t();
    //dnn_mem_t bia_dt = dnn_mem_t::optional(use_bia, bia_dt_d, p->dst_dt);
#else //new
    dnn_mem_t src_dt(src_dt_d, p->cfg[SRC].dt);
    dnn_mem_t wei_dt(wei_dt_d, p->cfg[WEI].dt);
    dnn_mem_t dst_dt(dst_dt_d, p->cfg[DST].dt);
    dnn_mem_t bia_dt = p->dir & FLAG_BIA
        ? dnn_mem_t(bia_dt_d, p->cfg[BIA].dt) : dnn_mem_t();
#endif

    auto src_format = is_3d(p) ? mkldnn_ncdhw : mkldnn_nchw;
    auto wei_format = is_3d(p) ? mkldnn_oidhw : mkldnn_oihw;
    dnn_mem_t src_fp(src_dt_d, fp, src_format);
    dnn_mem_t wei_fp(wei_dt_d, fp, wei_format);
    dnn_mem_t dst_fp(dst_dt_d, fp, mkldnn_nc);
    dnn_mem_t bia_fp = use_bia? dnn_mem_t(bia_dt_d, fp, mkldnn_x) : dnn_mem_t();
    //dnn_mem_t bia_fp = p->dir & FLAG_BIA
    //        ? dnn_mem_t(bia_dt_d, fp, mkldnn_x) : dnn_mem_t();
    //dnn_mem_t bia_fp = dnn_mem_t::optional(use_bia, bia_dt_d, fp, mkldnn_x);

#if 0 //mine
    fill_src(src_dt, src_fp, r);
    fill_wei(wei_dt, wei_fp, r);
    fill_dst(dst_dt, dst_fp, r);
    if (use_bia)
        fill_bia(bia_dt, bia_fp, r);
#else //new
    SAFE(fill_src(p, src_dt, src_fp, r), WARN);
    SAFE(fill_wei(p, wei_dt, wei_fp, r), WARN);
    SAFE(fill_dst(p, dst_dt, dst_fp, r), WARN);
    if (p->dir & FLAG_BIA)
        SAFE(fill_bia(p, bia_dt, bia_fp, r), WARN);
#endif

    if (p->dir & FLAG_FWD) {
        mkldnn_primitive_at_t inputs[3] = { {src_dt.p_, 0}, {wei_dt.p_, 0},
            {use_bia? bia_dt.p_ : NULL, 0}
        };
        const_mkldnn_primitive_t outputs[] = { dst_dt.p_ };
        DNN_SAFE(mkldnn_primitive_create(&ip, ippd, inputs, outputs), WARN);
        SAFE(execute(ip), WARN);
        if (bench_mode & CORR) {
            compute_ref_fwd(p, src_fp, wei_fp, bia_fp, dst_fp);
            dnn_mem_t dst(dst_dt, fp, mkldnn_nc);
            SAFE(dst.reorder(dst_dt), WARN);
            SAFE(compare_dat(p, DST, dst, dst_fp, r), WARN);
        }
    } else if (p->dir == BWD_D) {
        mkldnn_primitive_at_t inputs[3] = { {dst_dt.p_, 0}, {wei_dt.p_, 0}, };
        const_mkldnn_primitive_t outputs[] = { src_dt.p_ };
        DNN_SAFE(mkldnn_primitive_create(&ip, ippd, inputs, outputs), WARN);
        SAFE(execute(ip), WARN);
        if (bench_mode & CORR) {
            compute_ref_bwd_d(p, src_fp, wei_fp, dst_fp);
            dnn_mem_t src(src_dt, fp, src_format);
            SAFE(src.reorder(src_dt), WARN);
            SAFE(compare_dat(p, SRC, src, src_fp, r), WARN);
        }
    } else if (p->dir & FLAG_BWD && p->dir & FLAG_WEI) {
        mkldnn_primitive_at_t inputs[3] = { {src_dt.p_, 0}, {dst_dt.p_, 0}, };
        const_mkldnn_primitive_t outputs[] = { wei_dt.p_,
            use_bia ? bia_dt.p_ : NULL,
        };
#if 0 //mine (huh?)
        DNN_SAFE(mkldnn_primitive_create(&c, ippd, inputs, outputs), WARN);
        execute(c);
        dnn_mem_t wei(wei_dt, fp, mkldnn_goihw);
        wei.reorder(wei_dt);
        if (compare_dat(wei, wei_fp, r) != 0) return FAIL;
        if (use_bia) {
            dnn_mem_t bia(bia_dt, fp, mkldnn_x);
            bia.reorder(bia_dt);
            compare_dat(bia, bia_fp, r);
#endif
        DNN_SAFE(mkldnn_primitive_create(&ip, ippd, inputs, outputs), WARN);
        SAFE(execute(ip), WARN);
        if (bench_mode & CORR) {
            compute_ref_bwd_w(p, src_fp, wei_fp, bia_fp, dst_fp);
            dnn_mem_t wei(wei_dt, fp, wei_format);
            SAFE(wei.reorder(wei_dt), WARN);
            if (compare_dat(p, WEI, wei, wei_fp, r) != OK) return FAIL;
            if (p->dir & FLAG_BIA) {
                dnn_mem_t bia(bia_dt, fp, mkldnn_x);
                SAFE(bia.reorder(bia_dt), WARN);
                SAFE(compare_dat(p, BIA, bia, bia_fp, r), WARN);
            }
        }
    }

    if (bench_mode & PERF) {
        auto &t = r->timer;
        t.reset();
        while (true) {
            SAFE(execute(ip), WARN);
            t.stamp();
            const bool stop = false
                || (fix_times_per_prb && t.times() >= fix_times_per_prb)
                || (!fix_times_per_prb
                        && t.total_ms() >= max_ms_per_prb
                        && t.times() >= min_times_per_prb);
            if (stop) break;
        }
    }

    return OK;
}

}
