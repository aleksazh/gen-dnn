#!/bin/bash
# vim: et ts=4 sw=4 ai
ORIGINAL_CMD="$0 $*"
DOTEST=0
DODEBUG="n"
DODOC="y"
DONEEDMKL="y"
DOJUSTDOC="n"
DOWARN="y"
BUILDOK="y"
SIZE_T=32 # or 64, for -s or -S SX compile
JOBS="-j8"
CMAKETRACE=""
USE_CBLAS=1
QUICK=0
DOGCC_VER=0
NEC_FTRACE=0
if [ "${CC##sx}" == "sx" -o "${CXX##sx}" == "sx" ]; then
    DOTARGET="s" # s for SX (C/C++ code, cross-compile)
elif [ "${CC}" == "ncc" -a "${CXX}" == "nc++" ]; then
    echo "auto-detected '-a' Aurora compiler (ncc, nc++)"
    DOTARGET="a"; DOJIT=0; SIZE_T=64; DONEEDMKL="n"
    if [ `uname -n` = "zoro" ]; then JOBS="-j8"; else JOBS="-j1"; fi
elif [ -d src/vanilla ]; then
    DOTARGET="v" # v for vanilla (C/C++ code)
else
    DOTARGET="j" # j for JIT (Intel assembler)
fi
usage() {
    echo "$0 usage:"
    #head -n 30 "$0" | grep "^[^#]*.)\ #"
    awk '/getopts/{flag=1;next} /^done/{flag=0} flag&&/^[^#]+) #/; flag&&/^ *# /' $0
    echo "Example: time a full test run for a debug compilation --- time $0 -dtt"
    echo "         SX debug compile, quick (no doxygen)         --- time $0 -Sdq"
    echo "         *just* run cmake, for SX debug compile       ---      $0 -SdQ"
    echo "         *just* create doxygen docs                   ---      $0 -D"
    echo "Aurora : quick Aurora Ftrace Trace-Cmake)  --- $0 -qaFT"
    echo "         quick, quick: no cmake, just make --- $0 -qqa"
    echo "     see what cmake is doing, and create build.log"
    echo "         CMAKEOPT='--trace -LAH' ./build.sh -q >&/dev/null"
    echo "Debug: Individual tests can be run like build-sx/tests/gtests/test_relu"
    echo "  We look at CC and CXX to try to guess -S or -a (SX or Aurora)"
    exit 0
}
while getopts ":hatvjdDqQpsSTwWbF1567" arg; do
    #echo "arg = ${arg}, OPTIND = ${OPTIND}, OPTARG=${OPTARG}"
    case $arg in
        a) # NEC Aurora VE
            DOTARGET="a"; DOJIT=0; SIZE_T=64; DONEEDMKL="n"
            JOBS="-j1" # -j1 to avoid SIGSEGV in ccom
            if [ `uname -n` = "zoro" ]; then JOBS="-j8"; fi
            #export LDFLAGS="${LDFLAGS} -L/opt/nec/ve/musl/lib"
            ;;
        F) # NEC Aurora VE or SX : add ftrace support (generate ftrace.out)
            NEC_FTRACE=1
            ;;
        t) # [0] increment test level: (1) examples, (2) tests (longer), ...
            # Apr-14-2017 build timings:
            # 0   : build    ~ ?? min  (jit), 1     min  (vanilla)
            # >=1 : examples ~  1 min  (jit), 13-16 mins (vanilla)
            # >=2 : test_*   ~ 10 mins (jit), 108   mins (vanilla)
            # >=3 : benchdnn (bench.sh) performance/correctness tests (long)
            DOTEST=$(( DOTEST + 1 ))
            ;;
        v) # [yes] (vanilla C/C++ only: no src/cpu/ JIT assembler)
            if [ -d src/vanilla ]; then DOTARGET="v"; fi
            ;;
        j) # force Intel JIT (src/cpu/ JIT assembly code)
            DOTARGET="j"; DOJIT=100 # 100 means all JIT funcs enabled
            ;;
        d) # [no] debug release
            DODEBUG="y"
            ;;
        D) # [no] Doxygen-only : build documentation and then stop
            DOJUSTDOC="y"
            ;;
        q) # quick: once, skip doxygen on OK build; twice, cd BUILDDIR && make
            QUICK=$((QUICK+1))
            ;;
        Q) # really quick: skip build and doxygen docs [JUST run cmake and stop]
            BUILDOK="n"; DODOC="n"
            ;;
        p) # permissive: disable the FAIL_WITHOUT_MKL switch
            DONEEDMKL="n"
            ;;
        S) # SX cross-compile (size_t=64, built in build-sx/, NEW: default if $CC==sxcc)
            DOTARGET="s"; DOJIT=0; SIZE_T=64; JOBS="-j4"
            ;;
        s) # SX cross-compile (size_t=32, built in build-sx/) DISCOURAGED
            # -s is NOT GOOD: sizeof(ptrdiff_t) is still 8 bytes!
            DOTARGET="s"; DOJIT=0; SIZE_T=32; JOBS="-j4"
            echo "*** WARNING ***"
            echo "-s --> -size_t32 compilation NOT SUPPORTED (-S is recommended)"
            echo "***************"
            ;;
        r) # reference impls only: no -DUSE_CBLAS compile flag (->no im2col gemm)
            USE_CBLAS=0
            ;;
        w) # reduce compiler warnings
            DOWARN=0
            ;;
        W) # lots of compiler warnings (default)
            DOWARN=1
            ;;
        T) # cmake --trace
            CMAKETRACE="--trace"
            ;;
        1) # make -j1
            JOBS="-j1"
            ;;
        5) # gcc-5, if found
            DOGCC_VER=5
            ;;
        6) # gcc-6, if found
            DOGCC_VER=6
            ;;
        7) # gcc-7, if found
            DOGCC_VER=7
            ;;
    h | *) # help
            usage
            ;;
    esac
