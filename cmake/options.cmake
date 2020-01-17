#===============================================================================
# Copyright 2018-2019 Intel Corporation
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

# ===========
# CPU and JIT
# ===========

#
# Useful constants
#
# DNNL_CPU is directly determined from CMAKE_SYSTEM_PROCESSOR, and is
# controlled by your cmake toolchain, or your CC/CXX environment.
# Actual values agree with dnnl_types.h
set(DNNL_CPU_X86 1)
set(DNNL_CPU_VE  2)
#
# JIT (or external libs) may be specified as cmake -DDNNL_ISA=<string>
# all cpus support <string> = VANILLA, ALL, and ANY
#
set(DNNL_ISA_VANILLA 1)
#set(DNNL_ISA_ANY     2)  # value changes below, once cpu is known
#set(DNNL_ISA_ALL     3)  # value changes below, once cpu is known
# x86-specific JIT -- FIXME jit_uni drivers might bypass this XXX
set(DNNL_ISA_X86                10)
set(DNNL_ISA_SSE41              11)
set(DNNL_ISA_AVX                12)
set(DNNL_ISA_AVX2               13)
set(DNNL_ISA_AVX512_MIC         14) # AVX_512_COMMON ?
set(DNNL_ISA_AVX512_MIC_4OPS    15)
set(DNNL_ISA_AVX512_CORE        16)
set(DNNL_ISA_AVX512_CORE_VNNI   17)
set(DNNL_ISA_AVX512_CORE_BF16   18)
set(DNNL_ISA_X86_ALL            18)
# ve-specific
set(DNNL_ISA_VE                 20)
set(DNNL_ISA_VEDNN              21)
set(DNNL_ISA_VEJIT              22)
set(DNNL_ISA_VE_ALL             22)

#
# Determine target CPU (governed by available compiler / environment CC)
#
if(${CMAKE_SYSTEM_PROCESSOR} STREQUAL "x86_64")
    set(DNNL_CPU ${DNNL_CPU_X86})
    set(DNNL_ISA_ANY ${DNNL_ISA_X86})
    set(DNNL_ISA_ALL ${DNNL_ISA_X86_ALL})
elseif(${CMAKE_SYSTEM_PROCESSOR} STREQUAL "aurora")
    set(DNNL_CPU ${DNNL_CPU_VE})
    set(DNNL_ISA_ANY ${DNNL_ISA_VE})
    set(DNNL_ISA_ALL ${DNNL_ISA_VE_ALL})
else()
    message(FATAL_ERROR "Unrecognized target CPU : ${CMAKE_SYSTEM_PROCESSOR}")
endif()

#
# You may specify additional CPU target info with cmake -DDNNL_ISA=<string>
# This is used for dnnl_config.h.in
#
if(DNNL_CPU EQUAL DNNL_CPU_X86)
    set(DNNL_ISA "ALL" CACHE STRING
        "x86 cpu build styles include VANILLA, ALL(x86 jit w/o vec ops),
        ANY(all x86 jit allowed), as well intermediate compile-time
        support for max isa SSE41 AVX AVX2 AVX512_MIC AVX512_MIC_4OPS
        AVX512_CORE AVX512_CORE_VNNI AVX512_CORE_BF16."
        ) # default to ALL bells and whistles
    if(NOT ${DNNL_ISA} MATCHES "^(VANILLA|ANY|SSE41|AVX|AVX2|AVX512_MIC|AVX512_MIC_4OPS|AVX512_CORE|AVX512_CORE_VNNI|AVX512_CORE_BF16|ALL)$")
        message(FATAL_ERROR "Unsupported ${CMAKE_SYSTEM_PROCESSOR} variant: DNNL_ISA=${DNNL_ISA}")
    endif()
else()
    set(DNNL_ISA "ALL" CACHE STRING
        "VE cpu build styles include VANILLA, ALL(same as vanilla),
        ANY(libvednn, jit...), as well intermediate compile-time
        support for max isa VEDNN and VEJIT."
        ) # default to ALL bells and whistles
    if(NOT ${DNNL_ISA} MATCHES "^(VANILLA|ANY|VEDNN|VEJIT|ALL)$")
        message(FATAL_ERROR "Unsupported ${CMAKE_SYSTEM_PROCESSOR} variant: DNNL_ISA=${DNNL_ISA}")
    endif()
