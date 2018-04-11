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

#ifndef CPU_ISA_HPP
#define CPU_ISA_HPP

#if defined(TARGET_VANILLA) && !defined(JITFUNCS)
/** In principle could have multiple TARGETS.
 * For example: VANILLA, and later perhaps SSE42, AVX2, AVX512.
 * Default is "compile everything".
 * These TARGETS can be set in cmake to generate reduced-functionality libmkldnn.
 * Which jit impls get included in the engine is capped by a single value.
 *
 *
 * For example, TARGET_VANILLA includes NO Intel JIT code at all, and is suitable
 * for [cross-]compiling for other platforms.
 *
 * \note TARGET_VANILLA impls are not *yet* optimized for speed! */
#define JITFUNCS 0
#endif

#ifndef JITFUNCS
/* default mkl-dnn compile works as usual, 100 means include all impls */
#define JITFUNCS 100
#endif

//#if JITFUNCS == 0
//#warning "JITFUNCS == 0"
//#endif

namespace mkldnn {
namespace impl {
namespace cpu {

//@{
/** \ref JITFUNCS compile-time thresholds.
 *
 * <em>gen-dnn</em> compile introduces "vanilla" target for a library that can run on any CPU.
 * It removes all jit and xbyak code, and runs... slowly.
 *
 * This is set by compile time flags <b>-DTARGET_VANILLA -DJITFUNCS=0</b>.
 *
 * <em>mkl-dnn</em> compile by default uses <b>-DJITFUNCS=100</b>,
 * which compiles xbyak and includes all jit implementations.
 *
 * To remove a subset of implementations from libmkldnn (well, at least remove them from the
 * default list in \ref cpu_engine.cpp ) you can compare the JITFUNCS value with these
 * thresholds.
 *
 * - Example:
 *   - '#if JITFUNCS >= JITFUNCS_AVX2'
 *     - for a code block enabled for AVX2 or higher CPU
 *
 * Default jit compile has JITFUNCS=100,
 *
 * *WIP* \sa cpu_isa_t
 */
#define JITFUNCS_ANY 0
#define JITFUNCS_SSE42 1
#define JITFUNCS_AVX2 2
#define JITFUNCS_AVX512 3
//@}
typedef enum {
    isa_any,
    sse42,
    avx2,
    avx512_common,
    avx512_core,
    avx512_core_vnni,
    avx512_mic,
    avx512_mic_4ops,
} cpu_isa_t;

template <cpu_isa_t> struct cpu_isa_traits {}; /* ::vlen -> 32 (for avx2) */

template <> struct cpu_isa_traits<sse42> {
    static constexpr int vlen_shift = 4;
    static constexpr int vlen = 16;
    static constexpr int n_vregs = 16;
};
template <> struct cpu_isa_traits<avx2> {
    static constexpr int vlen_shift = 5;
    static constexpr int vlen = 32;
    static constexpr int n_vregs = 16;
};
template <> struct cpu_isa_traits<avx512_common> {
    static constexpr int vlen_shift = 6;
    static constexpr int vlen = 64;
    static constexpr int n_vregs = 32;
};
template <> struct cpu_isa_traits<avx512_core>:
    public cpu_isa_traits<avx512_common> {};

template <> struct cpu_isa_traits<avx512_mic>:
    public cpu_isa_traits<avx512_common> {};

template <> struct cpu_isa_traits<avx512_mic_4ops>:
    public cpu_isa_traits<avx512_common> {};

/* whatever is required to generate string literals... */
#include "z_magic.hpp"
#define JIT_IMPL_NAME_HELPER(prefix, isa, suffix_if_any) \
    (isa == sse42 ? prefix STRINGIFY(sse42) : \
    (isa == avx2 ? prefix STRINGIFY(avx2) : \
    (isa == avx512_common ? prefix STRINGIFY(avx512_common) : \
    (isa == avx512_core ? prefix STRINGIFY(avx512_core) : \
    (isa == avx512_mic ? prefix STRINGIFY(avx512_mic) : \
    (isa == avx512_mic_4ops ? prefix STRINGIFY(avx512_mic_4ops) : \
    prefix suffix_if_any))))))

#if defined(TARGET_VANILLA)
// should not include jit_generator.hpp (or any other jit stuff)
static inline constexpr bool mayiuse(const cpu_isa_t /*cpu_isa*/) {
    return true;
}
#endif

}
}
}
#endif // CPU_ISA_HPP
