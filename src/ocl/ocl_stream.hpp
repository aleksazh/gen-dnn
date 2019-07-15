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

#ifndef OCL_STREAM_HPP
#define OCL_STREAM_HPP

#include "common/c_types_map.hpp"
#include "common/utils.hpp"
#include "ocl/cl_stream.hpp"
#include "ocl/ocl_engine.hpp"
#include "ocl/ocl_utils.hpp"

namespace mkldnn {
namespace impl {
namespace ocl {

struct ocl_stream_t : public cl_stream_t {
    static status_t create_stream(
            stream_t **stream, engine_t *engine, unsigned generic_flags) {

        unsigned flags;
        status_t status = ocl_stream_t::init_flags(&flags, generic_flags);
        if (status != status::success)
            return status;

        auto *ocl_stream = new ocl_stream_t(engine, flags);
        if (!ocl_stream)
            return status::out_of_memory;

        status = ocl_stream->init();
        if (status != status::success)
            return status;

        *stream = ocl_stream;
        return status::success;
    }

    static status_t create_stream(
            stream_t **stream, engine_t *engine, cl_command_queue queue) {
        unsigned flags;
        status_t status = ocl_stream_t::init_flags(&flags, queue);
        if (status != status::success)
            return status;

        auto *ocl_stream = new ocl_stream_t(engine, flags, queue);
        if (!ocl_stream)
            return status::out_of_memory;

        status = ocl_stream->init();
        if (status != status::success)
            return status;

        *stream = ocl_stream;
        return status::success;
    }

    virtual status_t wait() override {
        OCL_CHECK(clFinish(queue_));
        return status::success;
    }

    cl_command_queue queue() const { return queue_; }

private:
    ocl_stream_t(engine_t *engine, unsigned flags)
        : cl_stream_t(engine, flags), queue_(nullptr) {}
    ocl_stream_t(engine_t *engine, unsigned flags, cl_command_queue queue)
        : cl_stream_t(engine, flags), queue_(queue) {}
    ~ocl_stream_t() {
        wait();
        if (queue_) {
            clReleaseCommandQueue(queue_);
        }
    }

    status_t init();

    static status_t init_flags(unsigned *flags, unsigned generic_flags) {
        *flags = 0;
        if (generic_flags & stream_flags::default_order)
            *flags |= stream_flags::in_order;
        return status::success;
    }

    static status_t init_flags(unsigned *flags, cl_command_queue queue) {
        // Determine if the passed queue is in-order/out-of-order
        cl_command_queue_properties props;
        OCL_CHECK(clGetCommandQueueInfo(queue, CL_QUEUE_PROPERTIES,
                sizeof(cl_command_queue_properties), &props, nullptr));

        *flags = 0;
        *flags |= (props & CL_QUEUE_OUT_OF_ORDER_EXEC_MODE_ENABLE)
                ? stream_flags::out_of_order
                : stream_flags::in_order;

        return status::success;
    }

private:
    cl_command_queue queue_;
};

} // namespace ocl
} // namespace impl
} // namespace mkldnn

#endif