endif()
set(DNNL_ISA_VALUE ${DNNL_ISA_${DNNL_ISA}})
message(STATUS "Target processor ${CMAKE_SYSTEM_PROCESSOR} has DNNL_CPU=${DNNL_CPU}, and maps")
message(STATUS "  DNNL_ISA=${DNNL_ISA} to DNNL_ISA_VALUE=DNNL_ISA_${DNNL_ISA}=${DNNL_ISA_VALUE}")
# DNNL_ISA is the cmake STRING mnemonic, and
# DNNL_ISA_VALUE is a numeric constant for dnnl_config.h.in

# ========
# Features
# ========

option(DNNL_VERBOSE
    "allows DNNL be verbose whenever DNNL_VERBOSE
    environment variable set to 1" ON) # enabled by default

option(DNNL_ENABLE_CONCURRENT_EXEC
    "disables sharing a common scratchpad between primitives.
    This option must be turned ON if there is a possibility of executing
    distinct primitives concurrently.
    CAUTION: enabling this option increases memory consumption."
    OFF) # disabled by default

option(DNNL_ENABLE_PRIMITIVE_CACHE "enables primitive cache.
    WARNING: the primitive cache is an experimental feature and might be
    changed without prior notification in future releases" OFF)
    # disabled by default

option(DNNL_ENABLE_MAX_CPU_ISA
    "enables control of CPU ISA detected by DNNL via DNNL_MAX_CPU_ISA
    environment variable and dnnl_set_max_cpu_isa() function" ON)

# =============================
# Building properties and scope
# =============================

set(DNNL_LIBRARY_TYPE "SHARED" CACHE STRING
    "specifies whether DNNL library should be SHARED or STATIC")
option(DNNL_BUILD_EXAMPLES "builds examples"  ON)
option(DNNL_BUILD_TESTS "builds tests" ON)
option(DNNL_BUILD_FOR_CI "specifies whether DNNL library should be built for CI" OFF)
option(DNNL_WERROR "treat warnings as errors" OFF)

set(DNNL_INSTALL_MODE "DEFAULT" CACHE STRING
    "specifies installation mode; supports DEFAULT or BUNDLE.

    When BUNDLE option is set DNNL will be installed as a bundle
    which contains examples and benchdnn.")

set(DNNL_CODE_COVERAGE "OFF" CACHE STRING
    "specifies which supported tool for code coverage will be used
    Currently only gcov supported")
if(NOT ${DNNL_CODE_COVERAGE} MATCHES "^(OFF|GCOV)$")
    message(FATAL_ERROR "Unsupported code coverage tool: ${DNNL_CODE_COVERAGE}")
endif()

# =============
# Optimizations
# =============

set(DNNL_ARCH_OPT_FLAGS "HostOpts" CACHE STRING
    "specifies compiler optimization flags (see below for more information).
    If empty default optimization level would be applied which depends on the
    compiler being used.

    - For Intel(R) C++ Compilers the default option is `-xSSE4.1` which instructs
      the compiler to generate the code for the processors that support SSE4.1
      instructions. This option would not allow to run the library on older
      architectures.

    - For GNU* Compiler Collection and Clang, the default option is `-msse4.1` which
      behaves similarly to the description above.

    - For all other cases there are no special optimizations flags.

    If the library is to be built for generic architecture (e.g. built by a
    Linux distributive maintainer) one may want to specify DNNL_ARCH_OPT_FLAGS=\"\"
    to not use any host specific instructions")

# ======================
# Profiling capabilities
# ======================

option(DNNL_ENABLE_JIT_PROFILING
    "Enable registration of DNNL kernels that are generated at
    runtime with Intel VTune Amplifier (on by default). Without the
    registrations, Intel VTune Amplifier would report data collected inside
    the kernels as `outside any known module`."
    ON)

# ===================
# Engine capabilities
# ===================

set(DNNL_CPU_RUNTIME "OMP" CACHE STRING
    "specifies the threading runtime for CPU engines;
    supports OMP (default) or TBB.

    To use Intel(R) Threading Building Blocks (Intel(R) TBB) one should also
    set TBBROOT (either environment variable or CMake option) to the library
    location.")
if(NOT ${DNNL_CPU_RUNTIME} MATCHES "^(OMP|TBB|SEQ)$")
    message(FATAL_ERROR "Unsupported CPU runtime: ${DNNL_CPU_RUNTIME}")
endif()

set(TBBROOT "" CACHE STRING
    "path to Intel(R) Thread Building Blocks (Intel(R) TBB).
    Use this option to specify Intel(R) TBB installation locaton.")

set(DNNL_GPU_RUNTIME "NONE" CACHE STRING
    "specifies the runtime to use for GPU engines.
    Can be NONE (default; no GPU engines) or OCL (OpenCL GPU engines).

    Using OpenCL for GPU requires setting OPENCLROOT if the libraries are
    installed in a non-standard location.")
if(NOT ${DNNL_GPU_RUNTIME} MATCHES "^(OCL|NONE)$")
    message(FATAL_ERROR "Unsupported GPU runtime: ${DNNL_GPU_RUNTIME}")
endif()

set(OPENCLROOT "" CACHE STRING
    "path to Intel(R) SDK for OpenCL(TM).
    Use this option to specify custom location for OpenCL.")

set(DNNL_CPU_EXTERNAL_GEMM "NONE" CACHE STRING
    "Use external GEMM (default NONE).  Supports MKL or CBLAS.  Typical use is
    to allow in2col-gemm convolutions when X86 jit is unavailable"
    )
if(NOT ${DNNL_CPU_EXTERNAL_GEMM} MATCHES "^(NONE|MKL|CBLAS)$")
    message(FATAL_ERROR "Unsupported DNNL_CPU_EXTERNAL_GEMM value")
endif()

# =============
# Miscellaneous
# =============

option(BENCHDNN_USE_RDPMC
    "enables rdpms counter to report precise cpu frequency in benchdnn.
     CAUTION: may not work on all cpus (hence disabled by default)"
    OFF) # disabled by default

# =========================
# Developer and debug flags
# =========================

set(DNNL_USE_CLANG_SANITIZER "" CACHE STRING
    "instructs build system to use a Clang sanitizer. Possible values:
    Address: enables AddressSanitizer
    Memory: enables MemorySanitizer
    MemoryWithOrigin: enables MemorySanitizer with origin tracking
    Undefined: enables UndefinedBehaviourSanitizer
    This feature is experimental and is only available on Linux.")

option(_DNNL_USE_MKL "use BLAS functions from Intel MKL" OFF)

# ==========================================
# Development work helpers (expert use only)
# ==========================================

# FIXME test jit with and without bfloat16 (or just remove this option)
# FIXME RNN ref impl !!!
set(DNNL_ENABLE_BFLOAT16 0) # please commit with value 1
set(DNNL_ENABLE_RNN 1)      # please commit with value 1
# for development work, we may disable big chunks of code
if(DNNL_CPU EQUAL DNNL_CPU_X86)
    if(DNNL_ISA STREQUAL "VANILLA") # note: DNNL_ISA is a "string"
        message(STATUS "x86 VANILLA compile removes rnn support (for now)")
        set(DNNL_ENABLE_RNN 0) # FIXME - there is some jit entanglement
    endif()
elseif(DNNL_CPU EQUAL DNNL_CPU_VE)
    message(STATUS "non-x86 compile wil disable BFLOAT16 and RNN support (for now)")
    set(DNNL_ENABLE_BFLOAT16 0) # initially 0 for porting
    set(DNNL_ENABLE_RNN 0)      # initially 0 for porting
else() # unknown? try enabling everything
endif()

if(0)
# ==============================================
# Some options depend on compiler/toolchain
# TARGET_VANILLA is a historical gen-dnn option
# JITFUNCS is also historical
# CPU_JIT is for the new dnnl-config.h.in
# ==============================================
if(NECVE)
    set(TARGET_VANILLA ON)
    set(JITFUNCS 7) # JITFUNCS_VE
    set(DNNL_CPU_JIT 29) # 2 ~ VE # deprecated
elseif(NECSX)
    set(DNNL_CPU "SX")
    set(TARGET_VANILLA ON)
    set(JITFUNCS 0)
    set(DNNL_CPU_JIT 30) # 3 ~ SX # deprecated
else() # target via compiler, probably x86
    # redo when build.sh is updated to use -DDNNL_CPU=ANY instead of TARGET_VANILLA
    OPTION(TARGET_VANILLA "Use only pure C/C++ source_code (no x86 jit)" OFF)
endif()
if(DNNL_CPU EQUAL 1 AND TARGET_VANILLA) # x86 vanilla
    set(DNNL_CPU_JIT -1) # cpu ANY and no x86 jit # deprecated
    set(JITFUNCS -1) # JITFUNCS_NONE
endif()
endif()