done
DOJIT=0
INSTALLDIR=install
BUILDDIR=build
#if [ "`echo ${CC}`" == 'sxcc' -a ! "$DOTARGET" == "s" ]; then
#    echo 'Detected $CC == sxcc --> SX compilation with 64-bit size_t'
#    DOTARGET="s"; DOJIT=0; SIZE_T=64; JOBS="-j4"
#fi
#
# I have not yet tried icc.
# For gcc, we will avoid the full MKL (omp issues)
#
if [ "${MKLROOT}" != "" ]; then
	module unload icc >& /dev/null || echo "module icc unloaded"
	if [ "${MKLROOT}" != "" ]; then
		echo "Please compile in an environment without MKLROOT"
		exit -1;
	fi
	# export -n MKLROOT
	# export MKL_THREADING_LAYER=INTEL # maybe ???
fi
#
#
#
if [ "$DOTARGET" == "j" ]; then DOJIT=100; INSTALLDIR='install-jit'; BUILDDIR='build-jit'; fi
if [ "$DOTARGET" == "s" ]; then DONEEDMKL="n"; DODOC="n"; DOTEST=0; INSTALLDIR='install-sx'; BUILDDIR='build-sx';
elif [ "$DOTARGET" != "a" -a "$DOGCC_VER" -gt 0 ]; then
    if $(gcc-${DOGCC_VER} -v); then export CXX=g++-${DOGCC_VER}; export CC=gcc-${DOGCC_VER}; fi
fi
#if [ "$DOTARGET" == "v" ]; then ; fi
if [ "$DODEBUG" == "y" ]; then INSTALLDIR="${INSTALLDIR}-dbg"; BUILDDIR="${BUILDDIR}d"; fi
if [ "$DOJUSTDOC" == "y" ]; then
    (
        if [ ! -d build ]; then mkdir build; fi
        if [ ! -f build/Doxyfile ]; then
            # doxygen does not much care HOW to build, just WHERE
            (cd build && cmake -DCMAKE_INSTALL_PREFIX=../${INSTALL_DIR} -DFAIL_WITHOUT_MKL=OFF ..)
        fi
        echo "Doxygen (please be patient) logging to doxygen.log"
        rm -rf build/doc*stamp build/reference "${INSTALL_DIR}/share/doc"
        #cd build \
        #&& make VERBOSE=1 doc \
        #&& cmake -DCOMPONENT=doc -P cmake_install.cmake
        cd build && make VERBOSE=1 install-doc # Doxygen.cmake custom target
        echo "doxygen.log ends up in gen-dnn project root"
        echo "Documentation installed under ${INSTALL_DIR}/share/doc/"
    ) 2>&1 | tee ../doxygen.log
    exit 0
