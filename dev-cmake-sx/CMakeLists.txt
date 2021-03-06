#
# reduce requirements so that you can also run on gcc linux host
#
cmake_minimum_required(VERSION 2.8.0 FATAL_ERROR)
#
# cmake-3.0 can be made to work, but cmake-3.8 pulls in a
# Platform/SX-Initialize file that is just too convenient
#    (not sure about actual minimum cmake version)
# cmake-3.8.0 built from sources without much difficulty
# my copy is in /gfs/nlabhpg/cmake3.8/
# and is available for me after bash:$ source ~/bashrc-local
#                                   or source /mnt/diamond2/home/4gi/nlabhpg/bashrc-local
# (sources in /fgs/nlabhpg/cmake-3.8.0/

project(cmake-sample C CXX)
# cmake-3.8:  patches no longer required
#if(NECSX)
#    fix_bad_gnu_things()
#    sx_local_settings()
#    if(SX_TRACE)
#        message(STATUS "after fix_bad_gnu_things, CMAKE_C_FLAGS_RELWITHDEBINFO is ${CMAKE_C_FLAGS_RELWITHDEBINFO}")
#    endif()
#endif()

if(NECSX)
    # add to (or override) your flags... example:
    #   for any environment :
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Kcpp11,gcc -v -f90lib")
    # NOTE: -size_t64 must **also** be given to linker:
    #set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-size_t64")
    #         ^^^ or SHARED or MODULE
    # or for just foo, set_target_properties(foo PROPERTIES LINK_FLAGS -Wl,-size_t64)

    # NOTE: without -f90lib, you might be able to guess that -lf2003 or something was required

    #  for Release mode only :
    #    (You can set other "standard" strings in Platform/SX-Initialize)
    #    - make rel
    #    - or cmake -DCMAKE_BUILD_TYPE=Release ...
    #    will now include a whole pile of options
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} ${SX_CCXXFLAGS_WARNALL} ${SX_CCXXFLAGS_VERBOSE}")
    set(CBLAS_LIBS "-lcblas -lblas") # -lf2003 removed, because I added -f90lib to CMAKE_CXX_FLAGS
else(NECSX)
    set(CMAKE_CXX_FLAGS "-march=native")
    set(CBLAS_LIBS "-lblas") # good enough for gcc
endif(NECSX)

add_executable(foo foo.cc)

# This prints out the sizes of various basic C++ types
# It requires no link libraries,
#    but -size_t64 compilation also needs -size_t64 for LINK_FLAGS !
add_executable(types types.cpp)
add_executable(types32 types.cpp)
add_executable(types64 types.cpp)
if(NECSX)
    set_target_properties(types32 PROPERTIES COMPILE_FLAGS -size_t32)
    set_target_properties(types64 PROPERTIES COMPILE_FLAGS -size_t64 LINK_FLAGS -size_t64)
endif()
# I find types is the same as types32, so -size_t32 seems to be the default setting

# Using blas libs, we can compile and link with size_t32 as default
add_executable(speed32 speed.cpp)
target_link_libraries(speed32 "${CBLAS_LIBS}")
if(NECSX)
    set_target_properties(speed32 PROPERTIES COMPILE_FLAGS -size_t32)
endif()

# size_t64 wants
add_executable(speed64 speed.cpp)
target_link_libraries(speed64 -size_t64 "${CBLAS_LIBS}")
if(NECSX)
    set_target_properties(speed64 PROPERTIES COMPILE_FLAGS -size_t64)
endif()

add_library(spd32 libspeed.cpp libspeed.hpp) # create libspd32.a
add_library(spd64 libspeed.cpp libspeed.hpp) # create libspd64.a
if(NECSX)
    set_target_properties(spd32 PROPERTIES COMPILE_FLAGS "-Tauto,all -size_t32")
    set_target_properties(spd64 PROPERTIES COMPILE_FLAGS "-Tauto,all -size_t64")
endif(NECSX)
list(APPEND spd_LINKER_LIBS "${CBLAS_LIBS}")

add_executable(speed2 speed2.cpp) # WITHOUT -size_tNN, it make 32-bit?
add_dependencies(speed2 spd32)
#add_dependencies(speed2 spd64)
if(NECSX)
    #target_link_libraries(speed2 "-size_t64 -Wl,-h,muldefs,-h,lib_cyclic -L. libspd64.a -lcblas -lblas -lf2003")
    target_link_libraries(speed2 "-size_t32 -Wl,-h,muldefs,-h,lib_cyclic -L. libspd32.a -lcblas -lblas -lf2003")
    # XXX also show minimal requirement
else(NECSX)
    target_link_libraries(speed2 "-L. libspd64.a ${CBLAS_LIBS}") # cmake recognizes that other libs are also required ???
endif(NECSX)
# vim: set ts=4 sw=4 et:
