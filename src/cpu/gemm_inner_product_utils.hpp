/*******************************************************************************
* Copyright 2019 Intel Corporation
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

#ifndef CPU_GEMM_INNER_PRODUCT_UTILS_HPP
#define CPU_GEMM_INNER_PRODUCT_UTILS_HPP

#include "cpu_isa_traits.hpp"
#include "c_types_map.hpp"
#include "cpu_engine.hpp"
#include "cpu_inner_product_pd.hpp"
#if TARGET_X86_JIT
#include "jit_avx512_core_bf16cvt.hpp"
#include "jit_generator.hpp"
#include "jit_uni_eltwise_injector.hpp"
#endif // TARGET_X86_JIT
#include "ref_eltwise.hpp"
#include "type_helpers.hpp"
#include "utils.hpp"

namespace dnnl {
namespace impl {
namespace cpu {

namespace inner_product_utils {

template <impl::data_type_t acc_type, impl::data_type_t dst_type>
class pp_kernel_t
#if TARGET_X86_JIT
: jit_generator
#endif // TARGET_X86_JIT
{
public:
#if TARGET_X86_JIT
    DECLARE_CPU_JIT_AUX_FUNCTIONS(gemm_x8s8s32x_inner_product_fwd_t::pp_kernel);
#endif // TARGET_X86_JIT
    pp_kernel_t(size_t OC, size_t MB, const primitive_attr_t *attr,
            data_type_t bias_dt, bool skip_sum);
    pp_kernel_t(const cpu_inner_product_fwd_pd_t *pd, bool skip_sum);
    ~pp_kernel_t() {
        if (do_eltwise_) {
#if TARGET_X86_JIT
            if (eltwise_injector_) delete eltwise_injector_;
#endif // TARGET_X86_JIT
            if (ref_eltwise_) delete ref_eltwise_;
        }
#if TARGET_X86_JIT // (x86 jit without bfloat support is not supported)
        delete bf16_emu_;
#endif // TARGET_X86_JIT
    }

    typedef typename prec_traits<acc_type>::type acc_data_t;
    typedef typename prec_traits<dst_type>::type dst_data_t;

    // mb kernel only supports single-threaded execution where performance
    // degradation is larger
    bool sequential_kernel() const { return mb_blk_kernel; }

    void operator()(dst_data_t *dst, const acc_data_t *acc, const char *bias,
            const float *scales, size_t start, size_t end,
            size_t runtime_oc = 0, const float *dst_zero_points = nullptr);

private:
#if TARGET_X86_JIT
    void generate();
#endif // TARGET_X86_JIT
    void compute_oc_channel_blk();
    void compute_mb_blk(); // vectorize across minibatch

    struct ker_args {
        dst_data_t *dst;
        const acc_data_t *acc;
        const char *bias;
        const float *scales;
        const float *dst_zero_points;
        float nslope;
        size_t oc;
        size_t len;
        size_t oc_offset;
    };

    enum { default_OC_loop_unroll_ = 4 };

    void (*ker_)(const ker_args *args);
    ref_eltwise_scalar_fwd_t *ref_eltwise_;

#if TARGET_X86_JIT
    jit_uni_eltwise_injector_f32<avx512_core> *eltwise_injector_;
    bf16_emulation_t *bf16_emu_;

    Xbyak::Reg64 reg_param = abi_param1;
    Xbyak::Reg64 reg_dst = rdx;
    Xbyak::Reg64 reg_acc = rax;
    Xbyak::Reg64 reg_bias = rbx;
    Xbyak::Reg64 reg_scales = rsi;

    Xbyak::Reg64 reg_oc = r13;
    Xbyak::Reg64 reg_len = r8;
    Xbyak::Reg64 reg_tmp = rcx; // intentional for shifting purposes
    Xbyak::Reg64 reg_oc_offset = r9;
    Xbyak::Reg64 reg_rem_mask = r10;
    Xbyak::Opmask kreg_rem_mask = k1;

    // Will be assigned in constructor
    Xbyak::Zmm vreg_zero, vreg_scale, vreg_sum_scale, vreg_dst_zero_points;

    Xbyak::Reg64 eltwise_reserved_1_ = r11;
    Xbyak::Opmask eltwise_reserved_2_ = k2;

    Xbyak::Zmm bf16_emu_reserv_1 = Xbyak::Zmm(28);
    Xbyak::Zmm bf16_emu_reserv_2 = Xbyak::Zmm(29);
    Xbyak::Zmm bf16_emu_reserv_3 = Xbyak::Zmm(30);
    Xbyak::Reg64 bf16_emu_reserv_4 = r12;
    Xbyak::Zmm bf16_emu_reserv_5 = Xbyak::Zmm(31);
#endif // TARGET_X86_JIT

    size_t OC_;
    size_t MB_;
    data_type_t bias_data_type_;
    size_t bias_data_type_size_;
    bool do_eltwise_;
    post_ops_t::entry_t::eltwise_t eltwise_;
    bool do_scale_;
    size_t scale_idx_mult_;
    bool do_sum_;
    bool do_dst_zero_points_;
    float sum_scale_;
#if TARGET_X86_JIT
    cpu_isa_t isa_;
    int max_OC_loop_unroll_;
    int idx_compute_vreg_start_;
    int idx_compute_vreg_max_;
    int compute_vregs_per_iter_;
    int compute_vreg_bias_shift_, compute_vreg_prev_dst_shift_;
#endif // TARGET_X86_JIT
    bool mb_blk_kernel;

    const size_t vlen = cpu_isa_traits<avx512_core>::vlen / sizeof(float);

    bool do_bias() const { return bias_data_type_ != data_type::undef; }
    bool runtime_oc() const { return OC_ == (size_t)DNNL_RUNTIME_DIM_VAL; }
    bool runtime_mb() const { return MB_ == (size_t)DNNL_RUNTIME_DIM_VAL; }

#if TARGET_X86_JIT
    Xbyak::Zmm vreg_dst(int iter) {
        int idx = idx_compute_vreg_start_ + iter * compute_vregs_per_iter_;
        assert(idx <= idx_compute_vreg_max_);
        return Xbyak::Zmm(idx);
    };

    Xbyak::Zmm vreg_prev_dst(int iter) {
        int idx = idx_compute_vreg_start_ + iter * compute_vregs_per_iter_
                + compute_vreg_prev_dst_shift_;
        assert(idx <= idx_compute_vreg_max_);
        return Xbyak::Zmm(idx);
    };

    Xbyak::Zmm vreg_bias(int iter) {
        int idx = idx_compute_vreg_start_ + iter * compute_vregs_per_iter_
                + compute_vreg_bias_shift_;
        assert(idx <= idx_compute_vreg_max_);
        return Xbyak::Zmm(idx);
    };
#endif // TARGET_X86_JIT
};

} // namespace inner_product_utils

} // namespace cpu
} // namespace impl
} // namespace dnnl

// vim: et ts=4 sw=4 cindent cino=+2s,^=l0,\:0,N-s
#endif