fi
if [ $QUICK -gt 0 ]; then DODOC="n"; fi
if [ $QUICK -gt 1 ]; then
    if [ ! -f "${BUILDDIR}/Makefile" ]; then # running cmake is absolutely required
        QUICK=1
    fi
fi
timeoutPID() { # unused
    PID="$1"
    timeout="$2"
    interval=1
    delay=1
    (
        ((t = timeout))

        while ((t > 0)); do
            sleep $interval
            kill -0 $$ || exit 0
            ((t -= interval))
        done

        # Be nice, post SIGTERM first.
        # The exit 0 below will be executed if any preceeding command fails.
        kill -s SIGTERM $$ && kill -0 $$ || exit 0
        sleep $delay
        kill -s SIGKILL $$
    ) 2> /dev/null &
}
if [ -d "${BUILDDIR}" -a $QUICK -lt 2 ]; then
    rm -rf "${BUILDDIR}".bak && mv -v "${BUILDDIR}" "${BUILDDIR}".bak
    if [ -f "${BUILDDIR}.log" ]; then
       mv "${BUILDDIR}.log" "${BUILDDIR}".bak/
    fi
fi
if [ -d "$INSTALLDIR}" -a $QUICK -lt 2 ]; then
    rm -rf "$INSTALLDIR}".bak && mv -v "$INSTALLDIR}" "$INSTALLDIR}".bak
fi

TESTRUNNER='time'
if { /usr/bin/time -v echo Hello >& /dev/null; } then
    TESTRUNNER='/usr/bin/time -v'
fi
#if [ "$NEC_FTRACE" -gt 0 ]; then
if [ "$DOTARGET" = "a" -o "$DOTARGET" = "s" ]; then
    #TESTRUNNER="VE_PROGINF=YES ${TESTRUNNER}" #works if used as bash -c ${TESTRUNNER}
    export VE_PROGINF=YES;
    export C_PROGINF=YES;
else
    unset VE_PROGINF
    unset C_PROGINF
fi

if [ "$DOTARGET" = "a" ]; then
    export OMP_NUM_THREADS=1; # for now XXX
fi
# Obtain initial guesses for TESTRUNNER and VE_EXEC
if [ "" ]; then
VE_EXEC=''
TESTRUNNER=''
if [ "$DOTARGET" = "a" ]; then
    export OMP_NUM_THREADS=1; # for now XXX
    if { ve_exec --version 2> /dev/null; } then
        # oops, this will not work for "${TESTRUNNER} make test"
        #TESTRUNNER="${TESTRUNNER} ve_exec"
        #echo "ve_exec! TESTRUNNER ${TESTRUNNER}"
        VE_EXEC=ve_exec
    else
        TESTRUNNER="echo Not-Running "
        echo "Aurora: ve_exec not found"
    fi
fi
echo "TESTRUNNER ${TESTRUNNER}"
echo "VE_EXEC    ${VE_EXEC}"
#read asdf
fi

