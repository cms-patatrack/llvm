// RUN: %clang -### -fsycl -c %s 2>&1 | FileCheck %s --check-prefix=ENABLED
// RUN: %clang -### -fsycl %s 2>&1 | FileCheck %s --check-prefix=ENABLED
// RUN: %clang -### -fsycl -sycl-std=1.2.1 %s 2>&1 | FileCheck %s --check-prefix=ENABLED
// RUN: %clang -### -fsycl -sycl-std=121 %s 2>&1 | FileCheck %s --check-prefix=ENABLED
// RUN: %clang -### -fsycl -sycl-std=2017 %s 2>&1 | FileCheck %s --check-prefix=ENABLED
// RUN: %clang -### -fsycl -sycl-std=sycl-1.2.1 %s 2>&1 | FileCheck %s --check-prefix=ENABLED
// RUN: %clang -### -fno-sycl -fsycl %s 2>&1 | FileCheck %s --check-prefix=ENABLED
// RUN: %clang -### -sycl-std=2017 %s 2>&1 | FileCheck %s --check-prefix=DISABLED
// RUN: %clangxx -### -fsycl %s 2>&1 | FileCheck %s --check-prefix=ENABLED
// RUN: %clangxx -### -fno-sycl %s 2>&1 | FileCheck %s --check-prefix=DISABLED
// RUN: %clangxx -### -fsycl -fno-sycl %s 2>&1 | FileCheck %s --check-prefix=DISABLED
// RUN: %clangxx -### %s 2>&1 | FileCheck %s --check-prefix=DISABLED
// RUN: %clang_cl -### -fsycl -sycl-std=2017 -- %s 2>&1 | FileCheck %s --check-prefix=ENABLED
// RUN: %clang_cl -### -fsycl -- %s 2>&1 | FileCheck %s --check-prefix=ENABLED
// RUN: %clang_cl -### -- %s 2>&1 | FileCheck %s --check-prefix=DISABLED

// ENABLED: "-cc1"{{.*}} "-fsycl-is-device"
// ENABLED-SAME: "-sycl-std={{[-.sycl0-9]+}}"
// ENABLED-SAME: "-internal-isystem" "{{.*}}bin{{[/\\]+}}..{{[/\\]+}}include{{[/\\]+}}sycl"

// DISABLED-NOT: "-fsycl-is-device"
// DISABLED-NOT: "-sycl-std="

// RUN: %clang -### -fsycl-device-only -c %s 2>&1 | FileCheck %s --check-prefix=DEFAULT
// RUN: %clang -### -fsycl-device-only %s 2>&1 | FileCheck %s --check-prefix=DEFAULT
// RUN: %clang -### -fsycl-device-only -S %s 2>&1 | FileCheck %s --check-prefix=TEXTUAL
// RUN: %clang -### -fsycl-device-only -fsycl  %s 2>&1 | FileCheck %s --check-prefix=DEFAULT
// RUN: %clang -### -fsycl-device-only -fno-sycl-use-bitcode -c %s 2>&1 | FileCheck %s --check-prefix=NO-BITCODE
// RUN: %clang -### -target spir64-unknown-linux-sycldevice -c -emit-llvm %s 2>&1 | FileCheck %s --check-prefix=TARGET
// RUN: %clang -### -fsycl-device-only -c -emit-llvm %s 2>&1 | FileCheck %s --check-prefix=COMBINED
// RUN: %clangxx -### -fsycl-device-only %s 2>&1 | FileCheck %s --check-prefix=DEFAULT
// RUN: %clang_cl -### -fsycl-device-only %s 2>&1 | FileCheck %s --check-prefix=DEFAULT

// DEFAULT: "-triple" "spir64-unknown-{{.*}}-sycldevice{{.*}}" "-fsycl-is-device"{{.*}} "-emit-llvm-bc"
// DEFAULT: "-internal-isystem" "{{.*}}bin{{[/\\]+}}..{{[/\\]+}}include{{[/\\]+}}sycl"
// DEFAULT: "-internal-isystem" "{{.*lib.*clang.*include}}"
// DEFAULT-NOT: "{{.*}}llvm-spirv"{{.*}} "-spirv-max-version=1.1"{{.*}} "-spirv-ext=+all"
// DEFAULT-NOT: "-std=c++11"
// DEFAULT-NOT: "-std=c++14"
// NO-BITCODE: "-triple" "spir64-unknown-{{.*}}-sycldevice"{{.*}} "-fsycl-is-device"{{.*}} "-emit-llvm-bc"
// NO-BITCODE: "{{.*}}llvm-spirv"{{.*}} "-spirv-max-version=1.1"{{.*}} "-spirv-ext=+all"
// TARGET: "-triple" "spir64-unknown-linux-sycldevice"{{.*}} "-fsycl-is-device"{{.*}} "-emit-llvm-bc"
// COMBINED: "-triple" "spir64-unknown-{{.*}}-sycldevice"{{.*}} "-fsycl-is-device"{{.*}} "-emit-llvm-bc"
// TEXTUAL: "-triple" "spir64-unknown-{{.*}}-sycldevice{{.*}}" "-fsycl-is-device"{{.*}} "-emit-llvm"

/// Verify -fsycl-device-only phases
// RUN: %clang -### -ccc-print-phases -fsycl-device-only %s 2>&1 | FileCheck %s --check-prefix=DEFAULT-PHASES
// DEFAULT-PHASES: 0: input, "{{.*}}", c
// DEFAULT-PHASES: 1: preprocessor, {0}, cpp-output
// DEFAULT-PHASES: 2: compiler, {1}, ir
// DEFAULT-PHASES: 3: backend, {2}, ir
// DEFAULT-PHASES-NOT: linker

// -fsycl-help tests
// RUN: mkdir -p %t-sycl-dir
// RUN: touch %t-sycl-dir/aoc
// RUN: chmod +x %t-sycl-dir/aoc
// Test with a bad argument is expected to fail
// RUN: not %clang -fsycl-help=foo %s 2>&1 | FileCheck %s --check-prefix=SYCL-HELP-BADARG
// RUN: %clang -### -fsycl-help=gen %s 2>&1 | FileCheck %s --check-prefix=SYCL-HELP-GEN
// RUN: env PATH=%t-sycl-dir %clang -### -fsycl-help=fpga %s 2>&1 | FileCheck %s --check-prefixes=SYCL-HELP-FPGA,SYCL-HELP-FPGA-OUT -DDIR=%t-sycl-dir
// RUN: %clang -### -fsycl-help=x86_64 %s 2>&1 | FileCheck %s --check-prefix=SYCL-HELP-CPU
// RUN: %clang -### -fsycl-help %s 2>&1 | FileCheck %s --check-prefixes=SYCL-HELP-GEN,SYCL-HELP-FPGA,SYCL-HELP-CPU
// SYCL-HELP-BADARG: unsupported argument 'foo' to option 'fsycl-help='
// SYCL-HELP-GEN: Emitting help information for ocloc
// SYCL-HELP-GEN: Use triple of 'spir64_gen-unknown-unknown-sycldevice' to enable ahead of time compilation
// SYCL-HELP-FPGA-OUT: "[[DIR]]{{[/\\]+}}aoc" "-help"
// SYCL-HELP-FPGA: Emitting help information for aoc
// SYCL-HELP-FPGA: Use triple of 'spir64_fpga-unknown-unknown-sycldevice' to enable ahead of time compilation
// SYCL-HELP-CPU: Emitting help information for opencl-aot
// SYCL-HELP-CPU: Use triple of 'spir64_x86_64-unknown-unknown-sycldevice' to enable ahead of time compilation
