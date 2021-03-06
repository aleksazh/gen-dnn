#===============================================================================
# Copyright 2017-2018 Intel Corporation
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

# main benchdnn files
#file(GLOB_RECURSE HEADERS
#    ${CMAKE_CURRENT_SOURCE_DIR}/../include/*.h
#    ${CMAKE_CURRENT_SOURCE_DIR}/../include/*.hpp
#    )
#message(STATUS "benchdnn HEADERS are ${HEADERS}") # empty!!
file(GLOB SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/*.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/*.h
    )
include_directories( ${CMAKE_CURRENT_SOURCE_DIR} )
# benchdnn subtests
foreach(subdir bnorm conv ip reorder rnn self)
    file(GLOB subfiles
        ${CMAKE_CURRENT_SOURCE_DIR}/${subdir}/*.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/${subdir}/*.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/${subdir}/*.c
        ${CMAKE_CURRENT_SOURCE_DIR}/${subdir}/*.h
        )
    list(APPEND SOURCES ${subfiles})
    message(STATUS "${subdir} : ${subfiles}")
    include_directories(${CMAKE_CURRENT_SOURCE_DIR}/${subdir})
endforeach()

if(BENCHDNN_USE_RDPMC)
    add_definitions(-DBENCHDNN_USE_RDPMC)
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "Intel")
    if(WIN32)
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Qprec-div")
    elseif(UNIX OR APPLE)
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -prec-div")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fp-model precise")
    endif()
endif()

add_executable(benchdnn ${SOURCES})
if(WIN32)
    configure_file(${CMAKE_SOURCE_DIR}/config_template.vcxproj.user
        benchdnn.vcxproj.user @ONLY)
    set_property(TARGET benchdnn PROPERTY ENVIRONMENT "PATH=${CTESTCONFIG_PATH}")
endif()

set_property(TARGET benchdnn PROPERTY CXX_STANDARD 11)
# [ejk] ease switching between #ifdef'ed code blocks with unused vars/fns
if(NECSX)
    set(BENCHDNN_EXTRA "${BENCHDNN_EXTRA} -pvctl,fullmsg")
elseif(NECVE)
    set(BENCHDNN_EXTRA "${BENCHDNN_EXTRA} -Wno-error")
else()
    if(UNIX AND NOT APPLE AND "${CMAKE_C_COMPILER_ID}" STREQUAL "GNU")
        # gcc-X optimization reporting ...
        # -finfo-vec- ... many things  (missed, all, note, vec, omp, inl, ...)
        #set(BENCHDNN_EXTRA "${BENCHDNN_EXTRA} -ftree-vectorizer-verbose=7 -fopt-info-all-loop-vec")
        #if(CMAKE_C_COMPILER_VERSION VERSION_GREATER "5.9.9")
        #    set(BENCHDNN_EXTRA "${BENCHDNN_EXTRA} -fopt-info-all-omp")
        #endif()
    endif()
endif()
set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS} ${BENCHDNN_EXTRA}")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${BENCHDNN_EXTRA}")

# by hand:
#          ( cd build/tests/benchdnn && make conv/sx_conv4.cpp.s )
#          ( cd build/tests/benchdnn && make conv/sx_conv4.cpp.i )
# Just for fun, take a look at assembler output for any conv/*.cxx files.
# These outputs are left in the cmake binary directory ~ build/tests/benchdnn.
file(GLOB ASM_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/conv/*.cxx ${CMAKE_CURRENT_SOURCE_DIR}/conv/impls/*.cpp)
#list(APPEND ASM_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/conv/sx_conv3.cpp)
#list(APPEND ASM_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/conv/sx_conv3_ve.cpp)
#list(APPEND ASM_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/conv/sx_conv2.cpp)
#list(APPEND ASM_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/conv/ref_conv3.cpp)
# hack: get the .o and .s **rules** into Makefiles, but skip the link step
add_library(unused STATIC ${ASM_SOURCES})
set_target_properties(unused PROPERTIES
    EXCLUDE_FROM_ALL 1)
#    # does not actually avoid the linking.
#    # actually I want to skip producing the .o files too
#    set_target_properties(unused PROPERTIES RULE_LAUNCH_LINK "${asm_ECHO}")
#message(STATUS "benchdnn ASM_SOURCES = ${ASM_SOURCES}")
string(REGEX REPLACE "${CMAKE_CURRENT_SOURCE_DIR}/conv/(.*)" "conv/\\1.s"
    ASM_TARGETS ${ASM_SOURCES} )
message(STATUS "benchdnn ASM_TARGETS = ${ASM_TARGETS}")
# --- moved .s rule modifications into cmake/asm.cmake

#set_property(TARGET benchdnn PROPERTY C_STANDARD 11)
target_link_libraries(benchdnn ${LIB_NAME} ${EXTRA_LIBS})
if(NECSX) # dirname and strnlen extensions in a special library:
    find_library(LIBGEN libgen) # NEW [ejk] probably need to tweak paths!!!
    message(STATUS, "libgen ?? ${LIBGEN} ...")
    if(LIBGEN)
        target_link_libraries(benchdnn ${LIBGEN})
    else()
        target_link_libraries(benchdnn -lgen)
    endif()
    # Let me try the approach Ishizaka-san uses for milde compilation:
    set_target_properties(benchdnn PROPERTIES
        LINK_FLAGS -Wl,-h,muldefs,-h,lib_cyclic)
elseif(NECVE)
    message(STATUS "Aurora benchdnn : not linking with any extra libs")
elseif(UNIX AND NOT APPLE)
    error_if_SX()
    find_library(LIBRT rt)
    if(LIBRT)
        target_link_libraries(benchdnn ${LIBRT})
    endif()
endif()
# add_test(benchdnn benchdnn)

file(COPY inputs DESTINATION .)
add_custom_target(test_conv
    COMMAND benchdnn -v1 --conv --batch=inputs/test_conv_all
    DEPENDS benchdnn
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)
if(WIN32)
    configure_file(${CMAKE_SOURCE_DIR}/config_template.vcxproj.user
        test_conv.vcxproj.user @ONLY)
    set_property(TARGET test_conv PROPERTY ENVIRONMENT "PATH=${CTESTCONFIG_PATH}")
endif()

add_custom_target(test_benchdnn_conv
    COMMAND benchdnn -v1 --conv --batch=inputs/test_conv_all
    DEPENDS benchdnn
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)
if(WIN32)
    configure_file(${CMAKE_SOURCE_DIR}/config_template.vcxproj.user
        test_benchdnn_conv.vcxproj.user @ONLY)
    set_property(TARGET test_benchdnn_conv PROPERTY ENVIRONMENT "PATH=${CTESTCONFIG_PATH}")
endif()

add_custom_target(test_benchdnn_deconv
    COMMAND benchdnn -v1 --deconv --batch=inputs/test_deconv_all
    DEPENDS benchdnn
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)
if(WIN32)
    configure_file(${CMAKE_SOURCE_DIR}/config_template.vcxproj.user
        test_benchdnn_deconv.vcxproj.user @ONLY)
    set_property(TARGET test_benchdnn_deconv PROPERTY ENVIRONMENT "PATH=${CTESTCONFIG_PATH}")
endif()

add_custom_target(test_benchdnn_rnn
    COMMAND benchdnn -v1 --rnn
    DEPENDS benchdnn
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)
if(WIN32)
    configure_file(${CMAKE_SOURCE_DIR}/config_template.vcxproj.user
        test_benchdnn_rnn.vcxproj.user @ONLY)
    set_property(TARGET test_benchdnn_rnn PROPERTY ENVIRONMENT "PATH=${CTESTCONFIG_PATH}")
endif()

add_custom_target(test_benchdnn_reorder
    COMMAND benchdnn --reorder --batch=inputs/reorder/test_default
    DEPENDS benchdnn
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)
if(WIN32)
    configure_file(${CMAKE_SOURCE_DIR}/config_template.vcxproj.user
        test_benchdnn_reorder.vcxproj.user @ONLY)
    set_property(TARGET test_benchdnn_reorder PROPERTY ENVIRONMENT "PATH=${CTESTCONFIG_PATH}")
endif()

add_custom_target(test_benchdnn_bnorm
    COMMAND benchdnn --bnorm --batch=inputs/bnorm/test_bnorm_topo
    DEPENDS benchdnn
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

if(WIN32)
    configure_file(${CMAKE_SOURCE_DIR}/config_template.vcxproj.user
        test_benchdnn_bnorm.vcxproj.user @ONLY)
    set_property(TARGET test_benchdnn_bnorm PROPERTY ENVIRONMENT "PATH=${CTESTCONFIG_PATH}")
endif()

add_custom_target(test_benchdnn_inner_product
    COMMAND benchdnn --ip --batch=inputs/ip/test_ip_all
    DEPENDS benchdnn
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

if(WIN32)
    configure_file(${CMAKE_SOURCE_DIR}/config_template.vcxproj.user
        test_benchdnn_inner_product.vcxproj.user @ONLY)
    set_property(TARGET test_benchdnn_inner_product PROPERTY ENVIRONMENT "PATH=${CTESTCONFIG_PATH}")
endif()

add_custom_target(test_benchdnn_regression
    COMMAND benchdnn --conv --batch=inputs/test_conv_regression
    COMMAND benchdnn --bnorm --batch=inputs/bnorm/test_bnorm_regressions
    DEPENDS benchdnn
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)
if(WIN32)
    configure_file(${CMAKE_SOURCE_DIR}/config_template.vcxproj.user
        test_benchdnn_regression.vcxproj.user @ONLY)
    set_property(TARGET test_benchdnn_regression PROPERTY ENVIRONMENT "PATH=${CTESTCONFIG_PATH}")
endif()
# vim: et ts=4 sw=4 ai cino=^=l0,\:0,N-s
