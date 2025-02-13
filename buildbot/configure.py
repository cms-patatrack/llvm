import argparse
import os
import subprocess
import sys
import platform

def do_configure(args):
    ret = False

    llvm_dir = os.path.join(args.src_dir, "llvm")
    sycl_dir = os.path.join(args.src_dir, "sycl")
    spirv_dir = os.path.join(args.src_dir, "llvm-spirv")
    xpti_dir = os.path.join(args.src_dir, "xpti")
    libdevice_dir = os.path.join(args.src_dir, "libdevice")
    ocl_header_dir = os.path.join(args.obj_dir, "OpenCL-Headers")
    icd_loader_lib = os.path.join(args.obj_dir, "OpenCL-ICD-Loader", "build")
    llvm_targets_to_build = 'X86'
    llvm_enable_projects = 'clang;llvm-spirv;sycl;opencl-aot;xpti;libdevice'
    libclc_targets_to_build = ''
    sycl_build_pi_cuda = 'OFF'
    llvm_enable_assertions = 'ON'
    llvm_enable_doxygen = 'OFF'
    llvm_build_shared_libs = 'OFF'

    if platform.system() == 'Linux':
        icd_loader_lib = os.path.join(icd_loader_lib, "libOpenCL.so")
    else:
        icd_loader_lib = os.path.join(icd_loader_lib, "OpenCL.lib")

    if args.cuda:
        llvm_targets_to_build += ';NVPTX'
        llvm_enable_projects += ';libclc'
        libclc_targets_to_build = 'nvptx64--;nvptx64--nvidiacl'
        sycl_build_pi_cuda = 'ON'

    if args.assertions:
        llvm_enable_assertions = 'ON'

    if args.docs:
        llvm_enable_doxygen = 'ON'

    if args.shared_libs:
        llvm_build_shared_libs = 'ON'

    install_dir = os.path.join(args.obj_dir, "install")

    cmake_cmd = [
        "cmake",
        "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE={}".format(args.build_type),
        "-DLLVM_ENABLE_ASSERTIONS={}".format(llvm_enable_assertions),
        "-DLLVM_TARGETS_TO_BUILD={}".format(llvm_targets_to_build),
        "-DLLVM_EXTERNAL_PROJECTS=sycl;llvm-spirv;opencl-aot;xpti;libdevice",
        "-DLLVM_EXTERNAL_SYCL_SOURCE_DIR={}".format(sycl_dir),
        "-DLLVM_EXTERNAL_LLVM_SPIRV_SOURCE_DIR={}".format(spirv_dir),
        "-DLLVM_EXTERNAL_XPTI_SOURCE_DIR={}".format(xpti_dir),
        "-DLLVM_EXTERNAL_LIBDEVICE_SOURCE_DIR={}".format(libdevice_dir),
        "-DLLVM_ENABLE_PROJECTS={}".format(llvm_enable_projects),
        "-DLIBCLC_TARGETS_TO_BUILD={}".format(libclc_targets_to_build),
        "-DSYCL_BUILD_PI_CUDA={}".format(sycl_build_pi_cuda),
        "-DLLVM_BUILD_TOOLS=ON",
        "-DSYCL_ENABLE_WERROR=ON",
        "-DCMAKE_INSTALL_PREFIX={}".format(install_dir),
        "-DSYCL_INCLUDE_TESTS=ON", # Explicitly include all kinds of SYCL tests.
        "-DLLVM_ENABLE_DOXYGEN={}".format(llvm_enable_doxygen),
        "-DBUILD_SHARED_LIBS={}".format(llvm_build_shared_libs),
        "-DSYCL_ENABLE_XPTI_TRACING=ON", # Explicitly turn on XPTI tracing
        llvm_dir
    ]

    if not args.no_ocl:
      cmake_cmd.extend([
            "-DOpenCL_INCLUDE_DIR={}".format(ocl_header_dir),
            "-DOpenCL_LIBRARY={}".format(icd_loader_lib)])

    print(cmake_cmd)

    try:
        subprocess.check_call(cmake_cmd, cwd=args.obj_dir)
    except subprocess.CalledProcessError:
        cmake_cache = os.path.join(args.obj_dir, "CMakeCache.txt")
        if os.path.isfile(cmake_cache):
            os.remove(cmake_cache)
        subprocess.check_call(cmake_cmd, cwd=args.obj_dir)

    ret = True
    return ret

def main():
    parser = argparse.ArgumentParser(prog="configure.py",
                                     description="script to do configure",
                                     formatter_class=argparse.RawTextHelpFormatter)
    parser.add_argument("-n", "--build-number", metavar="BUILD_NUM", help="build number")
    parser.add_argument("-b", "--branch", metavar="BRANCH", help="pull request branch")
    parser.add_argument("-d", "--base-branch", metavar="BASE_BRANCH", help="pull request base branch")
    parser.add_argument("-r", "--pr-number", metavar="PR_NUM", help="pull request number")
    parser.add_argument("-w", "--builder-dir", metavar="BUILDER_DIR",
                        help="builder directory, which is the directory contains source and build directories")
    parser.add_argument("-s", "--src-dir", metavar="SRC_DIR", required=True, help="source directory")
    parser.add_argument("-o", "--obj-dir", metavar="OBJ_DIR", required=True, help="build directory")
    parser.add_argument("-t", "--build-type",
                        metavar="BUILD_TYPE", required=True, help="build type, debug or release")
    parser.add_argument("--cuda", action='store_true', help="switch from OpenCL to CUDA")
    parser.add_argument("--assertions", action='store_true', help="build with assertions")
    parser.add_argument("--docs", action='store_true', help="build Doxygen documentation")
    parser.add_argument("--no-ocl", action='store_true', help="download OpenCL deps via CMake")
    parser.add_argument("--shared-libs", action='store_true', help="Build shared libraries")

    args = parser.parse_args()

    print("args:{}".format(args))

    return do_configure(args)

if __name__ == "__main__":
    ret = main()
    exit_code = 0 if ret else 1
    sys.exit(exit_code)
