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

#if !(defined(TARGET_VANILLA) || (defined(JITFUNCS) && JITFUNCS<0))
#include "mkldnn_test_common.hpp"
#include "gtest/gtest.h"

#include "mkldnn.h"
#include "test_gemm_common.hpp"

namespace mkldnn {

using gemm_test = gemm_test_common<bfloat16_t, bfloat16_t, float>;

TEST_P(gemm_test, TestGEMM)
{}

#define TEST_CASE_NAME_PREFIX bf16bf16f32
#define BF16BF16F32
#include "gemm_in.h"
}
// vim: et ts=4 sw=4 cindent cino=^l0,\:0,N-s
#endif // !TARGET_VANILLA
