#!/bin/bash
# vim: et ts=4 sw=4
#
# Usage: ./bench.sh -h
#   This runs benchdnn for some [quick?] --cfg=f32 *convolution* tests
#   Modify at will.  BUILDDIR matched with build.sh.
#
ORIGINAL_CMD="$0 $*"
usage() {
    echo "$0 usage:"
    #head -n 30 "$0" | grep "^[^#]*.)\ #"
    awk '/getopts/{flag=1;next} /done/{flag=0} flag&&/^[^#]+\) #/; flag&&/^ *# /' $0
    echo " This runs a default set of tests, typically some large convolutions"
    echo " from some real deep neural nets."
    echo " Examples:"
    echo "   ./bench.sh -v -t 4 -D 'BWD_D BWD_B'"
    echo "       # vanilla build, 4 threads, some BWD tests of default convolutions"
    echo "   ./bench.sh -q"
    echo "       # quick test only, FWD_B direction, debug jit build"
    echo "   ./bench.sh -v -t 6 -D 'FWD_B FWD_D BWD_D BWD_W BWD_WB'"
    echo "       # Long vanilla build test, 6 threads, all directions"
    exit 0
}
################# LOGDIR
if [ `uname` == 'SUPER-UX' ]; then
	LOGDIR=guest/sx
	mkdir -p guest/sx       # actually this should be in build.sh, no perms on Ace/Aurora
	chmod ugo+w guest
	chmod ugo+w guest/sx
else
    LOGDIR=.
fi
DOTARGET='v'
DOQUICK="n"
THREADS=0
BATCHDIR=tests/benchdnn/inputs
BATCHFILE="${BATCHDIR}/conv_regression_small_spatial" # for example
VERBOSITY=0
SKIP=""
MODE="--mode=P"
DIRS=(FWD_B) # FWD_D FWD_B BWD_D BWD_W BWD_WB
# TODO: add batch file selection for some long tests examples [other than the default list]
while getopts ":hqvjdD:m:t:V:s:" arg; do
    #echo "arg = ${arg}, OPTIND = ${OPTIND}, OPTARG=${OPTARG}"
    case $arg in
        v) # [yes] (if available, vanilla C/C++ only: no JIT)
            if [ -d src/vanilla -a -z "$BUILDDIR" ]; then BUILDDIR='build'; DOTARGET='v'; fi
            ;;
        j) # force Intel JIT (src/cpu/ JIT assembly code)
            if [ -z $BUILDDIR ]; then
                if [ -d build-jit ]; then BUILDDIR='build-jit'; DOTARGET='j';
                else                      BUILDDIR='build';     DOTARGET=''; fi
            fi
            ;;
        d) # [no] debug release
            DODEBUG="y"
            ;;
        D) # DIRS list : select from [FWD_B] FWD_D BWD_D BWD_W BWD_WB
            DIRS=(${OPTARG})
            ;;
        q) # quick : just a few QUICK samples
            DOQUICK="y"
            ;;
        m) # test mode string, C P [A] T -- for Corr Perf All Test
            MODE="--mode=${OPTARG}"
            ;;
        t) # N threads [0: default env variables]
            THREADS=${OPTARG}
            ;;
        V) # N [0] verbosity
            VERBOSITY=${OPTARG}
            ;;
        s) # skip-impl string [""]
            SKIP="${OPTARG}"
            ;;
    h | *) # help
            usage
            ;;
    esac
done
################# BUILDDIR
if [ -z "$BUILDDIR" ]; then
    if [ `uname` == 'SUPER-UX' ]; then
        # let's take the MOST RECENT of build-sx or build-sxd directories ...
        BUILDDIR=`ls -ldst build-sx* | grep ' drwx' | head -1 | sed 's/.*\(build.*\)/\1/'`
        DOTARGET='s'
    else
        if [ "${CC##sx}" == "sx" -o "${CXX##sx}" == "sx" ]; then
            BUILDDIR='build-sx'     # s for SX (C/C++ code, cross-compile)
            mkdir -p guest/sx       # perhaps should be in build.sh too
            chmod ugo+w guest
            chmod ugo+w guest/sx
        elif [ -d src/vanilla ]; then
            BUILDDIR='build'        # v for vanilla (C/C++ code)
            DOTARGET='v'
        else
            if [ -d build-jit ]; then
                BUILDDIR='build-jit'    # j for JIT (Intel assembler)
                DOTARGET='j'
            else
                BUILDDIR='build'
                DOTARGET=''
            fi
        fi
    fi
    echo "build directory GUESS: ${BUILDDIR}"
else
    echo "build directory GIVEN: ${BUILDDIR}"
fi
################# check BUILDDIR exists
if [ "$DODEBUG" == "y" ]; then
    if [ -d "${BUILDDIR}d" ]; then
        BUILDDIR="${BUILDDIR}d"
        DOTARGET="${DOTARGET}d"
    else
        echo "debug build dir ${BUILDDIR}d absent"
        echo "Perhaps run build.sh?"
    fi
fi
if [ ! -d "${BUILDDIR}" ]; then
    echo "Error: build directory ${BUILDDIR} absent"
    echo "Perhaps run build.sh?"
fi
################# remake benchdnn (just in case)
BENCHDIR="${BUILDDIR}/tests/benchdnn"
if [ ! -d "${BENCHDIR}" ]; then
    echo "Ohoh: benchdnn directory not found: ${BENCHDIR}"
    exit 0
fi
if [ `uname` == 'SUPER-UX' ]; then
	chmod -R ugo+w "${BUILDDIR}" # try anyway
	chmod ugo+w "${BENCHDIR}"
