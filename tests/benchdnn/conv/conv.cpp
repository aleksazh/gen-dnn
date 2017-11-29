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

#include <stdlib.h>
#include <stdio.h>
#include <float.h>
#include <math.h>

#include "mkldnn_io.hpp" // set MKLDNN_IO and provide mkldnn_name_XXX funcs
// Note: mainly for the 'shorten' function to make layer impl strings readable
//       It is a simple heuristric to shorten the oft-long __PRETTY_FUNCTION__
//       names returned via the mkl-dnn query API for primitive descriptors.

#if defined(JITFUNCS)    // only for necla-ml fork...
//   for additional quick debug...
#include <iomanip>
using mkldnn::operator <<;
using std::cout; using std::endl; using std::setw;
#endif

#include "mkldnn.h"

#include "mkldnn_common.hpp"
#include "mkldnn_memory.hpp"

#include "norm.hpp"

#include "conv/conv.hpp"
#include "conv/conv_test.hpp"
#include "conv/conv_test_data.hpp"

#include <functional> // ??? lambda passing?

namespace conv {

static conv_impls_t conv_impls_[] = {
    // Always keep this one, to calculate speedup factors
    {"orig", compute_ref_fwd,       compute_ref_bwd_d,          compute_ref_bwd_w},

#if defined(_SX)
    {"sx2", sxconv_2_fwd, sxconv_2_bwd_d, sxconv_2_bwd_w},
    {"sx3",  sxconv_3_fwd,  sxconv_3_bwd_d,  sxconv_3_bwd_w},
    {"NULL", nullptr, nullptr, nullptr},
    {"NULL", nullptr, nullptr, nullptr}
    //{"NULL", nullptr, nullptr, nullptr},
    //{"ref3", refconv_3_fwd, refconv_3_bwd_d, refconv_3_bwd_w},
    //{"ref4", refconv_4_fwd, refconv_4_bwd_d, refconv_4_bwd_w}
#else
    //{"ref2", refconv_2_fwd,         refconv_2_bwd_d,            refconv_2_bwd_w},
    //{"ref5", refconv_5_fwd,         refconv_5_bwd_d,            refconv_5_bwd_w},
    //{"ref5", refconv_5_fwd,         refconv_5_bwd_d,            refconv_5_bwd_w},
    {"sx2", sxconv_2_fwd, sxconv_2_bwd_d, sxconv_2_bwd_w},
    //{"NULL", nullptr, nullptr, nullptr},      // save time?

    {"ref3", refconv_3_fwd,         refconv_3_bwd_d,            refconv_3_bwd_w},
    //{"NULL", nullptr, nullptr, nullptr},

    {"ref4", refconv_4_fwd,         refconv_4_bwd_d,            refconv_4_bwd_w},
    //{"NULL", nullptr, nullptr, nullptr},

    {"rf99", refconv_99_fwd,         refconv_99_bwd_d,            refconv_99_bwd_w}
    //{"NULL", nullptr, nullptr, nullptr}
#endif
};

#define TESTN (sizeof(conv_impls_) / sizeof(conv_impls_t))

static_assert( TESTN == get_nref_impls(),
        "Please reconcile conv_impls (conv.cpp) with get_nref_impls() (conv/conv.hpp");
conv_impls_t *conv_impls = &conv_impls_[0];

conv_impls_t *get_ref_impls() { return conv_impls; }

double get_trust_nz_level(const prb_t *p, int what, bool final_compare) {
    if (!final_compare)
        return p->cfg[what].f_sparsity;

    double trust = 0.3; /* why? */
    switch (what) {
        case SRC:
            trust /= p->sh * p->sw;
            break;
        case WEI:
            trust /= 1. * p->kh * p->kw
                / MIN3(p->kh * p->kw, p->ih * p->iw, p->oh * p->ow);
            break;
        case BIA:
            trust = 0.8 * p->cfg[DST].f_sparsity; /* why? */
            break;
        case DST:
            trust /= p->merge == RELU ? 2. : 1.;
            break;
    }

    return trust;
}

inline int compare_dat(const prb_t *p, int what, dnn_mem_t &mem_dt,
        dnn_mem_t &mem_fp, res_t *r, bool final_compare = false) {
    //int nelems = static_cast<int>(mem_dt.nelems()); // making 'i' size_t is not as easy as in ip/ip.cpp
    size_t nelems = mem_dt.nelems();

    const char *swhat = inp_type2str(what);

    int in = 0, below = 0, above = 0;
    int in_ok = 0, below_ok = 0, above_ok = 0;
    int non_zero = 0;

    diff_norm_t diff_norm;

    r->errors = 0;
    r->total = nelems;

    for (size_t i = 0; i < nelems; ++i) {
        const float dt = ((float*)mem_dt)[i];
        const float fp0 = ((float*)mem_fp)[i];

        float fp = fp0;
        if (p->cfg[what].dt != mkldnn_f32) {
            using R = attr_t::round_mode_t;
            switch (p->attr.irmode) {
                case R::DOWN: fp = floorf(fp0); break;
                case R::NEAREST: fp = rintf(fp0); break;
                default:
                    return UNTESTED;
            }
        }

        const float diff = fabsf(fp - dt);
        const float rel_diff = diff / (fabsf(fp) > FLT_MIN ? fabsf(fp) : 1);

        bool ok = true;
        if (fp < p->cfg[what].min) {
            diff_norm.update(p->cfg[what].min, dt);
            ok = dt == p->cfg[what].min;
            below += 1;
            below_ok += ok;
        } else if (fp > p->cfg[what].max) {
            diff_norm.update(p->cfg[what].max, dt);
            ok = dt == p->cfg[what].max;
            above += 1;
            above_ok += ok;
        } else {
            diff_norm.update(fp, dt);
            ok = (fabs(fp) > 1e-5 ? rel_diff : diff) <= p->cfg[what].eps;
            in += 1;
            in_ok += ok;
        }
        if (!ok) {
            r->errors++;
            if (r->errors < 10 || verbose >= 10) {
                int mb_or_g = 0, g_or_oc = 0, c = 0, h = 0, w = 0;
                switch (what) {
                case SRC: inv_src_off_f(p, i, mb_or_g, g_or_oc, c, h, w); break;
                case WEI: inv_wei_off_f(p, i, mb_or_g, g_or_oc, c, h, w); break;
                case BIA: inv_bia_off_f(p, i, mb_or_g, g_or_oc); break;
                case DST: inv_dst_off_f(p, i, mb_or_g, g_or_oc, c, h, w); break;
                }
                print(0, "[%4lu][%s%s][%d,%d,%d,%d,%d] "
                        "fp:%8g fp0:%8g dt:%8g diff:%8g rdiff:%8g\n",
                        (unsigned long)i,
                        final_compare == false ? "REORDER " : "",
                        swhat, mb_or_g, g_or_oc, c, h, w,
                        fp, fp0, dt, diff, rel_diff);
            }
        }

        /* for debug purposes only: dump the output */
        if (final_compare && verbose >= 50 && i < 30) {
            int mb_or_g = 0, g_or_oc = 0, c = 0, h = 0, w = 0;
            switch (what) {
            case SRC: inv_src_off_f(p, i, mb_or_g, g_or_oc, c, h, w); break;
            case WEI: inv_wei_off_f(p, i, mb_or_g, g_or_oc, c, h, w); break;
            case BIA: inv_bia_off_f(p, i, mb_or_g, g_or_oc); break;
            case DST: inv_dst_off_f(p, i, mb_or_g, g_or_oc, c, h, w); break;
            }

            print(0, "[%4lu][%s][%d,%d,%d,%d,%d] fp:%8g fp0:%8g dt:%8g\n",
                    (unsigned long)i,
                    swhat, mb_or_g, g_or_oc, c, h, w, fp, fp0, dt);
        }

        non_zero += fp != 0;
    }

    diff_norm.done();

    if (final_compare || r->errors) {
        const int vl = r->errors ? 0 : 2;
        print(vl, "@@@ [%s] %sdiff: l0(``%g``) "
                "l1:(%g,%g,%g,``%g``) "
                "l2:(%g,%g,%g,``%g``) "
                "l8:(%g,%g,%g,``%g``)\n",
                swhat, final_compare ? "final: " : "",
                diff_norm.rel_diff(norm_t::L0),
                diff_norm.a_[norm_t::L1], diff_norm.b_[norm_t::L1],
                diff_norm.diff_[norm_t::L1], diff_norm.rel_diff(norm_t::L1),
                diff_norm.a_[norm_t::L2], diff_norm.b_[norm_t::L2],
                diff_norm.diff_[norm_t::L2], diff_norm.rel_diff(norm_t::L2),
                diff_norm.a_[norm_t::L8], diff_norm.b_[norm_t::L8],
                diff_norm.diff_[norm_t::L8], diff_norm.rel_diff(norm_t::L8));
    }

    const double trust_rg_level = 0.3;
    const double trust_nz_level = get_trust_nz_level(p, what, final_compare);

    const double trust_rg = (double)in / r->total;
    const double trust_nz = (double)non_zero / r->total;

    const bool no_trust = true /* ...in the test ...at all */
        && final_compare
        && (trust_rg < trust_rg_level || trust_nz < trust_nz_level);

    const bool dump = verbose >= 20
        || (verbose >= 10 && (trust_rg < 1. || trust_nz < 1.));
    if (dump) {
        print(0, "@@@ [%s] %strust range:%.2f nz:%.2f "
                "(level range:%.2f nz:%.2f). "
                "in:%d (ok:%d) below:%d (ok:%d) above:%d (ok:%d) nz:%d "
                "total:%lu\n", swhat, final_compare ? "final: " : "",
                trust_rg, trust_nz, trust_rg_level, trust_nz_level, in, in_ok,
                below, below_ok, above, above_ok, non_zero,
                (unsigned long)r->total);
    }

    if (no_trust) {
        r->state = MISTRUSTED;
        print(0, "@@@ [%s] test-bug: trust is too low. "
                "range:%.2f (?<%.2f) nz:%.2f (?<%.2f) (nz: %d total: %lu)\n",
                swhat, trust_rg, trust_rg_level, trust_nz, trust_nz_level,
                non_zero, (unsigned long)r->total);
    }

    if (r->errors)
        r->state = FAILED;

    if (final_compare && r->state == UNTESTED)
        r->state = PASSED; /* optimism */

    return r->state == FAILED ? FAIL : OK;
}

inline int compare_src(const prb_t *p, dnn_mem_t &mem_dt, dnn_mem_t &mem_fp,
        res_t *r, bool final_compare = false)
{ return compare_dat(p, SRC, mem_dt, mem_fp, r, final_compare); }
inline int compare_wei(const prb_t *p, dnn_mem_t &mem_dt, dnn_mem_t &mem_fp,
        res_t *r, bool final_compare = false)
{ return compare_dat(p, WEI, mem_dt, mem_fp, r, final_compare); }
inline int compare_bia(const prb_t *p, dnn_mem_t &mem_dt, dnn_mem_t &mem_fp,
        res_t *r, bool final_compare = false)
{ return compare_dat(p, BIA, mem_dt, mem_fp, r, final_compare); }
inline int compare_dst(const prb_t *p, dnn_mem_t &mem_dt, dnn_mem_t &mem_fp,
        res_t *r, bool final_compare = false)
{ return compare_dat(p, DST, mem_dt, mem_fp, r, final_compare); }

static int fill_src_f32(const prb_t *p, dnn_mem_t &mem_fp)
{
    if( mem_fp.dt() != mkldnn_f32 )
        return FAIL;

    const auto &c = p->cfg[SRC];
    const int range = c.f_max - c.f_min + 1;
#   pragma omp parallel for collapse(4)
    for (int mb = 0; mb < p->mb; ++mb)
    for (int ic = 0; ic < p->ic; ++ic)
    for (int ih = 0; ih < p->ih; ++ih)
    for (int iw = 0; iw < p->iw; ++iw)
    {
        const int gen = 17 * ih + 13 * iw + 13 * mb + 19 * ic + 1637;
        const bool non_base = true
            && gen % (p->kh * p->kw) <= c.f_sparsity * (p->kh * p->kw);
//            && (17 * ih + 13 * mb) % p->kh == 0
//            && (13 * iw + 19 * ic) % p->kw == 0;
        const float value = static_cast<float>(
            non_base ? c.f_min + gen * c.f_step % range : c.f_base);

        ((float*)mem_fp)[src_off_f(p, mb, 0, ic, ih, iw)] = value;
    }
    return OK;
}

inline int fill_src(const prb_t *p, dnn_mem_t &mem_dt, dnn_mem_t &mem_fp,
        res_t *r) {
    auto fill_src_args_ok = p && r
          && mem_fp.dt() == mkldnn_f32
          && mem_fp.md_.format == mkldnn_nchw
          ? OK: FAIL;
    SAFE(fill_src_args_ok, CRIT);

    const bool extra_mem = mem_dt.dt() != mem_fp.dt();
    dnn_mem_t *p_mem_00 = extra_mem
        ? new dnn_mem_t(mem_dt.md_, mkldnn_f32, mkldnn_nchw)
        : &mem_fp;
    dnn_mem_t &mem_00 = *p_mem_00; // ALWAYS f32, nchw

    SAFE(fill_src_f32(p, mem_00), CRIT); // fill (possibly tmp) f32 buffer

    SAFE(mem_dt.reorder(mem_00), WARN); // mem_dt acquires content from mem_00
    if (extra_mem) {
        SAFE(mem_fp.reorder(mem_dt), WARN); // mem_fp acquires content from mem_dt
        SAFE(compare_src(p, mem_fp, mem_00, r), WARN);
        delete &mem_00;
    }

    return OK;
}

// now one of 'mem_dt' or 'mem_fp' may be a nullptr
inline int fill_src_ouch(const prb_t *p, dnn_mem_t *mem_dt, dnn_mem_t *mem_fp,
        res_t *r) {
    if( p == nullptr || r == nullptr || (mem_dt == nullptr && mem_fp == nullptr) )
        return FAIL;

    dnn_mem_t *p_mem_00 = mem_fp;
    if( mem_fp == nullptr || mem_dt->dt() != mem_fp->dt())
        p_mem_00 = new dnn_mem_t(mem_dt->md_, mkldnn_f32, mkldnn_nchw);
                                 // ouch  //
    dnn_mem_t &mem_00 = *p_mem_00;

    const auto &c = p->cfg[SRC];
    const int range = c.f_max - c.f_min + 1;

#   pragma omp parallel for collapse(4)
    for (int mb = 0; mb < p->mb; ++mb)
    for (int ic = 0; ic < p->ic; ++ic)
    for (int ih = 0; ih < p->ih; ++ih)
    for (int iw = 0; iw < p->iw; ++iw)
    {
        const int gen = 17 * ih + 13 * iw + 13 * mb + 19 * ic + 1637;
        const bool non_base = true
            && gen % (p->kh * p->kw) <= c.f_sparsity * (p->kh * p->kw);
//            && (17 * ih + 13 * mb) % p->kh == 0
//            && (13 * iw + 19 * ic) % p->kw == 0;
        const float value = static_cast<float>(
            non_base ? c.f_min + gen * c.f_step % range : c.f_base);

        ((float*)mem_00)[src_off_f(p, mb, 0, ic, ih, iw)] = value;
    }

    if( mem_dt )
        SAFE(mem_dt->reorder(mem_00), WARN);

    if (p_mem_00 != mem_fp) { // i.e. old 'extra_mem' case
        if (mem_fp && mem_dt) {
            SAFE(mem_fp->reorder(*mem_dt), WARN);
            SAFE(compare_src(p, *mem_fp, mem_00, r), WARN);
        }
        delete p_mem_00;
    }

    return OK;
}

inline int fill_wei(const prb_t *p, dnn_mem_t &mem_dt, dnn_mem_t &mem_fp,
    res_t *r) {
    const bool extra_mem = mem_dt.dt() != mem_fp.dt();
    dnn_mem_t *p_mem_00 = extra_mem
        ? new dnn_mem_t(mem_dt.md_, mkldnn_f32, mkldnn_goihw)
        : &mem_fp;
    dnn_mem_t &mem_00 = *p_mem_00;

    const auto &c = p->cfg[WEI];
    const int range = c.f_max - c.f_min + 1;

#   pragma omp parallel for collapse(5)
    for (int g = 0; g < p->g; ++g)
    for (int oc = 0; oc < p->oc / p->g; ++oc)
    for (int ic = 0; ic < p->ic / p->g; ++ic)
    for (int kh = 0; kh < p->kh; ++kh)
    for (int kw = 0; kw < p->kw; ++kw)
    {
        const int gen = 17 * kh + 13 * kw + 13 * oc + 19 * ic + 38;
        const bool non_base = true
            && gen % (p->kh * p->kw) <= c.f_sparsity * (p->kh * p->kw);
//            && (17 * kh + 13 * oc) % p->kh == 0
//            && (13 * kw + 19 * ic) % p->kw == 0;
        const float value = static_cast<float>(
            non_base ? c.f_min + gen * c.f_step % range : c.f_base);

        ((float*)mem_00)[wei_off_f(p, g, oc, ic, kh, kw)] = value;
    }

    SAFE(mem_dt.reorder(mem_00), WARN);
    if (extra_mem) {
        SAFE(mem_fp.reorder(mem_dt), WARN);
        SAFE(compare_wei(p, mem_fp, mem_00, r), WARN);
        delete &mem_00;
    }

    return OK;
}

inline int fill_bia(const prb_t *p, dnn_mem_t &mem_dt, dnn_mem_t &mem_fp,
        res_t *r) {
    const bool extra_mem = mem_dt.dt() != mem_fp.dt();
    dnn_mem_t *p_mem_00 = extra_mem
        ? new dnn_mem_t(mem_dt.md_, mkldnn_f32, mkldnn_x)
        : &mem_fp;
    dnn_mem_t &mem_00 = *p_mem_00;

    const auto &c = p->cfg[BIA];
    const int range = c.f_max - c.f_min + 1;

    const size_t sz = mem_00.nelems();
    for (size_t i = 0; i < sz; ++i) {
        const int gen = (int)(19 * i);
        const bool non_base = true
            && gen % (p->kh * p->kw) <= c.f_sparsity * (p->kh * p->kw);
        const float value = static_cast<float>(
            non_base ? c.f_min + gen * c.f_step % range : c.f_base);

        ((float*)mem_00)[i] = value;
    }

    SAFE(mem_dt.reorder(mem_00), WARN);
    if (extra_mem) {
        SAFE(mem_fp.reorder(mem_dt), WARN);
        SAFE(compare_bia(p, mem_fp, mem_00, r), WARN);
        delete &mem_00;
    }

    return OK;
}

inline int fill_dst(const prb_t *p, dnn_mem_t &mem_dt, dnn_mem_t &mem_fp,
        res_t *r) {
    const bool extra_mem = mem_dt.dt() != mem_fp.dt();
    dnn_mem_t *p_mem_00 = extra_mem
        ? new dnn_mem_t(mem_dt.md_, mkldnn_f32, mkldnn_nchw)
        : &mem_fp;
    dnn_mem_t &mem_00 = *p_mem_00;

    const auto &c = p->cfg[DST];
    const int range = c.f_max - c.f_min + 1;

#   pragma omp parallel for collapse(4)
    for (int mb = 0; mb < p->mb; ++mb)
    for (int oc = 0; oc < p->oc; ++oc)
    for (int oh = 0; oh < p->oh; ++oh)
    for (int ow = 0; ow < p->ow; ++ow)
    {
        const int gen = 19 * oh + 17 * ow + 13 * mb + 13 * oc + 223;
        const bool non_base = true
            && gen % (p->kh * p->kw) <= c.f_sparsity * (p->kh * p->kw);
//            && (19 * oh + 13 * mb) % p->kh == 0
//            && (17 * ow + 13 * oc) % p->kw == 0;
        const float value = static_cast<float>(
            non_base ? c.f_min + gen * c.f_step % range : c.f_base);

        ((float*)mem_00)[dst_off_f(p, mb, 0, oc, oh, ow)] = value;
    }

    SAFE(mem_dt.reorder(mem_00), WARN);
    if (extra_mem) {
        SAFE(mem_fp.reorder(mem_dt), WARN);
        SAFE(compare_dst(p, mem_fp, mem_00, r), WARN);
        delete &mem_00;
    }

    return OK;
}

/** Initialize conv desc as per \c prb_t command-line args */
static int init_conv_desc(mkldnn_convolution_desc_t &cd, const prb_t *p );
/** Finalize conv desc \c cd by querying the convolution primitive \c cpd */
static int finalize_conv_desc(mkldnn_convolution_desc_t &cd, const prb_t *p,
            const mkldnn_primitive_desc_t &cpd );

int init_conv_desc(mkldnn_convolution_desc_t &cd, const prb_t *p )
{
    mkldnn_memory_desc_t src_d, wei_d, bia_d, dst_d;

    mkldnn_dims_t src_dims = {p->mb, p->ic, p->ih, p->iw};
    mkldnn_dims_t wei_dims = {p->g, p->oc / p->g, p->ic / p->g, p->kh, p->kw};
    mkldnn_dims_t bia_dims = {p->oc};
    mkldnn_dims_t dst_dims = {p->mb, p->oc, p->oh, p->ow};

    DNN_SAFE(mkldnn_memory_desc_init(&src_d, 4, src_dims, p->cfg[SRC].dt, mkldnn_any), WARN);
    DNN_SAFE(mkldnn_memory_desc_init(&wei_d, 5, wei_dims, p->cfg[WEI].dt, mkldnn_any), WARN);
    DNN_SAFE(mkldnn_memory_desc_init(&bia_d, 1, bia_dims, p->cfg[BIA].dt, mkldnn_any), WARN);
    //printf(" dst_dims = %d,%d,%d,%d\n",p->mb, p->oc, p->oh, p->ow);
    DNN_SAFE(mkldnn_memory_desc_init(&dst_d, 4, dst_dims, p->cfg[DST].dt, mkldnn_any), WARN);

    int strides[] = {p->sh, p->sw};
    int dilates[] = {p->dh, p->dw};
    int padding[] = {p->ph, p->pw};
    auto bph = [&](int ih, int oh, int kh, int sh, int ph, int dh) {
        return (oh - 1) * sh - ih + ((kh - 1) * (dh + 1) + 1) - ph;
    };
    int padding_r[] = {
        bph(p->ih, p->oh, p->kh, p->sh, p->ph, p->dh),
        bph(p->iw, p->ow, p->kw, p->sw, p->pw, p->dw)};

    mkldnn_alg_kind_t alg = mkldnn_convolution_direct;
    if (p->alg == WINO) alg = mkldnn_convolution_winograd;

    switch (p->dir) {
    case FWD_D: case FWD_B:
        DNN_SAFE(mkldnn_dilated_convolution_forward_desc_init(&cd,
                    mkldnn_forward_inference, alg, &src_d, &wei_d,
                    p->dir == FWD_D ? NULL : &bia_d, &dst_d, strides, dilates,
                    padding, padding_r, mkldnn_padding_zero), WARN);
        break;
    case BWD_D:
        DNN_SAFE(mkldnn_dilated_convolution_backward_data_desc_init(&cd, alg,
                    &src_d, &wei_d, &dst_d, strides, dilates, padding,
                    padding_r, mkldnn_padding_zero), WARN);
        break;
    case BWD_W: case BWD_WB:
        DNN_SAFE(mkldnn_dilated_convolution_backward_weights_desc_init(&cd,
                    alg, &src_d, &wei_d, p->dir == BWD_W ? NULL : &bia_d,
                    &dst_d, strides, dilates, padding, padding_r,
                    mkldnn_padding_zero), WARN);
        break;
    default: DNN_SAFE(mkldnn_invalid_arguments, WARN);
             print(0,"%s\n", "bad init_conv_desc call : unsupported p->dir");
    }
    DNN_SAFE(cd.accum_data_type == p->cfg[ACC].dt
             ? mkldnn_success : mkldnn_unimplemented, CRIT);

    return OK;
}
#if 0 // memory valid checks... musings :
/** memory pointer exists and non-NULL? */
mkldnn_status_t memory_notnull( const primitive_t *memory ){
    void *handle = nullptr;
    mkldnn_status_t ret = mkldnn_memory_get_handle(memory, &handle);
    if( ret == mkldnn_success && handle == nullptr )
        ret = mkldnn_out_of_memory;
    return ret;
}
bool memory_sz( const primitive_desc_t *memory_pd ){
    size_t const sz = mkldnn_memory_primitive_desc_get_size(memory_pd);
    return sz > 0U;
}
#endif

/** We need a "first" convolution to succeed (not skipped).
 * Then we are able to get right-sized memory and initialize it
 * with test data.  This does *not* check for --skip-impl. */
int init_conv_prim_any( mkldnn_primitive_desc_t &cpd, const prb_t *p,
            mkldnn_convolution_desc_t const& cd, res_t *r)
{
    r->state = UNTESTED;
    mkldnn_status_t init_status = mkldnn_success;
    if (p->merge == RELU) {
        mkldnn_convolution_relu_desc_t crd;
        DNN_SAFE(mkldnn_convolution_relu_desc_init(&crd, &cd, 0), WARN);
        init_status = mkldnn_primitive_desc_create_v2(&cpd, &crd,
                p->attr.mkldnn_attr, engine, NULL);
    } else {
        init_status = mkldnn_primitive_desc_create_v2(&cpd, &cd,
                p->attr.mkldnn_attr, engine, NULL);
    }

    if (init_status == mkldnn_unimplemented){
        return r->state = UNIMPLEMENTED, OK;
    } else {
        SAFE(init_status, WARN);
    }

    return OK;
}
int finalize_conv_desc(mkldnn_convolution_desc_t &cd, const prb_t *p,
            const mkldnn_primitive_desc_t &cpd )
{
    auto q = [=](mkldnn_query_t query, int index = 0) {
        return *mkldnn_primitive_desc_query_memory_d(
                mkldnn_primitive_desc_query_pd(cpd, query, index));
    };

    if (p->dir == BWD_D)
        cd.diff_src_desc = q(mkldnn_query_diff_src_pd);
    else
        cd.src_desc = q(mkldnn_query_src_pd);

    if (p->dir & FLAG_WEI)
        cd.diff_weights_desc = q(mkldnn_query_diff_weights_pd);
    else
        cd.weights_desc = q(mkldnn_query_weights_pd);

    if (p->dir & FLAG_BIA) {
        if (p->dir & FLAG_BWD)
            cd.diff_bias_desc = q(mkldnn_query_diff_weights_pd, 1);
        else
            cd.bias_desc = q(mkldnn_query_weights_pd, 1);
    }

    if (p->dir & FLAG_BWD)
        cd.diff_dst_desc = q(mkldnn_query_diff_dst_pd);
    else
        cd.dst_desc = q(mkldnn_query_dst_pd);

    return OK;
}
#if 0
static int finalize_conv_desc( mkldnn_primitive_desc_t it_cpd,
                              const prb_t *p )
{
    //cout<<" update_conv_desc not needed (no-op)"<<endl;
    if (0){
        mkldnn_convolution_desc_t it_cd;
        if(p->merge == NONE){
            DNN_SAFE(mkldnn_primitive_desc_query( it_cpd,
                    mkldnn_query_convolution_d, 0, &it_cd), CRIT);
            return update_conv_desc( it_cd, p, it_cpd );
        } else {
            // RELU is more complicated, conv desc is a subfield.
        }
    }
    return OK;
}
#endif

#if MKLDNN_IO
static char const* tostr(dnn_mem_t const &src_fp, dnn_mem_t const &wei_fp,
            dnn_mem_t const &bia_fp, dnn_mem_t const &dst_fp) {
    const int buflen = 1024;
    static char buf[buflen];
    char *o = &buf[0];
    int r = buflen;
#define CHKBUF do \
    { \
        assert( n > 0 ); \
        if( n > r ){ r=0; } \
        else { o += n; r -= n; } \
    }while(0)
    int n;
#define PRT1( XXX ) do \
    { \
        if( XXX##_fp.active_ ) { \
            n = snprintf(o,r, #XXX ".md_ : "); CHKBUF; \
            n = mkldnn_name_memory_desc(& XXX##_fp.md_, o, r); CHKBUF; \
            n = snprintf(o,r, "\n"); CHKBUF; \
        } else { \
            n = snprintf(o,r, #XXX "     : inactive\n"); CHKBUF; \
        } \
    }while(0)
    PRT1(src);
    PRT1(wei);
    PRT1(bia);
    PRT1(dst);
    buf[buflen-1] = '\0'; // just in case
    return buf;
#undef PRT1
#undef CHKBUF
}
#else
static char const* tostr(dnn_mem_t const &src_fp, dnn_mem_t const &wei_fp,
            dnn_mem_t const &bia_fp, dnn_mem_t const &dst_fp) {
    return "";
}
#endif

static const char* cmp_fp_data(const char* msg, const dnn_mem_t &f32a,
                const dnn_mem_t &f32b ) {
    if( f32a.active_ && f32b.active_ ){
        RT_ASSERT( (f32a.dt() == mkldnn_f32 ));
        RT_ASSERT( (f32b.dt() == mkldnn_f32 ));
        unsigned const nPrt = [&](){ unsigned n=20U, m;
            if( (m=f32a.nelems()) < n ) n = m;
            if( (m=f32b.nelems()) < n ) n = m;
            return n;
        }();
        const int buflen = 1024;
        static char buf[buflen];
        char *o = &buf[0];
        int r = buflen;
#define CHKBUF do \
        { \
            assert( n > 0 ); \
            if( n > r ){ r=0; } \
            else { o += n; r -= n; } \
        }while(0)
        int n;
        for(unsigned i=0U; i<nPrt; ++i){
            n = snprintf(o,r," %s[%3u] = %8.2f, %8.2f%c",
                         msg, i, ((float*)f32a)[i], ((float*)f32b)[i],
                         (i%5U==4U? '\n': ' '));
            CHKBUF;
        }
        n = snprintf(o,r,"\n"); CHKBUF;
        buf[buflen-1] = '\0'; // just in case
        return &buf[0];
    } else {
        return "";
    }
#undef CHKBUF
}

/** run performance loops if bench_mode \& PERF.
 * \return 0/1 OK/FAIL */
static int do_perf( mkldnn_primitive_t prim, res_t *r, const prb_t *p,
                const char *impl=nullptr ) {
    bool want_perf_report = (bench_mode & PERF);
    if (!want_perf_report) // iterating, can skip perf test without failure
        return OK;
    //cout<<" +do_perf"<<bench_mode2str(bench_mode);
    //cout<<": r->state="<<state2str(r->state);
    auto &t = r->timer; // <--- ahaa
    t.reset();
    while (true) {
        SAFE(execute(prim), WARN);
        t.stamp();
        const bool stop = false
            || (fix_times_per_prb && t.times() >= fix_times_per_prb)
            || (!fix_times_per_prb
                && t.total_ms() >= max_ms_per_prb
                && t.times() >= min_times_per_prb);
        if (stop) break;
    }
    //cout<<": r->state="<<state2str(r->state);
    char pstr[max_prb_len];
    prb2str(p, pstr);
    perf_report(p, r, pstr, impl);
    //cout<<": r->state="<<state2str(r->state);
    //cout<<" -do_perf OK"<<endl;
    //r->state = PASSED;
    return OK;
}

/** plain iterator, iterate over "all possible" (version 0).
 * Perhaps need to template on type of primitive_desc? */
struct conv_iter_t {
    /** Generic constructor from \c void* C operation descriptor.
     * \return none but \c (bool) cast false if there were errors,
     *         and \c status() can tell you what went wrong.
     * \note For convolutions, we can <B>probably</B> get away with
     * nullptr hint, but this may not always work, esp. if iterating
     * over layers like batch-normalization, lrn, and pooling.
     * \sa [mkl-dnn issue \#132](https://github.com/01org/mkl-dnn/issues/132)
     */
    conv_iter_t( const_mkldnn_op_desc_t op_desc,
        mkldnn_engine_t engine,
        const_mkldnn_primitive_desc_t hint_forward_primitive_desc=nullptr)
        : n_(0U)
    {
        iter_status_ = mkldnn_primitive_desc_iterator_create
            ( &iter_,
              op_desc,
              engine,
              hint_forward_primitive_desc);
    }
    ~conv_iter_t() {
        RT_ASSERT( mkldnn_primitive_desc_iterator_destroy(iter_)
                   == mkldnn_success );
    }

    mkldnn_status_t status() const { return iter_status_; }
    int n() const { return n_; }
    explicit operator bool() { return iter_status_ == mkldnn_success; }

    /** pre-increment only! */
    conv_iter_t& operator++() {
        if( iter_status_ == mkldnn_success ){
            iter_status_ = mkldnn_primitive_desc_iterator_next( iter_ );
            ++n_;
        }
        return *this;
    }
    /** return primitive descriptor (or nullptr).
     * If *return_value* is non-null, client is responsible for
     * calling \c mkldnn_primitive_desc_destroy(return_value) */
    mkldnn_primitive_desc_t operator*() {
        mkldnn_primitive_desc_t ret = nullptr;
        if (iter_status_ == mkldnn_success){
            ret = mkldnn_primitive_desc_iterator_fetch(iter_);
            if ((void*)ret == nullptr)
               iter_status_ = mkldnn_iterator_ends;
        }
        return ret;
    }
    /** A scoped \c mkldnn_primitive_desc_t object.
     * \deprecated probably should be using the C++ API
     * handle/wrapper stuff, but I didn't have a good example
     * at hand to work from  :( */
    struct scoped_prim_desc {
        scoped_prim_desc( mkldnn_primitive_desc_t prim ) : prim_(prim) {}
        ~scoped_prim_desc() {
            if(prim_){
                RT_ASSERT( mkldnn_primitive_desc_destroy(prim_)
                           == mkldnn_success );
            }
        }
        operator mkldnn_primitive_desc_t() const
        { return prim_; }
        operator const_mkldnn_primitive_desc_t() const
        { return const_cast<const_mkldnn_primitive_desc_t>(prim_); }
        //bool operator==(void* voidptr) const {return prim_==nullptr;}
        operator bool() const { return prim_ != nullptr; }
    private:
        mkldnn_primitive_desc_t prim_;
    };
    /** return a scoped primitive descriptor. */
    scoped_prim_desc get(){
        return scoped_prim_desc(this->operator*());
    }
private:
    mkldnn_primitive_desc_iterator_t iter_;
    mkldnn_status_t iter_status_;
    unsigned n_;
};
/** scoped wrapper around an mkldnn_primitive_t.
 * Ensure matching calls to \c mkldnn_primitive_create
 * and \c mkldnn_primitive_destroy. */
struct scoped_prim{
    scoped_prim( mkldnn_primitive_t &c, const_mkldnn_primitive_desc_t pd,
            const mkldnn_primitive_at_t *inputs, const_mkldnn_primitive_t *outputs)
    : prim_(c)
    {
        status_ = mkldnn_primitive_create(&c, pd, inputs, outputs);
    }
    ~scoped_prim()
    {
        mkldnn_primitive_destroy(prim_); prim_=nullptr;
    }
    explicit operator bool() const {
        return status_ == mkldnn_success;
    }
    private:
        mkldnn_primitive_t& prim_;
        mkldnn_status_t status_;
};
/** scoped wrapper around an mkldnn_primitive_t.
 * Ensure that convolutional layers properly release
 * expanded descriptor resources _crd <EM>for merged layers</EM> .
 * Client <EM>must</EM> check \c (bool)(*this) for error conditions.
 */
struct scoped_op_desc{
    scoped_op_desc( const mkldnn_convolution_desc_t &cd, const merge_t merge )
        : crd_(nullptr)
        , op_desc_(nullptr)
        , status_(mkldnn_runtime_error)
    {
        switch(merge){
        case(NONE):
            op_desc_ = &cd;
            status_ = mkldnn_success;
            break;
        case(RELU):
            crd_ = new mkldnn_convolution_relu_desc_t;
            if( crd_ == nullptr ){
                status_ = mkldnn_out_of_memory;
            } else {
                status_ = mkldnn_convolution_relu_desc_init(crd_, &cd, 0);
                if (status_ == mkldnn_success){
                    op_desc_ = crd_;
                }
            }
            break;
        }
    }
    ~scoped_op_desc(){ if (crd_) delete crd_; }
    operator const_mkldnn_op_desc_t() const { return op_desc_; }
    mkldnn_status_t status() const {return status_;}
    operator bool() const { return status_ == mkldnn_success; }
private:
    mkldnn_convolution_relu_desc_t *crd_;
    const_mkldnn_op_desc_t op_desc_;
    mkldnn_status_t status_;
};

/** fold in one impl status \c st into run result \c r
 * and into overall \c benchdnn_stat. */
static void benchdnn_stat_update( res_state_t st, res_t *r )
{
    auto &bs = benchdnn_stat;
    print(0," benchdnn_stat_update!!!%c", ' ');
    //char const *state = state2str(r->state);
    switch (st) {
    case UNTESTED:
        break;
    case FAILED:
        ++bs.failed;
        break;
    case SKIPPED:
        ++bs.skipped;
        if (r->state == UNTESTED) r->state = SKIPPED;
        break;
    case UNIMPLEMENTED:
        ++bs.unimplemented;
        bs.failed += !allow_unimpl;
        break;
    case MISTRUSTED:
        ++bs.mistrusted;
        // bs.failed++; /* temporal workaround for some tests */
        if (r->state == UNTESTED || r->state == SKIPPED)
            r->state = MISTRUSTED;
        break;
    case PASSED:
        ++bs.passed;
        if (r->state == UNTESTED || r->state == SKIPPED || r->state == MISTRUSTED)
            r->state = PASSED;
        break;
    default:
        RT_ASSERT(!"unknown state");
    }
    ++bs.impls;
}

/** return a short substring of the possibly long convolution function name.
 * Short and generic enough to move into \c mkldnn_primitive_desc as a
 * static helper function.
 */
static char const* shorten(char const* impl_str)
{
    return mkldnn_primitive_desc_shorten(impl_str);
}

/** \c run_fn a benchdnn test loop and return status of \c compare_fn.
 * Accumulate stats and report as per \c bench_mode TEST/PERF. */
static void test_impl_compare( const prb_t* p, res_t* r, const size_t imp,
        std::function<void(void)> run_fn, std::function<int(void)> compare_fn )
{ /* always do a single test run, for pass/error status */
    auto &bs = benchdnn_stat;
    assert( imp <= get_nref_impls() && imp >= 0 );
    char const* impname = get_ref_impls()[imp].name;
    benchdnn_timer_t tt;
    tt.start();
    run_fn();
    tt.stop();
    print(5, "compare impl[%lu] %s vs impl[0]", (unsigned long)imp, impname);
    int status = compare_fn();
    if (!(bench_mode & PERF)) {
        bs.ts->update_impl(p, r, status, tt, imp);
    }
    if ((bench_mode & PERF)) {
        auto &t = r->timer;
        t.reset();
        const int msmax = 50; const int repmax = 10;
        while (true) {
            t.start();
            run_fn();
            t.stop();
            const bool stop = false
                || (0 /* fix_times_per_prb */
                        && t.times() >= repmax/*fix_times_per_prb*/ )
                || (1 /* !fix_times_per_prb */
                        && t.total_ms() >= msmax/*max_ms_per_prb*/
                        && t.times() >= 1/*min_times_per_prb*/ );
            if (stop) break;
        }
        if(0) {
            char impl_str[20];
            snprintf(&impl_str[0], 20, "TEST:%2lu %-8s", imp, impname);
            char pstr[max_prb_len];
            prb2str(p, pstr);
            perf_report(p, r, pstr, &impl_str[0]);
        } else {
            bs.ts->update_impl(p, r, status, tt, imp);
        }
    }
}
static void test_impl_end(res_t *r){
    auto &bs = benchdnn_stat;
    bool all_passed = bs.ts->end_impls();
    r->state = (all_passed? PASSED: MISTRUSTED);
    printf(" TEST all_passed=%d ",(int)all_passed);
    RT_ASSERT( (bench_mode & PERF) );
    if (!(bench_mode&CORR)) {
        if(all_passed) ++bs.mistrusted; else ++bs.test_fail;
        ++bs.impls;
    }
}

int doit(const prb_t *p, res_t *r) {
    auto &bs = benchdnn_stat;
    if ((bench_mode & TEST)) RT_ASSERT( bs.ts != nullptr );
    char pstr[max_prb_len];
    prb2str(p, pstr);
    res_t res_zero{};
    *r = res_zero;
    RT_ASSERT(r->state == UNTESTED);
    //printf(" doit: dst_dims = %d,%d,%d,%d\n",p->mb, p->oc, p->oh, p->ow);

    mkldnn_convolution_desc_t cd;
    {
        auto cd_init = [&]()->int{
            mkldnn_primitive_desc_t cpd;
            //SAFE(init_pd(p, cd, cpd, r), WARN);
            // --> oops if best-impl is a skip-impl, so test for skip-impl later.
            // Intialize convolution in "must-succeed" mode, so that we
            // can set memory descriptors for convolution primitives
            SAFE(init_conv_desc(cd, p), WARN);              // test data
            SAFE(init_conv_prim_any( cpd, p, cd, r ), WARN);// primitive desc
            if( r->state == UNTESTED ){
                SAFE(finalize_conv_desc( cd, p, cpd ), CRIT);     // finish up
                DNN_SAFE(mkldnn_primitive_desc_destroy(cpd), WARN);
            } else {
                RT_ASSERT( r->state == SKIPPED  || r->state == UNIMPLEMENTED );
                return FAILED;
            }
            return OK;
        };
        if (cd_init() != OK){
            if( r->state == UNTESTED ) r->state = UNIMPLEMENTED;
            benchdnn_stat_update( r->state, r );
            print(0, " Oops: primitive descriptor %s\n", state2str(r->state));
            return FAIL;
        }
    }
    // improvement: TEST with dilates should not return UNIMPLEMENTED just
    // because mkl-dnn does not support BWD dirn with dilates.
    print(0, "%s\n", ""); //"(setting up test data)");
    RT_ASSERT( r->state == UNTESTED || r->state == UNIMPLEMENTED );

    auto &src_dt_d = p->dir == BWD_D ? cd.diff_src_desc : cd.src_desc;
    auto &wei_dt_d = p->dir & FLAG_WEI ? cd.diff_weights_desc : cd.weights_desc;
    auto &bia_dt_d = p->dir & FLAG_BWD ? cd.diff_bias_desc : cd.bias_desc;
    auto &dst_dt_d = p->dir & FLAG_BWD ? cd.diff_dst_desc: cd.dst_desc;

    dnn_mem_t src_dt(src_dt_d, p->cfg[SRC].dt);
    dnn_mem_t wei_dt(wei_dt_d, p->cfg[WEI].dt);
    dnn_mem_t dst_dt(dst_dt_d, p->cfg[DST].dt);
    dnn_mem_t bia_dt = dnn_mem_t::optional(p->dir & FLAG_BIA,
            bia_dt_d, p->cfg[BIA].dt);

    const auto fp = mkldnn_f32;
    dnn_mem_t src_fp(src_dt_d, fp, mkldnn_nchw);
    dnn_mem_t wei_fp(wei_dt_d, fp, mkldnn_goihw);
    dnn_mem_t dst_fp(dst_dt_d, fp, mkldnn_nchw);
    dnn_mem_t bia_fp = dnn_mem_t::optional( p->dir & FLAG_BIA,
            bia_dt_d, fp, mkldnn_x);

    SAFE(fill_src(p, src_dt, src_fp, r), WARN);
    SAFE(fill_wei(p, wei_dt, wei_fp, r), WARN);
    SAFE(fill_dst(p, dst_dt, dst_fp, r), WARN);
    if (p->dir & FLAG_BIA)
        SAFE(fill_bia(p, bia_dt, bia_fp, r), WARN);
    if( r->state != UNTESTED ){
        print(0," Oops: setting test data: %s", state2str(r->state));
        benchdnn_stat_update( r->state, r );
        r->state = UNTESTED;
    }

    mkldnn_primitive_t c{};
    //int const v = verbose;
    char const* impl = nullptr;
    size_t const nimp = get_nref_impls();
    if (p->dir & FLAG_FWD) {
        mkldnn_primitive_at_t inputs[3] = { {src_dt.p_, 0}, {wei_dt.p_, 0},
            {p->dir & FLAG_BIA ? bia_dt.p_ : NULL, 0}
        };
        const_mkldnn_primitive_t outputs[] = { dst_dt.p_ };
        if (bench_mode & CORR || bench_mode & TEST )
            compute_ref_fwd(p, src_fp, wei_fp, bia_fp, dst_fp);
        if ((bench_mode & TEST) && nimp > imp0){
            //printf(" FWD TESTS... ");
            /* any optional floating point reference loops?          */
            /* similar to CORR/PERF tests, but always floating point */
            bs.ts->begin_impls();
            for(size_t imp=imp0; imp<nimp; ++imp){
                auto impl_fn = get_ref_impls()[imp].fwd;
                if (impl_fn == nullptr) continue;
                /* get new zeroed data "just like" the ref fp32 calc */
                print(2," imp=%lu/%lu : %s ...\n", (unsigned long)imp,
                        (unsigned long)nimp, "test loop mem descriptors");
                /* inputs (for FWD calc) acquire content from ref fp32 calc */
                /*dnn_mem_t src_tt(src_fp); // separate, zeroed data */
                /*src_tt.reorder(src_fp);   // + acquire data explicitly */
                /* not most elegant, in principle could reuse const input mem */
                dnn_mem_t src_tt(src_fp, fp); /* same layout, copied data */
                dnn_mem_t wei_tt(wei_fp, fp); /* same layout, copied data */
                dnn_mem_t bia_tt(bia_fp, fp); /* now OK for !active_ */
                dnn_mem_t dst_tt(dst_fp.md_); /* separate, ZEROED data */
                if(verbose){
                    if (imp == imp0)
                        print(2, "%s", tostr(src_fp, wei_fp, bia_fp, dst_fp));
                    print(3, "%s", cmp_fp_data("src", src_fp, src_tt));
                    print(3, "%s", cmp_fp_data("wei", wei_fp, wei_tt));
                    print(3, "%s", cmp_fp_data("bia", bia_fp, bia_tt));
                    print(1, "conv fwd test imp %lu ", (long unsigned)imp);
                }
                auto run_fn = [&]()->void{
                    (*impl_fn)(p, src_tt, wei_tt, bia_tt, dst_tt);
                };
                auto compare_fn = [&]()->int{ /* return OK / FAIL */
                    if(verbose>1) cmp_fp_data("dst", dst_fp, dst_tt);
                    SAFE(compare_dst(p, dst_tt, dst_fp, r), WARN);
                    return OK;
                };
                test_impl_compare(p, r, imp, run_fn, compare_fn);
            }
            //printf(" FWD TESTS END... ");
            test_impl_end(r);
        }
        if( ((bench_mode & PERF) && !(bench_mode & TEST))
                || bench_mode & CORR ) {
            r->state = UNTESTED; // ignore errors in test loops (just report)
            //
            auto fwd_test = [&c,&r,&p,&dst_fp,&dst_dt,&impl]()
                ->int{
                    SAFE(execute(c), WARN);
                    if( do_perf(c, r, p, impl) != OK ) return FAIL;
                    if (bench_mode & CORR) {
                        dnn_mem_t dst(dst_dt, fp, mkldnn_nchw);
                        SAFE(dst.reorder(dst_dt), WARN);
                        SAFE(compare_dst(p, dst, dst_fp, r, true), WARN);
                    }
                    return OK;
                };
            // iterate over impls
            scoped_op_desc copd( cd, p->merge ); // basic or merged conv layer?
            DNN_SAFE(copd.status(), CRIT);
            // NOTE: For now, we **depend** on convolutions NOT requiring a fwd hint.
            // Alternatively, we could grab a "default" fwd hint from above,
            // and assume that hint compatibility is properly checked by mkldnn
            for ( conv_iter_t pit(copd, engine, NULL); (bool)pit; ++pit) {
#define ITERATE_OVER_IMPLS_BEGIN \
                print((bench_mode&PERF?10:0),"impl#%u,", pit.n()); \
                auto pd_n = pit.get(); /* a scoped mkldnn_primitive_desc_t */ \
                if( !pd_n ){ print(2," pd_%u==nullptr!?\n",pit.n()); break; } \
                /* Not needed: update_conv_desc( pd_n, p ); */ \
                const char *impl_str = query_impl_info(pd_n); \
                impl = (verbose>0? impl_str: shorten(impl_str)); \
                res_state_t pitst = UNTESTED;
                // in: bench_mode, pit
                // out: impl_str, impl pitst, pd_n
                ITERATE_OVER_IMPLS_BEGIN;
#define ITERATE_OVER_IMPLS_TEST( test_ok_fail ) \
                if (maybe_skip(impl_str)) pitst = SKIPPED; \
                else { \
                    print(20, " %s,%s\n", impl, dir2str(p->dir)); \
                    scoped_prim prim_create(c, pd_n, inputs, outputs); \
                    SAFE((bool)prim_create? OK: FAIL, WARN); \
                    pitst = (test_ok_fail() != OK? FAILED: PASSED); \
                }
                // in: impl_str, pitst, p, pd_n, inputs, outputs
                // out: c
                ITERATE_OVER_IMPLS_TEST( fwd_test );
#define ITERATE_OVER_IMPLS_END \
                benchdnn_stat_update(pitst, r); \
                print((pitst==PASSED? 10: pitst==SKIPPED? 1: 0), \
                      "%s#%u,%s,%s\n", state2str(pitst), pit.n(), \
                      impl, dir2str(p->dir)); \
                if (pitst == SKIPPED) continue; \
                if(! (bench_mode & ALL)) break;
                // in: pitst, r, pit, impl, p, bench_mode
                // out: <> + continue/break
                ITERATE_OVER_IMPLS_END;
            }
            if (r->state == UNTESTED){ // paranoia
                r->state = UNIMPLEMENTED;
                benchdnn_stat_update(r->state, r);
            }
        }
    } else if (p->dir == BWD_D) {
        mkldnn_primitive_at_t inputs[3] = { {dst_dt.p_, 0}, {wei_dt.p_, 0}, };
        const_mkldnn_primitive_t outputs[] = { src_dt.p_ };
        if (bench_mode & CORR || bench_mode & TEST )
            compute_ref_bwd_d(p, src_fp, wei_fp, dst_fp);
        if ((bench_mode & TEST) && nimp > imp0){
            for(size_t imp=imp0; imp<nimp; ++imp){
                auto impl_fn = get_ref_impls()[imp].bwd_d;
                if (impl_fn == nullptr) continue;
                dnn_mem_t src_tt(src_fp.md_); /* ZEROED */
                dnn_mem_t wei_tt(wei_fp, fp); /* copied (should reuse) */
                //dnn_mem_t bia_tt(bia_fp, fp); /* not needed */
                dnn_mem_t dst_tt(dst_fp, fp); /* copied (should reuse) */
                auto run_fn = [&]()->void{
                    (*impl_fn)(p, src_tt, wei_tt, dst_tt);
                };
                auto compare_fn = [&]()->int{ // like bwd_d_test, but float
                    SAFE(compare_src(p, src_tt, src_fp, r), WARN);
                    return OK;
                };
                test_impl_compare(p, r, imp, run_fn, compare_fn);
            }
            test_impl_end(r);
        }
        if( ((bench_mode & PERF) && !(bench_mode & TEST))
                || bench_mode & CORR ) {
            r->state = UNTESTED; // ignore errors in test loops (just report)
            // iterate over impls
            auto bwd_d_test = [&c,&r,&p,&src_dt,&src_fp,&impl]()
                ->int{
                    SAFE(execute(c), WARN);
                    if( do_perf(c, r, p, impl) != OK ) return FAIL;
                    if (bench_mode & CORR) {
                        dnn_mem_t src(src_dt, fp, mkldnn_nchw);
                        SAFE(src.reorder(src_dt), WARN);
                        SAFE(compare_src(p, src, src_fp, r, true), WARN);
                    }
                    return OK;
                };
            scoped_op_desc copd( cd, p->merge ); // basic or merged conv layer?
            DNN_SAFE(copd.status(), CRIT);
            for (conv_iter_t pit(copd, engine, NULL); (bool)pit; ++pit) {
                ITERATE_OVER_IMPLS_BEGIN;
                ITERATE_OVER_IMPLS_TEST( bwd_d_test );
                ITERATE_OVER_IMPLS_END;
            }
            if (r->state == UNTESTED){ // paranoia
                r->state = UNIMPLEMENTED;
                benchdnn_stat_update(r->state, r);
            }
        }
    } else if (p->dir & FLAG_BWD && p->dir & FLAG_WEI) {
        mkldnn_primitive_at_t inputs[3] = { {src_dt.p_, 0}, {dst_dt.p_, 0}, };
        const_mkldnn_primitive_t outputs[] = { wei_dt.p_,
            p->dir & FLAG_BIA ? bia_dt.p_ : NULL,
        };
        if (bench_mode & CORR || bench_mode & TEST )
            compute_ref_bwd_w(p, src_fp, wei_fp, bia_fp, dst_fp);
        if ((bench_mode & TEST) && nimp > imp0){
            for(size_t imp=imp0; imp<nimp; ++imp){
                auto impl_fn = get_ref_impls()[imp].bwd_w;
                if (impl_fn == nullptr) continue;
                dnn_mem_t src_tt(src_fp, fp); /* copied (should reuse) */
                dnn_mem_t wei_tt(wei_fp.md_); /* ZEROED */
                /* [ejk] dnn_mem_t now handles bia_fp.md_ inactive */
                dnn_mem_t bia_tt(bia_fp.md_); /* ZEROED (or inactive) */
                dnn_mem_t dst_tt(dst_fp, fp); /* copied (should reuse) */
                auto run_fn = [&]()->void{
                    (*impl_fn)(p, src_tt, wei_tt, bia_tt, dst_tt);
                };
                auto compare_fn = [&]()->int{ // like bwd_w_test, but both float
                    SAFE(compare_wei(p, wei_tt, wei_fp, r), WARN);
                    if (p->dir & FLAG_BIA) {
                        SAFE(compare_bia(p, bia_tt, bia_fp, r), WARN);
                    }
                    return OK;
                };
                test_impl_compare(p, r, imp, run_fn, compare_fn);
            }
            test_impl_end(r);
        }
        if( ((bench_mode & PERF) && !(bench_mode & TEST))
                || bench_mode & CORR ) {
            r->state = UNTESTED; // ignore errors in test loops (just report)
            auto bwd_w_test = [&c,&r,&p,&wei_dt,&wei_fp,&bia_dt,&bia_fp,&impl]()
                            ->int{
                SAFE(execute(c), WARN);
                if( do_perf(c, r, p, impl) != OK ) return FAIL;
                if (bench_mode & CORR) {
                    dnn_mem_t wei(wei_dt, fp, mkldnn_goihw);
                    SAFE(wei.reorder(wei_dt), WARN);
                    SAFE(compare_wei(p, wei, wei_fp, r, true), WARN);
                    if (p->dir & FLAG_BIA) {
                        dnn_mem_t bia(bia_dt, fp, mkldnn_x);
                        SAFE(bia.reorder(bia_dt), WARN);
                        SAFE(compare_bia(p, bia, bia_fp, r, true), WARN);
                    }
                }
                return OK;
            };
            // iterate over impls
            scoped_op_desc copd( cd, p->merge ); // basic or merged conv layer?
            DNN_SAFE(copd.status(), CRIT);
            for (conv_iter_t pit(copd, engine, NULL); (bool)pit; ++pit) {
                ITERATE_OVER_IMPLS_BEGIN;
                ITERATE_OVER_IMPLS_TEST( bwd_w_test );
                ITERATE_OVER_IMPLS_END;
            }
            if (r->state == UNTESTED){ // paranoia
                r->state = UNIMPLEMENTED;
                benchdnn_stat_update(r->state, r);
            }
        }
    } else {
        print(0," p->dir = %s\n", dir2str(p->dir));
        RT_ASSERT(!"unhandled problem direction p->dir");
    }
    if ((bench_mode & TEST) && !(nimp > imp0)){
        print(1, "%s\n", "TEST mode: no alternate test convolutions");
    }
    if ((bench_mode & TEST) && !(nimp > imp0)){
        print(1, "%s\n", "TEST mode: no alternate test convolutions");
    }

    return OK;
#undef ITERATE_OVER_IMPLS_END
#undef ITERATE_OVER_IMPLS_TEST
#undef ITERATE_OVER_IMPLS_BEGIN
}

} 
// vim: et ts=4 sw=4 cindent ai cino=^=l0,\:0,N-s