(
    echo "# vim: set ro ft=log:"
    echo "DOTARGET   $DOTARGET"
    echo "DOJIT      $DOJIT"
    echo "DOTEST     $DOTEST"
    echo "DODEBUG    $DODEBUG"
    echo "DODOC      $DODOC"
    echo "QUICK      $QUICK"
    echo "BUILDDIR   ${BUILDDIR}"
    echo "INSTALLDIR ${INSTALLDIR}"
    if [ $QUICK -lt 2 ]; then
        mkdir "${BUILDDIR}"
    fi
    cd "${BUILDDIR}"
    # iterator debug code ?
    #export CFLAGS="${CFLAGS} -DVERBOSE_PRIMITIVE_CREATE=1"
    #export CXXFLAGS="${CXXFLAGS} -DVERBOSE_PRIMITIVE_CREATE=1"

    #
    # CMAKEOPT="" # allow user to pass flag, ex. CMAKEOPT='--trace -LAH' ./build.sh
    CMAKEOPT="${CMAKEOPT} -DCMAKE_CCXX_FLAGS=-DJITFUNCS=${DOJIT}"
    if [ $USE_CBLAS -ne 0 ]; then
        export CFLAGS="${CFLAGS} -DUSE_CBLAS"
        export CXXFLAGS="${CXXFLAGS} -DUSE_CBLAS"
    fi
    if [ ! "$DOTARGET" == "j" ]; then
        CMAKEOPT="${CMAKEOPT} -DTARGET_VANILLA=ON"
        export CFLAGS="${CFLAGS} -DTARGET_VANILLA"
        export CXXFLAGS="${CXXFLAGS} -DTARGET_VANILLA"
    fi
    if [ "$DOTARGET" == "a" ]; then
        TOOLCHAIN=../cmake/ve.cmake
        if [ ! -f "${TOOLCHAIN}" ]; then echo "Ohoh. ${TOOLCHAIN} not found?"; BUILDOK="n"; fi
        CMAKEOPT="${CMAKEOPT} -DCMAKE_TOOLCHAIN_FILE=${TOOLCHAIN}"
        CMAKEOPT="${CMAKEOPT} -DUSE_OPENMP=OFF -DUSE_SHAREDLIBS=OFF"
        # -proginf  : Run with 'export VE_PROGINF=YES' to get some stats output
        export CFLAGS="${CFLAGS} -DCBLAS_LAYOUT=CBLAS_ORDER -proginf"
        export CXXFLAGS="${CXXFLAGS} -DCBLAS_LAYOUT=CBLAS_ORDER -proginf"
        if [ "$NEC_FTRACE" -eq 1 ]; then
            export CFLAGS="${CFLAGS} -ftrace"
            export CXXFLAGS="${CXXFLAGS} -ftrace"
        fi
        echo "Aurora CMAKEOPT = ${CMAKEOPT}"
    fi
    if [ ${DOWARN} == 'y' ]; then
        DOWARNFLAGS=""
        if [ "$DOTARGET" == "s" ]; then DOWARNFLAGS="-wall"
        else DOWARNFLAGS="-Wall"; fi
        export CFLAGS="${CFLAGS} ${DOWARNFLAGS}"
        export CXXFLAGS="${CXXFLAGS} ${DOWARNFLAGS}"
    fi
    if [ "$DOTARGET" == "s" ]; then
        TOOLCHAIN=../cmake/sx.cmake
        if [ ! -f "${TOOLCHAIN}" ]; then echo "Ohoh. ${TOOLCHAIN} not found?"; BUILDOK="n"; fi
        CMAKEOPT="${CMAKEOPT} -DCMAKE_TOOLCHAIN_FILE=${TOOLCHAIN}"
        CMAKEOPT="${CMAKEOPT} --debug-trycompile --trace -LAH" # long debug of cmake
        #  ... ohoh no easy way to include the spaces and expand variable properly ...
        #      Solution: do these changes within CMakeLists.txt
        #CMAKEOPT="${CMAKEOPT} -DCMAKE_C_FLAGS=-g\ -ftrace\ -Cdebug" # override Cvopt
        #SXOPT="${SXOPT} -DTARGET_VANILLA"
        SXOPT="${SXOPT} -D__STDC_LIMIT_MACROS"
        # __STDC_LIMIT_MACROS is a way to force definitions like INT8_MIN in stdint.h (cstdint)
        #    (it **should** be autmatic in C++11, imho)
        SXOPT="${SXOPT} -woff=1097 -woff=4038" # turn off warnings about not using attributes
        SXOPT="${SXOPT} -woff=1901"  # turn off sxcc warning defining arr[len0] for constant len0
        SXOPT="${SXOPT} -wnolongjmp" # turn off warnings about setjmp/longjmp (and tracing)

        SXOPT="${SXOPT} -Pauto -acct" # enable parallelization (and run with C_PROGINF=YES)
        #SXOPT="${SXOPT} -Pstack" # disable parallelization

        # Generate 'ftrace.out' profiling that can be displayed with ftrace++
        #  BUT not compatible with POSIX threads
        if [ "$NEC_FTRACE" -eq 1 ]; then
            export CFLAGS="${CFLAGS} -ftrace demangled"
            export CXXFLAGS="${CXXFLAGS} -ftrace demangled"
        fi

        # REMOVE WHEN FINISHED SX DEBUGGING
        SXOPT="${SXOPT} -g -traceback" # enable source code tracing ALWAYS
        #SXOPT="${SXOPT} -DVERBOSE_PRIMITIVE_CREATE" # this DOES NOT COMPILE with sxcc: c++11 features missing

        export CFLAGS="${CFLAGS} -size_t${SIZE_T} -Kc99,gcc ${SXOPT}"
        # An object file that is generated with -Kexceptions and an object file
        # that is generated with -Knoexceptions must not be linked together. In
        # such conditions the exception may not be thrown correctly Therefore, do
        # not specify -Kexceptions if the program does not use the try, catch
        # and throw keywords.
        #export CXXFLAGS="${CXXFLAGS} -size_t${SIZE_T} -Kcpp11,gcc,rtti,exceptions ${SXOPT}"
        export CXXFLAGS="${CXXFLAGS} -size_t${SIZE_T} -Kcpp11,gcc,exceptions ${SXOPT}"
        #export CXXFLAGS="${CXXFLAGS} -size_t${SIZE_T} -Kcpp11,gcc,rtti"
    fi
    CMAKEOPT="${CMAKEOPT} -DCMAKE_INSTALL_PREFIX=../${INSTALLDIR}"
    if [ "$DODEBUG" == "y" ]; then
        CMAKEOPT="${CMAKEOPT} -DCMAKE_BUILD_TYPE=Debug"
    else
        CMAKEOPT="${CMAKEOPT} -DCMAKE_BUILD_TYPE=Release"
        #CMAKEOPT="${CMAKEOPT} -DCMAKE_BUILD_TYPE=RelWithDebInfo"
    fi
    if [ "$DONEEDMKL" == "y" ]; then
        CMAKEOPT="${CMAKEOPT} -DFAIL_WITHOUT_MKL=ON"
    fi
    #echo "DONEEDMKL = ${DONEEDMKL}"
    #echo "CMAKEOPT  = ${CMAKEOPT}"
    # Remove leading whitespace from CMAKEENV (bash magic)
    shopt -s extglob; CMAKEENV=\""${CMAKEENV##*([[:space:]])}"\"; shopt -u extglob
    # Without MKL, unit tests take **forever**
    #    TODO: cblas / mathkeisan alternatives?
    if [ "$BUILDOK" == "y" ]; then
        BUILDOK="n"
        if [ $QUICK -gt 1 ]; then # rebuild in existing directory, WITHOUT rerunning cmake
            pwd
            { { make VERBOSE=1 ${JOBS} || make VERBOSE=1; } \
                && BUILDOK="y" || echo "build failed: ret code $?"; }
        else
            rm -f ./stamp-BUILDOK ./CMakeCache.txt
            echo "${CMAKEENV}; cmake ${CMAKEOPT} ${CMAKETRACE} .."
            set -x
            { if [ x"${CMAKEENV}" == x"" ]; then ${CMAKEENV}; fi; \
                cmake ${CMAKEOPT} ${CMAKETRACE} .. \
                && { make VERBOSE=1 ${JOBS} || make VERBOSE=1; } \
                && BUILDOK="y" || echo "build failed: ret code $?"; }
            set +x
        fi
        # Make some assembly-source translations automatically...
        cxxfiles=`(cd ../tests/benchdnn && ls -1 conv/*conv?.cpp conv/*.cxx)`
        echo "cxxfiles = $cxxfiles"
        (cd tests/benchdnn && { for f in ${cxxfiles}; do make -j1 VERBOSE=1 $f.s; done; }) || true
        pwd
        ls -l asm || true

    else # skip the build, just run cmake ...
        echo "CMAKEENV   <${CMAKEENV}>"
        echo "CMAKEOPT   <${CMAKEOPT}>"
        echo "CMAKETRACE <${CMAKETRACE}>"
        set -x
        { if [ x"${CMAKEENV}" == x"" ]; then ${CMAKEENV}; fi; \
            cmake ${CMAKEOPT} ${CMAKETRACE} .. ; }
        set +x
    fi
    set -x
    if [ "$BUILDOK" == "y" -a ! "$DOTARGET" == "s" ]; then
        echo "DOTARGET  $DOTARGET"
        echo "DOJIT     $DOJIT"
        echo "DOTEST    $DOTEST"
        echo "DODEBUG   $DODEBUG"
        echo "DODOC     $DODOC"
        source "./bash_help.inc" # we are already in ${BUILDDIR}
        if [ "${CMAKE_CROSSCOMPILING_EMULATOR}" ]; then
            VE_EXEC="${CMAKE_CROSSCOMPILING_EMULATOR}"
            # Use TESTRUNNER VE_EXEC for explicit targets,
            # But leave out VE_EXEC if executing 'make' within $BUILDDIR,
            # because 'make' tragets already supply VE_EXEC if needed.
        fi
        # Whatever you are currently debugging (and is a quick sanity check) can go here
        if [ -x tests/api-io-c ]; then
            { echo "api-io-c                ..."; ${TESTRUNNER} ${VE_EXEC} tests/api-io-c || BUILDOK="n"; }
        else
            { echo "api-c                ..."; ${TESTRUNNER} ${VE_EXEC} tests/api-c || BUILDOK="n"; }
        fi
        if [ $DOTEST -eq 0 -a $DOJIT -gt 0 ]; then # this is fast ONLY with JIT (< 5 secs vs > 5 mins)
            { echo "simple-training-net-cpp ..."; ${TESTRUNNER}  ${VE_EXEC} examples/simple-training-net-cpp || BUILDOK="n"; }
        fi
    fi
    if [ "$BUILDOK" == "y" -a "$DOTARGET" == "s" ]; then
        # make SX build dirs all-writable so SX runs can store logs etc.
        #find "${BUILDDIR}" -type d -exec chmod o+w {} \;
        { cd ..; find "${BUILDDIR}" -type d -exec chmod o+w {} \; ; }
    fi
    if [ "$BUILDOK" == "y" ]; then
        touch ./stamp-BUILDOK
        if [ "$DODOC" == "y" ]; then
            echo "Build OK... Doxygen (please be patient)"
            make VERBOSE=1 doc >& ../doxygen.log
        fi
    fi
    set +x
) 2>&1 | tee "${BUILDDIR}".log
ls -l "${BUILDDIR}"
BUILDOK="n"; sync "${BUILDDIR}/stamp-BUILDOK"; if [ -f "${BUILDDIR}/stamp-BUILDOK" ]; then BUILDOK="y"; fi

set +x
# after cmake we might have a better idea about ve_exec (esp. if PATH is not set properly)
if [ -f "${BUILDDIR}/bash_help.inc" ]; then
    # snarf some CMAKE variables
    source "${BUILDDIR}/bash_help.inc"
    TESTRUNNER=''
    if [ "${CMAKE_CROSSCOMPILING_EMULATOR}" ]; then
        VE_EXEC="${CMAKE_CROSSCOMPILING_EMULATOR}"
        if [ ! -x "${VE_EXEC}" ]; then
            TESTRUNNER="echo Not-Running "
            echo "cmake crosscompiling emulator, such as ve_exec, not available?"
        fi
    fi
fi
set -x
echo "TESTRUNNER ${TESTRUNNER}"
echo "VE_EXEC    ${VE_EXEC}"

echo "BUILDDIR   ${BUILDDIR}"
echo "INSTALLDIR ${INSTALLDIR}"
echo "DOTARGET=${DOTARGET}, DOJIT=${DOJIT}, DODEBUG=${DODEBUG}, DOTEST=${DOTEST}, DODOC=${DODOC}, DONEEDMKL=${DONEEDMKL}"
LOGDIR="log-${DOTARGET}${DOJIT}${DODEBUG}${DOTEST}${DODOC}${DONEEDMKL}"
if [ "$BUILDOK" == "y" ]; then
    set -x
    echo "BUILDOK !    QUICK=$QUICK"
    if [ $QUICK -lt 2 ]; then # make install ?
        (
        cd "${BUILDDIR}"
        # trouble with cmake COMPONENTs ...
        echo "Installing :"; make install;
        #if [ "$DODOC" == "y" ]; then { echo "Installing docs ..."; make install-doc; } fi
        ) 2>&1 >> "${BUILDDIR}".log || { echo "'make install' in ${BUILDDIR} had issues (ignored)"; }
    fi
    echo "Testing ?"
    if [ ! $DOTEST -eq 0 -a ! "$DOTARGET" == "s" ]; then # non-SX: -t might run some tests
        rm -f test1.log test2.log test3.log
        echo "Testing ... test1"
        if [ true ]; then
            (cd "${BUILDDIR}" && ARGS='-VV -E .*test_.*' ${TESTRUNNER} make test) 2>&1 | tee "${BUILDDIR}/test1.log" || true
        fi
        if [ $DOTEST -ge 2 ]; then
            echo "Testing ... test2"
            (cd "${BUILDDIR}" && ARGS='-VV -N' ${TESTRUNNER} make test \
            && ARGS='-VV -R .*test_.*' ${TESTRUNNER}  make test) 2>&1 | tee "${BUILDDIR}/test2.log" || true
        fi
        if [ $DOTEST -ge 3 ]; then
            if [ -x ./bench.sh ]; then
                ${TESTRUNNER} ./bench.sh -q${DOTARGET} 2>&1 | tee "${BUILDDIR}/test3.log" || true
            fi
        fi
        echo "Tests done"
    fi
    if [ ! $DOTEST -eq 0 -a "$DOTARGET" == "s" ]; then
        echo 'SX testing should be done manually (ex. ~/tosx script to log in to SX)'
    fi
    set +x
else
    echo "Build NOT OK..."
fi

# maintain directories of "success" log files
echo "BUILDDIR   ${BUILDDIR}"
echo "INSTALLDIR ${INSTALLDIR}"
echo "DOTARGET=${DOTARGET}, DOJIT=${DOJIT}, DODEBUG=${DODEBUG}, DOTEST=${DOTEST}, DODOC=${DODOC}, DONEEDMKL=${DONEEDMKL}"
if [ "${BUILDOK}" == "y" ]; then
    if [ $DOTEST -gt 0 ]; then
        echo "LOGDIR:       ${LOGDIR}" 2>&1 >> "${BUILDDIR}".log
    fi
    if [ $DOTEST -gt 0 ]; then
        if [ -d "${LOGDIR}" ]; then rm -rf "${LOGDIR}.bak"; mv -v "${LOGDIR}" "${LOGDIR}.bak"; fi
        mkdir ${LOGDIR}
        pwd -P
        ls "${BUILDDIR}/"*log
        for f in "${BUILDDIR}/"*log doxygen.log; do
            cp -av "${f}" "${LOGDIR}/" || true
        done
    fi
fi
echo "FINISHED:     $ORIGINAL_CMD" 2>&1 >> "${BUILDDIR}".log
# for a debug compile  --- FIXME
#(cd "${BUILDDIR}" && ARGS='-VV -R .*simple_training-net-cpp' /usr/bin/time -v make test) 2>&1 | tee test1-dbg.log
#(cd "${BUILDDIR}" && ARGS='-VV -R .*simple_training-net-cpp' valgrind make test) 2>&1 | tee test1-valgrind.log