else
    if [ "${CC##sx}" == "sx" -o "${CXX##sx}" == "sx" ]; then
        chmod -R ugo+w "${BUILDDIR}" # just in case
        chmod ugo+w "${BENCHDIR}"
    fi
	(cd ${BUILDDIR}/tests/benchdnn && VERBOSE=1 make) || { echo "Compile issues?"; exit; }
fi
#################
echo "build    directory : ${BUILDDIR}"
echo "log      directory : ${LOGDIR}"
echo "benchdnn directory : ${BENCHDIR}"
echo "V-erbosity         : ${VERBOSITY}"
echo "m-ode              : ${MODE}"
echo "s-kip              : ${SKIP}"
echo "D-irections        : ${DIRS[@]}" # multiple, like "FWD_B BWD_D BWD_WB", OK
if [ "${CC##sx}" == "sx" -o "${CXX##sx}" == "sx" ]; then
    echo "SX: cross-compiled benchdnn must be run on SX ACE/Aurora"
    exit 0
fi
#
ls -ld ${BENCHDIR}
cat <<EOF
Here are the output fields for performance benchmarks:
    string: perf
    run name
    full conv-desc
    number of giga ops calculated
    minimum time spent in ms
    best gigaops (since it corresponds to mimimum time)
    average time spent in ms
    average gigaops (since it corresponds to average time)
    convolution implementation name
Documentation:
    tests/benchdnn/README.md
EOF
echo "bench.sh : benchdnn Convolution Performance ..."
RUNME=./benchdnn
################# threads (environment)
if [ ${THREADS} -gt 0 ]; then
    export OMP_NUM_THREADS=${THREADS}
    export MKL_NUM_THREADS=${THREADS}
fi
#################
{
#       --batch=inputs/test_conv_all ... selected the winograd examples
# [ejk] direct conv + benchdnn doesn't work (was same in mkl-dnn master).
#       does it require special fmt?
#(cd ${BENCHDIR} && ./benchdnn --conv --mode=C -v${VERBOSITY} --skip-impl="$SKIP" \
#    --reset --cfg=f32_wino --alg=wino \
#    --match=.*kh3[^0-9].* \
#    --allow-unimpl=true \
#    --mb=2 \
#    --dir=FWD_B --batch=inputs/conv_all \
#    --dir=BWD_D --batch=inputs/conv_all \
#    --dir=BWD_WB --batch=inputs/conv_all \
#    --mb=1 \
#    --dir=FWD_B --batch=inputs/conv_all \
#    ) || { echo "Ohoh"; }
#(cd ${BENCHDIR} && ./benchdnn --conv --mode=AP --alg=DIRECT -v${VERBOSITY} --cfg=f32 --dir=FWD_B --skip-impl="$SKIP" \
#    mb1ic16oc16_ih32iw32kh3kw3_ph0pw0 \
#    mb1ic16oc16_ih3oh1kh5ph0 \
#    ) || { echo "Ohoh"; }
#(cd ${BENCHDIR} && ./benchdnn --conv --mode=AP -v${VERBOSITY} --cfg=f32 --dir=FWD_D --skip-impl="$SKIP" \
#    mb12_ic3ih227iw227_oc96oh55ow55_kh11kw11_sh4sw4ph0pw0_nalexnet:conv1 \
#    ) || { echo "Ohoh"; }
(cd ${BENCHDIR} && ./benchdnn --conv --mode=AC -v${VERBOSITY} --cfg=f32 --dir=FWD_D --skip-impl="$SKIP" \
    mb32_ic3ih44iw44_oc7oh10_kh11kw11_sh4sw4ph0pw0_nsmall1 \
    mb32_ic3ih44kh3_oc7oh42_nsmall2 \
    ) || { echo "Ohoh"; }
(cd ${BENCHDIR} && ./benchdnn --conv --mode=AP -v${VERBOSITY} --cfg=f32 --dir=FWD_D --skip-impl="$SKIP" \
    mb32_ic3ih44iw44_oc7oh10_kh11kw11_sh4sw4ph0pw0_nsmall1 \
    mb32_ic3ih44kh3_oc7oh42_nsmall2 \
    ) || { echo "Ohoh"; }
} 2>&1 | tee ${LOGDIR}/bench-quick-t${THREADS}.log
echo "bench.sh quick tests DONE -- logfile ${LOGDIR}/bench-quick-t${THREADS}.log"
#################
if [ `uname` == 'SUPER-UX' -o "${DOQUICK}" = "y" ]; then
	echo "Skipping long tests"
else
    for dir in ${DIRS[@]}; do
        echo ""
        echo "Bench Convolution Long Tests"
        echo "for ${MODE} --dir=${dir}  conv layers (many!) ..."
        echo ""
        LGBASE="${LOGDIR}/bench-convP-${DOTARGET}t${THREADS}"
        echo "cmd: ./benchdnn --conv ${MODE} -v${VERBOSITY} --cfg=f32 --dir=${dir} --skip-impl=${SKIP}"
        (cd ${BENCHDIR} && ./benchdnn --conv ${MODE} -v${VERBOSITY} --cfg=f32 --dir=${dir} --skip-impl="${SKIP}" \
            ) 2>&1 | tee ${LGBASE}-tmp.log \
            && mv ${LGBASE}-tmp.log ${LGBASE}.log && echo "${LGBASE}.log OK" \
            || { echo "${LGBASE}-tmp.log ERROR"; exit; };
    done;
fi
#
