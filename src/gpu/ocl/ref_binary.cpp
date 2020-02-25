/*******************************************************************************
* Copyright 2019-2020 Intel Corporation
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

#include "gpu/ocl/ref_binary.hpp"

namespace dnnl {
namespace impl {
namespace gpu {
namespace ocl {

status_t ref_binary_t::pd_t::init_conf() {
    const memory_desc_wrapper src0_d(src_md(0));
    const memory_desc_wrapper src1_d(src_md(1));
    const memory_desc_wrapper dst_d(dst_md());

    alg_kind_t alg = desc()->alg_kind;

    const auto &po = attr()->post_ops_;
    bool with_sum = po.contain(primitive_kind::sum, 0)
            && po.entry_[0].sum.scale != 0.f;
    float sum_scale = with_sum ? po.entry_[0].sum.scale : 1;
    int e_idx = po.find(primitive_kind::eltwise);
    bool with_eltwise = e_idx != -1 ? true : false;

    const int ndims = src0_d.ndims();
    conf.src0_md_info = memory_desc_info_t::create(src0_d);
    conf.src1_md_info = memory_desc_info_t::create(src1_d);
    conf.dst_md_info = memory_desc_info_t::create(dst_d);
    conf.data_type = src0_d.data_type();
    conf.ndims = ndims;
    for (int i = 0; i < MAX_NDIMS; ++i) {
        conf.bcast_dims[i] = i < ndims ? broadcast_dims()[i] : 1;
    }
    conf.is_add = (alg == alg_kind::binary_add);
    conf.is_mul = (alg == alg_kind::binary_mul);
    conf.is_max = (alg == alg_kind::binary_max);
    conf.is_min = (alg == alg_kind::binary_min);
    conf.is_tensor_op = is_tensor_op();
    conf.is_dense = dst_d.is_dense();
    conf.is_same_md = (src0_d == dst_d) && (src1_d == dst_d);

    conf.with_eltwise = with_eltwise;
    conf.with_sum = with_sum;
    conf.sum_scale = sum_scale;
    if (with_eltwise) { conf.eltwise = po.entry_[e_idx].eltwise; }

    auto *compute_engine
            = utils::downcast<compute::compute_engine_t *>(engine());
    conf.dispatch = compute_engine->create_dispatch(dst_d.md_);
    if (conf.is_tensor_op && conf.is_dense && conf.is_same_md) {
        conf.dispatch.define_dim("IDX", 0, dst_d.nelems());
    } else {
        for (int i = 0; i < MAX_NDIMS; ++i) {
            conf.dispatch.define_dim(utils::format("D%d", i),
                    nstl::min(i, ndims - 1), i < ndims ? dst_d.dims()[i] : 1);
        }
    }

    conf.dispatch.generate();

    return status::success;
}

status_t ref_binary_t::pd_t::init_kernel_ctx(
        compute::kernel_ctx_t &kernel_ctx) const {
    kernel_ctx.set_data_type(conf.data_type);
    kernel_ctx.define_int("NDIMS", conf.ndims);
    kernel_ctx.define_int("IS_MUL", conf.is_mul);
    kernel_ctx.define_int("IS_ADD", conf.is_add);
    kernel_ctx.define_int("IS_MAX", conf.is_max);
    kernel_ctx.define_int("IS_MIN", conf.is_min);
    kernel_ctx.define_int("IS_TENSOR_OP", conf.is_tensor_op);
    kernel_ctx.define_int("IS_DENSE", conf.is_dense);
    kernel_ctx.define_int("IS_SAME_MD", conf.is_same_md);
    kernel_ctx.define_int("BCAST_DIM0", conf.bcast_dims[0]);
    kernel_ctx.define_int("BCAST_DIM1", conf.bcast_dims[1]);
    kernel_ctx.define_int("BCAST_DIM2", conf.bcast_dims[2]);
    kernel_ctx.define_int("BCAST_DIM3", conf.bcast_dims[3]);
    kernel_ctx.define_int("BCAST_DIM4", conf.bcast_dims[4]);
    kernel_ctx.define_int("BCAST_DIM5", conf.bcast_dims[5]);

    kernel_ctx.define_int("WITH_ELTWISE", conf.with_eltwise);
    kernel_ctx.define_int("WITH_SUM", conf.with_sum);
    kernel_ctx.define_int("SUM_SCALE", conf.sum_scale == 1);

    def_memory_desc_info(kernel_ctx, conf.src0_md_info, "SRC0");
    def_memory_desc_info(kernel_ctx, conf.src1_md_info, "SRC1");
    def_memory_desc_info(kernel_ctx, conf.dst_md_info, "DST");

    if (conf.with_eltwise) { def_postops(kernel_ctx, conf.eltwise.alg); }

    def_dispatch(kernel_ctx, conf.dispatch);

    return status::success;
}

status_t ref_binary_t::execute_ref(const exec_ctx_t &ctx) const {
    compute::compute_stream_t *compute_stream
            = utils::downcast<compute::compute_stream_t *>(ctx.stream());

    auto &src0 = CTX_IN_STORAGE(DNNL_ARG_SRC_0);
    auto &src1 = CTX_IN_STORAGE(DNNL_ARG_SRC_1);
    auto &dst = CTX_OUT_STORAGE(DNNL_ARG_DST);

    auto eltwise_alpha = pd()->eltwise_alpha();
    auto eltwise_beta = pd()->eltwise_beta();
    auto sum_scale = pd()->sum_scale();

    compute::kernel_arg_list_t arg_list;
    arg_list.set(0, src0);
    arg_list.set(1, src1);
    arg_list.set(2, dst);
    arg_list.set(3, eltwise_alpha);
    arg_list.set(4, eltwise_beta);
    arg_list.set(5, sum_scale);

    const auto &conf = pd()->conf;

    auto nd_range = conf.dispatch.nd_range();
    status_t status = compute_stream->parallel_for(nd_range, kernel_, arg_list);
    return status;
}

} // namespace ocl
} // namespace gpu
} // namespace impl
} // namespace dnnl

// vim: et ts=4 sw=4 cindent cino+=l0,\:4,N-s
