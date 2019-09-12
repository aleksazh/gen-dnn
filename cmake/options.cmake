#===============================================================================
# Copyright 2018 Intel Corporation
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#===============================================================================

# Manage different library options
#===============================================================================

if(options_cmake_included)
    return()
endif()
set(options_cmake_included true)

# ========
# Features
# ========

option(MKLDNN_VERBOSE
    "allows Intel(R) MKL-DNN be verbose whenever MKLDNN_VERBOSE
    environment variable set to 1" ON) # enabled by default

option(MKLDNN_ENABLE_CONCURRENT_EXEC
    "disables sharing a common scratchpad between primitives.
    This option must be turned on if there is a possibility of concurrent
    execution of primitives that were created in the same thread.
    CAUTION: enabling this option increases memory consumption"
    OFF) # disabled by default

# =============================
# Building properties and scope
# =============================

set(MKLDNN_LIBRARY_TYPE "SHARED" CACHE STRING
    "specifies whether Intel(R) MKL-DNN library should be SHARED or STATIC")
option(MKLDNN_BUILD_EXAMPLES "builds examples"  ON)
option(MKLDNN_BUILD_TESTS "builds tests" ON)
option(MKLDNN_BUILD_FOR_CI "specifies whether Intel(R) MKL-DNN library should be built for CI" OFF)
option(MKLDNN_WERROR "treat warnings as errors" OFF)

set(MKLDNN_INSTALL_MODE "DEFAULT" CACHE STRING
    "specifies installation mode; supports DEFAULT or BUNDLE.

    When BUNDLE option is set MKL-DNN will be installed as a bundle
    which contains examples and benchdnn.")

####################################
# [ejk] support a TARGET_VANILLA **alternative** to src/cpu/ 
# The following did not work as expected:
#include(CMakeDependentOption)
#CMAKE_DEPENDENT_OPTION(TARGET_VANILLA "Use only pure C/C++ source_code, no JIT assembler" OFF
#    "NECVE OR NECSX" ON)
# Here's what I expected:
if(NECVE OR NECSX)
    set(TARGET_VANILLA ON) # no option (non-x86 cpu)
else()
    option(TARGET_VANILLA "Use only pure C/C++ source_code, no JIT assembler" OFF)
endif()
if(NECVE)
    set(VEJIT "0" CACHE STRING
        "Use libvednnx/jit for Aurora?")
else()
    set(VEJIT "0")
endif()
# NECVE: OpenMP support is rumored to be here, but experience suggests otherwise :0
#CMAKE_DEPENDENT_OPTION(USE_OPENMP "Use OpenMP?" ON
#                                        "NECVE" OFF)
#CMAKE_DEPENDENT_OPTION(USE_SHAREDLIB "Use shared libs?" ON   # libmkldnn.so
#                                                "NECVE" OFF) # libmkldnn.a
if(NECVE)
    option(USE_OPENMP "Use OpenMP (#pragma omp)?" OFF)
    option(USE_SHAREDLIB "Use shared libs?" OFF)   # libblas.a preferred
else()
    option(USE_OPENMP "Use OpenMP (#pragma omp)?" ON)
    option(USE_SHAREDLIB "Use shared libs?" ON)   # libmkldnn.so
endif()

if(USE_SHAREDLIB)
    set(MKLDNN_LIBRARY_TYPE "SHARED" CACHE STRING
        "specifies whether Intel(R) MKL-DNN library should be SHARED or STATIC")
else()
	set(MKLDNN_LIBRARY_TYPE "STATIC" CACHE STRING
        "specifies whether Intel(R) MKL-DNN library should be SHARED or STATIC")
endif()

# These options can be overridden from the cmake command line
if(TARGET_VANILLA)
    set(TARGET_VANILLA ON)
    set(TARGET_JIT OFF)
    set(TARGET_DEFS "-DTARGET_VANILLA ${TARGET_DEFS}")
else()
    set(TARGET_JIT ON)
    set(TARGET_VANILLA OFF)
endif()
if(TARGET_VANILLA)
    set(CPU_DIR vanilla)
else()
    set(CPU_DIR cpu)
endif()
if(NOT NECVE AND VEJIT)
    message(WARNING "VEJIT should only be set ECVE compilation")
endif()
MESSAGE(STATUS "NECVE: ${NECVE}    NECSX: ${NECSX}   TARGET_JIT: ${TARGET_JIT}")
message(STATUS "-DTARGET_VANILLA=${TARGET_VANILLA} -DUSE_OPENMP=${USE_OPENMP} -DUSE_SHAREDLIB=${USE_SHAREDLIB}")
#if(NECVE)
#    show_cmake_stuff("Initial setup completed ...")
#endif()
####################################

# =============
# Optimizations
# =============

set(MKLDNN_ARCH_OPT_FLAGS "HostOpts" CACHE STRING
    "specifies compiler optimization flags (see below for more information).
    If empty default optimization level would be applied which depends on the
    compiler being used.

    - For Intel(R) C++ Compilers the default option is `-xHOST` which instructs
      the compiler to generate the code for the architecture where building is
      happening. This option would not allow to run the library on older
      architectures.

    - For GNU* Compiler Collection version 5 and newer the default options are
      `-march=native -mtune=native` which behaves similarly to the description
      above.

    - For all other cases there are no special optimizations flags.

    If the library is to be built for generic architecture (e.g. built by a
    Linux distributive maintainer) one may want to specify MKLDNN_ARCH_OPT_FLAGS=\"\"
    to not use any host specific instructions")

# ======================
# Profiling capabilities
# ======================

option(MKLDNN_ENABLE_JIT_PROFILING
    "Enable registration of Intel(R) MKL-DNN kernels that are generated at
    runtime with Intel VTune Amplifier (on by default). Without the
    registrations, Intel VTune Amplifier would report data collected inside
    the kernels as `outside any known module`."
    ON)

# ===================
# Engine capabilities
# ===================

set(MKLDNN_CPU_RUNTIME "OMP" CACHE STRING
    "specifies the threading runtime for CPU engines;
    supports OMP (default) or TBB.

    To use Intel(R) Threading Building Blocks (Intel(R) TBB) one should also
    set TBBROOT (either environment variable or CMake option) to the library
    location.")

set(TBBROOT "" CACHE STRING
    "path to Intel(R) Thread Building Blocks (Intel(R) TBB).
    Use this option to specify Intel(R) TBB installation locaton.")

set(MKLDNN_GPU_RUNTIME "NONE" CACHE STRING
    "specifies the runtime to use for GPU engines.
    Can be NONE (default; no GPU engines) or OCL (OpenCL GPU engines).

    Using OpenCL for GPU requires setting OPENCLROOT if the libraries are
    installed in a non-standard location.")

set(OPENCLROOT "" CACHE STRING
    "path to Intel(R) SDK for OpenCL(TM).
    Use this option to specify custom location for OpenCL.")

# =============
# Miscellaneous
# =============

option(BENCHDNN_USE_RDPMC
    "enables rdpmc counter to report precise cpu frequency in benchdnn.
     CAUTION: may not work on all cpus (hence disabled by default)"
    OFF) # disabled by default

# =========================
# Developer and debug flags
# =========================

set(MKLDNN_USE_CLANG_SANITIZER "" CACHE STRING
    "instructs build system to use a Clang sanitizer. Possible values:
    Address: enables AddressSanitizer
    Memory: enables MemorySanitizer
    MemoryWithOrigin: enables MemorySanitizer with origin tracking
    Undefined: enables UndefinedBehaviourSanitizer
    This feature is experimental and is only available on Linux.")

option(_MKLDNN_USE_MKL "use BLAS functions from Intel MKL" OFF)

option(MKLDNN_USE_CBLAS
    "Use system cblas libraries.  This replaces many calls to mkl-dnns
    built-in gemm_driver with calls to cblas_sgemm."
    OFF)
# vim: sw=4 ts=4 et
