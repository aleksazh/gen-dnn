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

#ifndef MKLDNN_TRAITS_HPP
#define MKLDNN_TRAITS_HPP

#include <assert.h>
#include <stdint.h>

#include "mkldnn.h"
#include "c_types_map.hpp"
#include "nstl.hpp"
#include "utils.hpp"
#include "z_magic.hpp"

namespace mkldnn {
namespace impl {

template <data_type_t> struct prec_traits {}; /* ::type -> float */
template <typename> struct data_traits {}; /* ::data_type -> f32 */
template <primitive_kind_t> struct pkind_traits {}; /* ::desc_type, ::query_d */
//template <data_type_t> struct accum_traits {}; /* --> float_t and accsqr_t */


template <> struct prec_traits<data_type::f32> { typedef float type; };
template <> struct prec_traits<data_type::s32> { typedef int32_t type; };
template <> struct prec_traits<data_type::s16> { typedef int16_t type; };
template <> struct prec_traits<data_type::s8> { typedef int8_t type; };
template <> struct prec_traits<data_type::u8> { typedef uint8_t type; };

template <> struct data_traits<float>
{ static constexpr data_type_t data_type = data_type::f32; };
template <> struct data_traits<int32_t>
{ static constexpr data_type_t data_type = data_type::s32; };
template <> struct data_traits<int16_t>
{ static constexpr data_type_t data_type = data_type::s16; };
template <> struct data_traits<int8_t>
{ static constexpr data_type_t data_type = data_type::s8; };
template <> struct data_traits<uint8_t>
{ static constexpr data_type_t data_type = data_type::u8; };

#if 0
/** float_t for calcs like pow, accsqr_t for summing squares.
 *  traits-like in case you want to go to double someday. */
template <> struct accum_traits<data_type::f32>
{ typedef float float_t; typedef float accsqr_t; };
template <> struct accum_traits<data_type::s32>
{ typedef float float_t; typedef float accsqr_t; };
template <> struct accum_traits<data_type::s16>
{ typedef float float_t; typedef float/*int64_t?*/ accsqr_t; };
template <> struct accum_traits<data_type::s8>
{ typedef float float_t; typedef uint32_t accsqr_t; };
template <> struct accum_traits<data_type::u8>
{ typedef float float_t; typedef uint32_t accsqr_t; };
#endif

#define PKIND_TRAITS_INST(op) \
template <> struct pkind_traits<primitive_kind::op> { \
    typedef CONCAT2(op, _desc_t) desc_type; \
    static constexpr query_t query_d = query::CONCAT2(op, _d); \
}
PKIND_TRAITS_INST(memory);
PKIND_TRAITS_INST(convolution);
PKIND_TRAITS_INST(eltwise);
PKIND_TRAITS_INST(softmax);
PKIND_TRAITS_INST(pooling);
PKIND_TRAITS_INST(lrn);
PKIND_TRAITS_INST(batch_normalization);
PKIND_TRAITS_INST(inner_product);
PKIND_TRAITS_INST(convolution_relu);
#undef PKIND_TRAITS_INST

}
}

#endif

// vim: et ts=4 sw=4 cindent cino^=l0,\:0,N-s
