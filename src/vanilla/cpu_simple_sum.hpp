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

#ifndef CPU_SIMPLE_SUM_HPP
#define CPU_SIMPLE_SUM_HPP

#include <assert.h>

#include "c_types_map.hpp"
#include "type_helpers.hpp"
#include "utils.hpp"

#include "cpu_memory.hpp"
#include "cpu_primitive.hpp"
#include "mkldnn_thread.hpp"

#include <stdio.h>

namespace mkldnn {
namespace impl {
namespace cpu {

template <impl::data_type_t data_type>
struct cpu_simple_sum_t: public c_compatible {
    typedef typename prec_traits<data_type>::type data_t;
    enum { max_num_arrs = 16 };

    static bool applicable(const nstl::vector<cpu_memory_t::pd_t> &src_pds_,
                            const nstl::vector<double> &scale_,
                            cpu_memory_t::pd_t &dst_pd_)
    {
#if 1 || ! defined(_SX)
        const memory_desc_wrapper o_d(&dst_pd_);
#else // sxc++ compiler bug workaround
        const memory_desc_wrapper o_d(dst_pd_.desc()); // OK for SX
#endif

        assert( o_d._md != nullptr );
        bool ok = true && src_pds_.size() <= max_num_arrs;
        for (size_t i = 0; i < src_pds_.size(); ++i) {
#if 1 || ! defined(_SX)
            const memory_desc_wrapper i_d(&src_pds_[i]);
#else
            const memory_desc_wrapper i_d(src_pds_[i].desc()); // OK for SX
#endif
            assert( i_d._md != nullptr );
            ok = ok && i_d.data_type() == data_type
                && o_d.data_type() == data_type && i_d.format() == o_d.format()
                && i_d.is_dense() && o_d.is_dense();
        }
        printf(" ok=%d", (int)ok); fflush(stdout);
        return ok;
    }

    static void execute(const nstl::vector<cpu_memory_t::pd_t> &src_pds_,
                        const nstl::vector<double> &scale_,
                        cpu_memory_t::pd_t &dst_pd_,
                        cpu_primitive_t *sum)
    {
        const int num_arrs = int(src_pds_.size());

        auto output = reinterpret_cast<data_t *>(sum->memory());
#if 1 || ! defined(_SX)
        const memory_desc_wrapper o_d(&dst_pd_);     // OK, but FAIL for SX
#else
        const memory_desc_wrapper o_d(dst_pd_.desc()); // for SX
#endif
        output += o_d.blk_off(0);
        const size_t nelems = o_d.nelems();
        const data_t *input_ptrs[max_num_arrs];

        for (int a = 0; a < num_arrs; ++a) {
#if 1 || ! defined(_SX)
            const memory_desc_wrapper i_d(&src_pds_[a]); // OK, but FAIL for SX
#else
            const memory_desc_wrapper i_d(src_pds_[a].desc()); // for SX
#endif

            input_ptrs[a] = reinterpret_cast<const data_t *>(
                    sum->input_memory(a)) + i_d.blk_off(0);
        }

        unsigned block_size =  int(16 * 1024/sizeof(data_type));
        const size_t blocks_number = nelems / block_size;
        const size_t tail = nelems % block_size;

#pragma omp parallel
        {
            const int ithr = omp_get_thread_num();
            const int nthr = omp_get_num_threads();
            size_t start{0}, end{0};
            balance211(blocks_number, nthr, ithr, start, end);

            for (size_t nb = start; nb < end; ++nb) {
                size_t start_e = nb * block_size;
                size_t end_e = start_e + block_size;
                for (size_t e = start_e; e < end_e; e++) {
                    output[e] = static_cast<data_t>(scale_[0] * input_ptrs[0][e]);
                }
                for (int a = 1; a < num_arrs; a++) {
                    for (size_t e = start_e; e < end_e; e++) {
                        output[e] += static_cast<data_t>(scale_[a] * input_ptrs[a][e]);
                    }
                }
            }

            if (tail != 0 && ithr == nthr - 1) {
                size_t start_e = nelems - tail;
                size_t end_e = nelems;
                for (size_t e = start_e; e < end_e; e++) {
                    output[e] = static_cast<data_t>(scale_[0] * input_ptrs[0][e]);
                }
                for (int a = 1; a < num_arrs; a++) {
                    for (size_t e = start_e; e < end_e; e++) {
                        output[e] += static_cast<data_t>(scale_[a] * input_ptrs[a][e]);
                    }
                }
            }
        }
    }
};

}
}
}

#endif

// vim: et ts=4 sw=4 cindent cino^=l0,\:0,N-s
