#!/usr/bin/env python

import os, sys, subprocess, argparse, shutil, glob, re, multiprocessing
import logging as log

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))

class Fail(Exception):
    def __init__(self, text=None):
        self.t = text
    def __str__(self):
        return "ERROR" if self.t is None else self.t

def execute(cmd, shell=False):
    try:
        log.info("Executing: %s" % cmd)
        env = os.environ.copy()
        env['VERBOSE'] = '1'
        retcode = subprocess.call(cmd, shell=shell, env=env)
        if retcode < 0:
            raise Fail("Child was terminated by signal: %s" % -retcode)
        elif retcode > 0:
            raise Fail("Child returned: %s" % retcode)
    except OSError as e:
        raise Fail("Execution failed: %d / %s" % (e.errno, e.strerror))

def rm_one(d):
    d = os.path.abspath(d)
    if os.path.exists(d):
        if os.path.isdir(d):
            log.info("Removing dir: %s", d)
            shutil.rmtree(d)
        elif os.path.isfile(d):
            log.info("Removing file: %s", d)
            os.remove(d)

def check_dir(d, create=False, clean=False):
    d = os.path.abspath(d)
    log.info("Check dir %s (create: %s, clean: %s)", d, create, clean)
    if os.path.exists(d):
        if not os.path.isdir(d):
            raise Fail("Not a directory: %s" % d)
        if clean:
            for x in glob.glob(os.path.join(d, "*")):
                rm_one(x)
    else:
        if create:
            os.makedirs(d)
    return d

def check_file(d):
    d = os.path.abspath(d)
    if os.path.exists(d):
        if os.path.isfile(d):
            return True
        else:
            return False
    return False

def find_file(name, path):
    for root, dirs, files in os.walk(path):
        if name in files:
            return os.path.join(root, name)

class Builder:
    def __init__(self, options):
        self.options = options
        self.build_dir = check_dir(options.build_dir, create=True)
        self.gendnn_dir = check_dir(options.gendnn_dir)
        #self.emscripten_dir = check_dir(options.emscripten_dir)

    def get_toolchain_file(self):
        return os.path.join(self.emscripten_dir, "cmake", "Modules", "Platform", "Emscripten.cmake")

    def clean_build_dir(self):
        for d in ["CMakeCache.txt", "CMakeFiles/", "src/"]:
            rm_one(d)

    def get_cmake_cmd(self):
        cmd = ["emcmake", "cmake",
               "-DCMAKE_BUILD_TYPE=Release",
               #"-DCMAKE_TOOLCHAIN_FILE='%s'" % self.get_toolchain_file(),
               "-DENABLE_EMSCRIPTEN=ON"
               ]

        if self.options.cmake_option:
            cmd += self.options.cmake_option

        flags = self.get_build_flags()

        if flags:
            cmd += ["-DCMAKE_C_FLAGS='%s'" % flags,
                    "-DCMAKE_CXX_FLAGS='%s'" % flags]
        return cmd

    def get_build_flags(self):
        flags = " "

        if self.options.threads:
            flags += "-s USE_PTHREADS=1 -s PTHREAD_POOL_SIZE=4 "
        else:
            flags += "-s USE_PTHREADS=0 "

        # We are building GEN-DNN libraries as side modules for OpenCV main module.
        flags += "-s SIDE_MODULE=1 "

        if self.options.build_flags:
            flags += self.options.build_flags

        #flags += "-s USE_PTHREADS=1 -s PTHREAD_POOL_SIZE=4 "
        #flags +="-s ASSERTIONS=1 -s DISABLE_EXCEPTION_CATCHING=2 "
        #flags +="-s ALLOW_MEMORY_GROWTH=1 -s TOTAL_MEMORY=10MB -s RESERVED_FUNCTION_POINTERS=100 --use-preload-plugins "
        #flags += "-s NO_EXIT_RUNTIME=1 "
        #flags += "-s LINKABLE=1 "
        #flags += "-s EXPORTED_FUNCTIONS=\"['__ZNK6mkldnn4impl3cpu17_ref_rnn_common_tIL18mkldnn_prop_kind_t64EL18mkldnn_data_type_t1ELS4_1EE13bias_finalizeERKNS1_9rnn_utils10rnn_conf_tEPfPKfSC_','__ZN6mkldnn4impl3cpu14engine_factoryE','__ZTV21mkldnn_primitive_desc']\" "

        return flags

    def config(self):
        cmd = self.get_cmake_cmd()
        cmd.append(self.gendnn_dir)
        execute(cmd)

    def build_gendnn(self):
        execute(["emmake", "make", "-j", str(multiprocessing.cpu_count())])


#===================================================================================================

if __name__ == "__main__":
    gendnn_dir = os.path.abspath(os.path.join(SCRIPT_DIR, '.'))

    #emscripten_dir = None
    #if "EMSCRIPTEN" in os.environ:
    #    emscripten_dir = os.environ["EMSCRIPTEN"]

    parser = argparse.ArgumentParser(description='Build GEN-DNN by Emscripten')
    parser.add_argument("build_dir", help="Building directory (and output)")
    parser.add_argument('--gendnn_dir', default=gendnn_dir, help='GEN-DNN source directory (default is "./" relative to script location)')
    #parser.add_argument('--emscripten_dir', default=emscripten_dir, help="Path to Emscripten to use for build")
    parser.add_argument('--clean_build_dir', action="store_true", help="Clean build dir")
    parser.add_argument('--skip_config', action="store_true", help="Skip cmake config")
    parser.add_argument('--config_only', action="store_true", help="Only do cmake config")
    parser.add_argument('--threads', action="store_true", help="Build GEN-DNN with threads optimization")
    # Use flag --cmake option="-D...=ON" only for one argument, if you would add more changes write new cmake_option flags
    parser.add_argument('--cmake_option', action='append', help="Append CMake options")
    # Use flag --build_flags="-s USE_PTHREADS=0 -Os" for one and more arguments as in the example
    parser.add_argument('--build_flags', help="Append Emscripten build options")

    args = parser.parse_args()

    log.basicConfig(format='%(message)s', level=log.DEBUG)
    log.debug("Args: %s", args)

    builder = Builder(args)

    os.chdir(builder.build_dir)

    if args.clean_build_dir:
        log.info("=====")
        log.info("===== Clean build dir %s", builder.build_dir)
        log.info("=====")
        builder.clean_build_dir()

    if not args.skip_config:
        log.info("=====")
        log.info("===== Config GEN-DNN build")
        log.info("=====")
        builder.config()

    if args.config_only:
        sys.exit(0)

    log.info("=====")
    log.info("===== Building GEN-DNN by Emscripten")
    log.info("=====")
    builder.build_gendnn()

    log.info("=====")
    log.info("===== Build finished")
    log.info("=====")

    gendnn_libs_path = os.path.join(builder.build_dir, "..", "bin", "Release", "lib")
    if check_file(gendnn_libs_path):
        log.info("Libs location: %s", gendnn_libs_path)
