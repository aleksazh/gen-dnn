/*******************************************************************************
* Copyright 2016-2017 Intel Corporation
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

#include <cmath>

#include "c_types_map.hpp"
#include "type_helpers.hpp"

#include "ref_lrn.hpp"

namespace mkldnn {
namespace impl {

// in future, could move to mkldnn_traits.hpp ?    
template <data_type_t> struct lrn_promote {}; /* ::type -> promoted float type */

/** float_t for calcs like pow, accsqr_t for summing squares.
 *  traits-like in case you want to go to double someday. */
template <> struct lrn_promote<data_type::f32> { typedef float float_t; typedef float accsqr_t; };
template <> struct lrn_promote<data_type::s32> { typedef float float_t; typedef float accsqr_t; };
template <> struct lrn_promote<data_type::s16> { typedef float float_t; typedef float accsqr_t; };
template <> struct lrn_promote<data_type::s8>  { typedef float float_t; typedef uint32_t accsqr_t; };
template <> struct lrn_promote<data_type::u8>  { typedef float float_t; typedef uint32_t accsqr_t; };

namespace cpu {

template <impl::data_type_t data_type>
void ref_lrn_fwd_t<data_type>::execute_forward() {
    using namespace alg_kind;
    typedef typename impl::lrn_promote<data_type>::float_t float_t;
    typedef typename impl::lrn_promote<data_type>::accsqr_t accsqr_t;

    auto src = reinterpret_cast<const data_t *>(this->input_memory(0));
    auto dst = reinterpret_cast<data_t*>(this->memory(0));
    auto ws = reinterpret_cast<data_t*>(this->memory(1));

    const memory_desc_wrapper data_d(conf_.src_pd());
    const memory_desc_wrapper ws_d(conf_.workspace_pd());

    const int C = conf_.C();
    const int H = conf_.H();
    const int W = conf_.W();
    const bool across_channels = conf_.desc()->alg_kind == lrn_across_channels;

    auto ker = [=](data_t *d, int mb, int oc, int oh, int ow) {
        const double alpha = conf_.desc()->lrn_alpha;
        const float_t beta = static_cast<float_t>(conf_.desc()->lrn_beta);
        const double k = conf_.desc()->lrn_k;

        const int size = conf_.desc()->local_size;
        const int CSIZE = across_channels ? size : 1;
        const int HWSIZE = size + 1 - CSIZE;

        accsqr_t sum = 0;
        int summands = across_channels ? size : size*size;
        for (int c = oc; c < oc + CSIZE; ++c) {
            if (c < (CSIZE - 1) / 2) continue;
            if (c >= C + (CSIZE - 1) / 2) continue;
            for (int h = oh; h < oh + HWSIZE; ++h) {
                if (h < (HWSIZE - 1) / 2) continue;
                if (h >= H + (HWSIZE - 1) / 2) continue;
                for (int w = ow; w < ow + HWSIZE; ++w) {
                    if (w < (HWSIZE - 1) / 2) continue;
                    if (w >= W + (HWSIZE - 1) / 2) continue;
                    accsqr_t s = src[data_d.off(mb, c - (CSIZE - 1) / 2,
                            h - (HWSIZE - 1) / 2, w - (HWSIZE - 1) / 2)];
                    sum += s * s;
                }
            }
        }
        const double fsum = k + alpha * sum / summands;
        const data_t dsum = static_cast<data_t>(fsum);
        if (ws)
            ws[ws_d.off(mb, oc, oh, ow)] = dsum; // for back prop
        d[0] = static_cast<data_t>(src[data_d.off(mb, oc, oh, ow)] / std::pow(static_cast<float_t>(dsum), beta));
    };

    const int MB = conf_.MB();
#   pragma omp parallel for collapse(4) schedule(static)
    for (int mb = 0; mb < MB; ++mb) {
        for (int c = 0; c < C; ++c) {
            for (int h = 0; h < H; ++h) {
                for (int w = 0; w < W; ++w) {
                    ker(&dst[data_d.off(mb, c, h, w)], mb, c, h, w);
                }
            }
        }
    }
}

template <impl::data_type_t data_type>
void ref_lrn_bwd_t<data_type>::execute_backward() {
    using namespace alg_kind;
    typedef typename impl::lrn_promote<data_type>::float_t float_t;
    typedef typename impl::lrn_promote<data_type>::accsqr_t accsqr_t;

    auto src = reinterpret_cast<const data_t *>(this->input_memory(0));
    auto diff_dst = reinterpret_cast<const data_t *>(this->input_memory(1));
    //auto ws = reinterpret_cast<data_t*>(this->memory(3)); // unused
    auto diff_src = reinterpret_cast<data_t*>(this->memory(0));

    const memory_desc_wrapper data_d(conf_.src_pd());
    const memory_desc_wrapper diff_data_d(conf_.diff_dst_pd());
    //const memory_desc_wrapper ws_d(conf_.workspace_pd()); // unused

    const int MB = conf_.MB();
    const int C = conf_.C();
    const int H = conf_.H();
    const int W = conf_.W();

    // until f64 is here, these can be float:
    const float_t alpha = static_cast<float_t>(conf_.desc()->lrn_alpha);
    const float_t beta  = static_cast<float_t>(conf_.desc()->lrn_beta);
    const float_t k     = static_cast<float_t>(conf_.desc()->lrn_k);
    const int kernel_size = conf_.desc()->local_size;

    auto get_omega = [=](data_t c_k, int kernel_size, double alpha, int C,
            const data_t *src, int n, int c, int h, int w) -> float_t {
        accsqr_t sum = 0;

        int half_kernel_size = (kernel_size - 1) / 2;
        int c_start = (c < half_kernel_size) ? 0 : c - half_kernel_size;
        int c_end = c + kernel_size - half_kernel_size;
        c_end = c_end < C ? c_end : C;
        for (int i = c_start; i < c_end; ++i) {
            const accsqr_t value = src[data_d.off(n, i, h, w)];
            sum += value * value;
        }
        const auto fsum = sum * alpha / kernel_size;
        return static_cast<data_t>(c_k + fsum);
    };

    auto ker = [=](data_t *d, int mb, int oc, int oh, int ow) {
        int ks_start = kernel_size/2 > oc ? kernel_size/2 - oc : 0;
        int ks_stop = C - oc <= kernel_size/2 ? C - oc + kernel_size/2 : kernel_size;

        float_t A = 0, B = 0, omega_mid = 0;

        for (int ks = ks_start; ks < ks_stop; ks++) {
            int _t = oc + ks - (kernel_size/2);
            const float_t omega = get_omega(k, kernel_size, alpha, C,
                    src, mb, _t, oh, ow);

            if (ks == kernel_size/2) omega_mid = omega;

            const float_t t = src[data_d.off(mb, _t, oh, ow)]
                    / std::pow(static_cast<float_t>(omega), beta);
            B +=  diff_dst[diff_data_d.off(mb, _t, oh, ow)] * t / omega;
        }

        A = diff_dst[diff_data_d.off(mb, oc, oh, ow)] / std::pow(omega_mid, beta);
        B *= src[data_d.off(mb, oc, oh, ow)];
        B *= (2.0f * alpha * beta) / kernel_size;
        *d = static_cast<data_t>(A - B);
    };

#   pragma omp parallel for collapse(4) schedule(static)
    for (int mb = 0; mb < MB; ++mb) {
        for (int c = 0; c < C; ++c) {
            for (int h = 0; h < H; ++h) {
                for (int w = 0; w < W; ++w) {
                    ker(&diff_src[diff_data_d.off(mb, c, h, w)], mb, c, h, w);
                }
            }
        }
    }
}

template struct ref_lrn_fwd_t<data_type::f32>;
template struct ref_lrn_bwd_t<data_type::f32>;

}
}
}

// vim: et ts=4 sw=4 cindent cino^=l0,\:0,N-s
