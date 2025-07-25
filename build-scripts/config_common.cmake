# Copyright (C) 2019 Intel Corporation.  All rights reserved.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

string(TOUPPER ${WAMR_BUILD_TARGET} WAMR_BUILD_TARGET)

# Add definitions for the build target
if (WAMR_BUILD_TARGET STREQUAL "X86_64")
  add_definitions(-DBUILD_TARGET_X86_64)
elseif (WAMR_BUILD_TARGET STREQUAL "AMD_64")
  add_definitions(-DBUILD_TARGET_AMD_64)
elseif (WAMR_BUILD_TARGET STREQUAL "X86_32")
  add_definitions(-DBUILD_TARGET_X86_32)
elseif (WAMR_BUILD_TARGET MATCHES "ARM.*")
  if (WAMR_BUILD_TARGET MATCHES "(ARM.*)_VFP")
    add_definitions(-DBUILD_TARGET_ARM_VFP)
    add_definitions(-DBUILD_TARGET="${CMAKE_MATCH_1}")
  else ()
    add_definitions(-DBUILD_TARGET_ARM)
    add_definitions(-DBUILD_TARGET="${WAMR_BUILD_TARGET}")
  endif ()
elseif (WAMR_BUILD_TARGET MATCHES "THUMB.*")
  if (WAMR_BUILD_TARGET MATCHES "(THUMB.*)_VFP")
    add_definitions(-DBUILD_TARGET_THUMB_VFP)
    add_definitions(-DBUILD_TARGET="${CMAKE_MATCH_1}")
  else ()
    add_definitions(-DBUILD_TARGET_THUMB)
    add_definitions(-DBUILD_TARGET="${WAMR_BUILD_TARGET}")
  endif ()
elseif (WAMR_BUILD_TARGET MATCHES "AARCH64.*")
  add_definitions(-DBUILD_TARGET_AARCH64)
  add_definitions(-DBUILD_TARGET="${WAMR_BUILD_TARGET}")
elseif (WAMR_BUILD_TARGET STREQUAL "MIPS")
  add_definitions(-DBUILD_TARGET_MIPS)
elseif (WAMR_BUILD_TARGET STREQUAL "XTENSA")
  add_definitions(-DBUILD_TARGET_XTENSA)
elseif (WAMR_BUILD_TARGET STREQUAL "RISCV64" OR WAMR_BUILD_TARGET STREQUAL "RISCV64_LP64D")
  add_definitions(-DBUILD_TARGET_RISCV64_LP64D)
elseif (WAMR_BUILD_TARGET STREQUAL "RISCV64_LP64")
  add_definitions(-DBUILD_TARGET_RISCV64_LP64)
elseif (WAMR_BUILD_TARGET STREQUAL "RISCV32" OR WAMR_BUILD_TARGET STREQUAL "RISCV32_ILP32D")
  add_definitions(-DBUILD_TARGET_RISCV32_ILP32D)
elseif (WAMR_BUILD_TARGET STREQUAL "RISCV32_ILP32F")
  add_definitions(-DBUILD_TARGET_RISCV32_ILP32F)
elseif (WAMR_BUILD_TARGET STREQUAL "RISCV32_ILP32")
  add_definitions(-DBUILD_TARGET_RISCV32_ILP32)
elseif (WAMR_BUILD_TARGET STREQUAL "ARC")
  add_definitions(-DBUILD_TARGET_ARC)
else ()
  message (FATAL_ERROR "-- WAMR build target isn't set")
endif ()

if (CMAKE_BUILD_TYPE STREQUAL "Debug")
  add_definitions(-DBH_DEBUG=1)
endif ()

if (CMAKE_SIZEOF_VOID_P EQUAL 8)
  if (WAMR_BUILD_TARGET STREQUAL "X86_64" OR WAMR_BUILD_TARGET STREQUAL "AMD_64"
      OR WAMR_BUILD_TARGET MATCHES "AARCH64.*" OR WAMR_BUILD_TARGET MATCHES "RISCV64.*")
    if (NOT WAMR_BUILD_PLATFORM STREQUAL "windows")
      # Add -fPIC flag if build as 64-bit
      set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fPIC")
      set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fPIC")
      set (CMAKE_SHARED_LIBRARY_LINK_C_FLAGS "${CMAKE_SHARED_LIBRARY_LINK_C_FLAGS} -fPIC")
      set (CMAKE_SHARED_LIBRARY_LINK_CXX_FLAGS "${CMAKE_SHARED_LIBRARY_LINK_CXX_FLAGS} -fPIC")
    endif ()
  else ()
    include(CheckCCompilerFlag)
    Check_C_Compiler_Flag(-m32 M32_OK)
    if (M32_OK OR WAMR_BUILD_TARGET STREQUAL "X86_32")
      add_definitions (-m32)
      set (CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -m32")
      set (CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -m32")
    endif ()
  endif ()
endif ()

if (WAMR_BUILD_TARGET MATCHES "ARM.*")
  set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -marm")
elseif (WAMR_BUILD_TARGET MATCHES "THUMB.*")
  set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -mthumb")
  set (CMAKE_ASM_FLAGS "${CMAKE_ASM_FLAGS} -Wa,-mthumb")
endif ()

if (NOT WAMR_BUILD_INTERP EQUAL 1)
if (NOT WAMR_BUILD_AOT EQUAL 1)
  message (FATAL_ERROR "-- WAMR Interpreter and AOT must be enabled at least one")
endif ()
endif ()

if (WAMR_BUILD_FAST_JIT EQUAL 1)
  if (NOT WAMR_BUILD_LAZY_JIT EQUAL 0)
    # Enable Lazy JIT by default
    set (WAMR_BUILD_LAZY_JIT 1)
  endif ()
endif ()

if (WAMR_BUILD_JIT EQUAL 1)
  if (NOT WAMR_BUILD_LAZY_JIT EQUAL 0)
    # Enable Lazy JIT by default
    set (WAMR_BUILD_LAZY_JIT 1)
  endif ()
  if (NOT DEFINED LLVM_DIR)
    set (LLVM_SRC_ROOT "${WAMR_ROOT_DIR}/core/deps/llvm")
    set (LLVM_BUILD_ROOT "${LLVM_SRC_ROOT}/build")
    if (NOT EXISTS "${LLVM_BUILD_ROOT}")
        message (FATAL_ERROR "Cannot find LLVM dir: ${LLVM_BUILD_ROOT}")
    endif ()
    set (CMAKE_PREFIX_PATH "${LLVM_BUILD_ROOT};${CMAKE_PREFIX_PATH}")
    set (LLVM_DIR ${LLVM_BUILD_ROOT}/lib/cmake/llvm)
  endif ()
  find_package(LLVM REQUIRED CONFIG)
  include_directories(${LLVM_INCLUDE_DIRS})
  add_definitions(${LLVM_DEFINITIONS})
  message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
  message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

  # Disable -Wredundant-move when building LLVM JIT
  include(CheckCXXCompilerFlag)
  if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    check_cxx_compiler_flag("-Wredundant-move" CXX_SUPPORTS_REDUNDANT_MOVE_FLAG)
    if (CXX_SUPPORTS_REDUNDANT_MOVE_FLAG)
      set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-redundant-move")
    endif ()
    # Enable exporting symbols after llvm-17, or LLVM JIT may run failed
    # with `llvm_orc_registerEHFrameSectionWrapper` symbol not found error
    if (${LLVM_PACKAGE_VERSION} VERSION_GREATER_EQUAL "17.0.0")
      set (CMAKE_ENABLE_EXPORTS 1)
    endif ()
  endif ()
else ()
  unset (LLVM_AVAILABLE_LIBS)
endif ()

# Version
include (${WAMR_ROOT_DIR}/build-scripts/version.cmake)

# Package
include (${WAMR_ROOT_DIR}/build-scripts/package.cmake)

# Sanitizers

if (NOT DEFINED WAMR_BUILD_SANITIZER)
  set(WAMR_BUILD_SANITIZER $ENV{WAMR_BUILD_SANITIZER})
endif ()

if (NOT DEFINED WAMR_BUILD_SANITIZER)
  set(WAMR_BUILD_SANITIZER "")
elseif (WAMR_BUILD_SANITIZER STREQUAL "ubsan")
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g -O0 -fno-omit-frame-pointer -fsanitize=undefined -fno-sanitize-recover=all -fno-sanitize=alignment" )
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=undefined")
elseif (WAMR_BUILD_SANITIZER STREQUAL "asan")
  if (NOT WAMR_BUILD_JIT EQUAL 1)
    set (ASAN_OPTIONS "verbosity=2 debug=true ")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g -O0 -fno-omit-frame-pointer -fsanitize=address -fno-sanitize-recover=all" )
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=address")
  endif()
elseif (WAMR_BUILD_SANITIZER STREQUAL "tsan")
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g -O0 -fno-omit-frame-pointer -fsanitize=thread -fno-sanitize-recover=all" )
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=thread")
elseif (WAMR_BUILD_SANITIZER STREQUAL "posan")
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g -O0 -fno-omit-frame-pointer -fsanitize=pointer-overflow -fno-sanitize-recover=all" )
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=pointer-overflow")
elseif (NOT (WAMR_BUILD_SANITIZER STREQUAL "") )
  message(SEND_ERROR "Unsupported sanitizer: ${WAMR_BUILD_SANITIZER}")
endif()

if (WAMR_BUILD_LINUX_PERF EQUAL 1)
  if (NOT WAMR_BUILD_JIT AND NOT WAMR_BUILD_AOT)
    message(WARNING "only support perf in aot and llvm-jit")
    set(WAMR_BUILD_LINUX_PERF 0)
  endif ()
endif ()

if (NOT DEFINED WAMR_BUILD_SHRUNK_MEMORY)
  # Enable shrunk memory by default
  set (WAMR_BUILD_SHRUNK_MEMORY 1)
endif ()

########################################
# Default values
########################################
if (NOT DEFINED WAMR_BUILD_BULK_MEMORY)
  set (WAMR_BUILD_BULK_MEMORY 1)
endif ()

if (NOT DEFINED WAMR_BUILD_EXCE_HANDLING)
  set (WAMR_BUILD_EXCE_HANDLING 0)
endif ()

if (NOT DEFINED WAMR_BUILD_GC)
  set (WAMR_BUILD_GC 0)
endif ()

if (NOT DEFINED WAMR_BUILD_MEMORY64)
  set (WAMR_BUILD_MEMORY64 0)
endif ()

if (NOT DEFINED WAMR_BUILD_MULTI_MEMORY)
  set (WAMR_BUILD_MULTI_MEMORY 0)
endif ()

if (NOT DEFINED WAMR_BUILD_SHARED_MEMORY)
  set(WAMR_BUILD_SHARED_MEMORY 0)
endif ()

if (NOT DEFINED WAMR_BUILD_STRINGREF)
  set(WAMR_BUILD_STRINGREF 0)
endif ()

if (NOT DEFINED WAMR_BUILD_TAIL_CALL)
  set (WAMR_BUILD_TAIL_CALL 0)
endif ()

if (NOT DEFINED WAMR_BUILD_EXTENDED_CONST_EXPR)
  set (WAMR_BUILD_EXTENDED_CONST_EXPR 0)
endif ()

########################################
# Compilation options to marco
########################################

message ("-- Build Configurations:")
message ("     Build as target ${WAMR_BUILD_TARGET}")
message ("     Build for platform ${WAMR_BUILD_PLATFORM}")
message ("     CMAKE_BUILD_TYPE " ${CMAKE_BUILD_TYPE})
message ("     BUILD_SHARED_LIBS " ${BUILD_SHARED_LIBS})
################## running mode ##################
if (WAMR_BUILD_INTERP EQUAL 1)
  message ("     WAMR Interpreter enabled")
else ()
  message ("     WAMR Interpreter disabled")
endif ()
if ((WAMR_BUILD_FAST_INTERP EQUAL 1) AND (WAMR_BUILD_INTERP EQUAL 1))
  add_definitions (-DWASM_ENABLE_FAST_INTERP=1)
  message ("     Fast interpreter enabled")
else ()
  add_definitions (-DWASM_ENABLE_FAST_INTERP=0)
  message ("     Fast interpreter disabled")
endif ()
if (WAMR_BUILD_AOT EQUAL 1)
  message ("     WAMR AOT enabled")
else ()
  message ("     WAMR AOT disabled")
endif ()
if (WAMR_BUILD_FAST_JIT EQUAL 1)
  if (WAMR_BUILD_LAZY_JIT EQUAL 1)
    add_definitions("-DWASM_ENABLE_LAZY_JIT=1")
    message ("     WAMR Fast JIT enabled with Lazy Compilation")
  else ()
    message ("     WAMR Fast JIT enabled with Eager Compilation")
  endif ()
else ()
  message ("     WAMR Fast JIT disabled")
endif ()
if (WAMR_BUILD_JIT EQUAL 1)
  add_definitions("-DWASM_ENABLE_JIT=1")
  if (WAMR_BUILD_LAZY_JIT EQUAL 1)
    add_definitions("-DWASM_ENABLE_LAZY_JIT=1")
    message ("     WAMR LLVM ORC JIT enabled with Lazy Compilation")
  else ()
    message ("     WAMR LLVM ORC JIT enabled with Eager Compilation")
  endif ()
else ()
  message ("     WAMR LLVM ORC JIT disabled")
endif ()
if (WAMR_BUILD_FAST_JIT EQUAL 1 AND WAMR_BUILD_JIT EQUAL 1
    AND WAMR_BUILD_LAZY_JIT EQUAL 1)
  message ("     Multi-tier JIT enabled")
endif ()
################## test modes ##################
if (WAMR_BUILD_SPEC_TEST EQUAL 1)
  add_definitions (-DWASM_ENABLE_SPEC_TEST=1)
  message ("     spec test compatible mode is on")
endif ()
if (WAMR_BUILD_WASI_TEST EQUAL 1)
  add_definitions (-DWASM_ENABLE_WASI_TEST=1)
  message ("     wasi test compatible mode is on")
endif ()
################## native ##################
if (WAMR_BUILD_LIBC_BUILTIN EQUAL 1)
  message ("     Libc builtin enabled")
else ()
  message ("     Libc builtin disabled")
endif ()
if (WAMR_BUILD_LIBC_UVWASI EQUAL 1)
  message ("     Libc WASI enabled with uvwasi implementation\n"
           "     WANRING:: uvwasi does not currently provide the comprehensive\n"
           "     file system security properties provided by some WASI runtimes.\n"
           "     Full support for secure file system sandboxing may or may not\n"
           "     be implemented in future. In the mean time, DO NOT RELY ON IT\n"
           "     TO RUN UNTRUSTED CODE.")
elseif (WAMR_BUILD_LIBC_WASI EQUAL 1)
  message ("     Libc WASI enabled")
else ()
  message ("     Libc WASI disabled")
endif ()
if (WAMR_BUILD_THREAD_MGR EQUAL 1)
  message ("     Thread manager enabled")
endif ()
if (WAMR_BUILD_LIB_PTHREAD EQUAL 1)
  message ("     Lib pthread enabled")
endif ()
if (WAMR_BUILD_LIB_PTHREAD_SEMAPHORE EQUAL 1)
  message ("     Lib pthread semaphore enabled")
endif ()
if (WAMR_BUILD_LIB_WASI_THREADS EQUAL 1)
  message ("     Lib wasi-threads enabled")
endif ()
if (WAMR_BUILD_LIBC_EMCC EQUAL 1)
  message ("     Libc emcc enabled")
endif ()
if (WAMR_BUILD_LIB_RATS EQUAL 1)
  message ("     Lib rats enabled")
endif()
if ((WAMR_BUILD_LIB_SIMDE EQUAL 1))
  message ("     Lib simde enabled")
endif()
################## WAMR features ##################
if (WAMR_BUILD_MULTI_MODULE EQUAL 1)
  add_definitions (-DWASM_ENABLE_MULTI_MODULE=1)
  message ("     Multiple modules enabled")
else ()
  add_definitions (-DWASM_ENABLE_MULTI_MODULE=0)
  message ("     Multiple modules disabled")
endif ()
if (WAMR_BUILD_BULK_MEMORY EQUAL 1)
  add_definitions (-DWASM_ENABLE_BULK_MEMORY=1)
else ()
  add_definitions (-DWASM_ENABLE_BULK_MEMORY=0)
endif ()
if (WAMR_BUILD_SHARED_MEMORY EQUAL 1)
  add_definitions (-DWASM_ENABLE_SHARED_MEMORY=1)
  message ("     Shared memory enabled")
else ()
  add_definitions (-DWASM_ENABLE_SHARED_MEMORY=0)
endif ()
if (WAMR_BUILD_SHARED_HEAP EQUAL 1)
  add_definitions (-DWASM_ENABLE_SHARED_HEAP=1)
  message ("     Shared heap enabled")
endif()
if (WAMR_BUILD_COPY_CALL_STACK EQUAL 1)
  add_definitions (-DWASM_ENABLE_COPY_CALL_STACK=1)
  message("     Copy callstack enabled")
endif()
if (WAMR_BUILD_MEMORY64 EQUAL 1)
  # if native is 32-bit or cross-compiled to 32-bit
  if (NOT WAMR_BUILD_TARGET MATCHES ".*64.*")
    message (FATAL_ERROR "-- Memory64 is only available on the 64-bit platform/target")
  endif()
  add_definitions (-DWASM_ENABLE_MEMORY64=1)
  set (WAMR_DISABLE_HW_BOUND_CHECK 1)
endif ()
if (WAMR_BUILD_MULTI_MEMORY EQUAL 1)
  add_definitions (-DWASM_ENABLE_MULTI_MEMORY=1)
  set (WAMR_BUILD_DEBUG_INTERP 0)
endif ()
if (WAMR_BUILD_MINI_LOADER EQUAL 1)
  add_definitions (-DWASM_ENABLE_MINI_LOADER=1)
  message ("     WASM mini loader enabled")
else ()
  add_definitions (-DWASM_ENABLE_MINI_LOADER=0)
endif ()
if (WAMR_DISABLE_HW_BOUND_CHECK EQUAL 1)
  add_definitions (-DWASM_DISABLE_HW_BOUND_CHECK=1)
  add_definitions (-DWASM_DISABLE_STACK_HW_BOUND_CHECK=1)
  message ("     Hardware boundary check disabled")
else ()
  add_definitions (-DWASM_DISABLE_HW_BOUND_CHECK=0)
  if (WAMR_DISABLE_STACK_HW_BOUND_CHECK EQUAL 1)
    add_definitions (-DWASM_DISABLE_STACK_HW_BOUND_CHECK=1)
    message ("     Hardware boundary check for native stack disabled")
  else ()
    add_definitions (-DWASM_DISABLE_STACK_HW_BOUND_CHECK=0)
  endif ()
endif ()
if (WAMR_DISABLE_WAKEUP_BLOCKING_OP EQUAL 1)
  add_definitions (-DWASM_DISABLE_WAKEUP_BLOCKING_OP=1)
  message ("     Wakeup of blocking operations disabled")
else ()
  add_definitions (-DWASM_DISABLE_WAKEUP_BLOCKING_OP=0)
  message ("     Wakeup of blocking operations enabled")
endif ()
if (WAMR_BUILD_SIMD EQUAL 1)
  if (WAMR_BUILD_TARGET MATCHES "RISCV64.*")
    set(SIMD_ENABLED 0)
    message ("     SIMD disabled due to not supported on target RISCV64")
  else()
    set(SIMD_ENABLED 1)
    message ("     SIMD enabled")
  endif ()
  add_definitions(-DWASM_ENABLE_SIMD=${SIMD_ENABLED})
endif ()
if (WAMR_BUILD_AOT_STACK_FRAME EQUAL 1)
  add_definitions (-DWASM_ENABLE_AOT_STACK_FRAME=1)
  message ("     AOT stack frame enabled")
endif ()
if (WAMR_BUILD_MEMORY_PROFILING EQUAL 1)
  add_definitions (-DWASM_ENABLE_MEMORY_PROFILING=1)
  message ("     Memory profiling enabled")
endif ()
if (WAMR_BUILD_PERF_PROFILING EQUAL 1)
  add_definitions (-DWASM_ENABLE_PERF_PROFILING=1)
  message ("     Performance profiling enabled")
endif ()
if (DEFINED WAMR_APP_THREAD_STACK_SIZE_MAX)
  add_definitions (-DAPP_THREAD_STACK_SIZE_MAX=${WAMR_APP_THREAD_STACK_SIZE_MAX})
endif ()
if (WAMR_BUILD_CUSTOM_NAME_SECTION EQUAL 1)
  add_definitions (-DWASM_ENABLE_CUSTOM_NAME_SECTION=1)
  message ("     Custom name section enabled")
endif ()
if (WAMR_BUILD_DUMP_CALL_STACK EQUAL 1)
  add_definitions (-DWASM_ENABLE_DUMP_CALL_STACK=1)
  message ("     Dump call stack enabled")
endif ()
if (WAMR_BUILD_TAIL_CALL EQUAL 1)
  add_definitions (-DWASM_ENABLE_TAIL_CALL=1)
endif ()
if (WAMR_BUILD_REF_TYPES EQUAL 1)
  add_definitions (-DWASM_ENABLE_REF_TYPES=1)
endif ()
if (WAMR_BUILD_GC EQUAL 1)
  if (WAMR_TEST_GC EQUAL 1)
    message("      GC testing enabled")
  endif()
endif ()
if (WAMR_BUILD_GC EQUAL 1 AND WAMR_BUILD_GC_PERF_PROFILING EQUAL 1)
  add_definitions (-DWASM_ENABLE_GC_PERF_PROFILING=1)
  message ("     GC performance profiling enabled")
else ()
  message ("     GC performance profiling disabled")
endif ()
if (WAMR_BUILD_STRINGREF EQUAL 1)
  if (NOT DEFINED WAMR_STRINGREF_IMPL_SOURCE)
    message ("       Using WAMR builtin implementation for stringref")
  else ()
    message ("       Using custom implementation for stringref")
  endif()
endif ()
if (WAMR_BUILD_PERF_PROFILING EQUAL 1 OR
    WAMR_BUILD_DUMP_CALL_STACK EQUAL 1 OR
    WAMR_BUILD_GC EQUAL 1)
  # Enable AOT/JIT stack frame when perf-profiling, dump-call-stack
  # or GC is enabled
  if (WAMR_BUILD_AOT EQUAL 1 OR WAMR_BUILD_JIT EQUAL 1)
    add_definitions (-DWASM_ENABLE_AOT_STACK_FRAME=1)
  endif ()
endif ()
if (WAMR_BUILD_EXCE_HANDLING EQUAL 1)
  add_definitions (-DWASM_ENABLE_EXCE_HANDLING=1)
  add_definitions (-DWASM_ENABLE_TAGS=1)
  message ("     Exception Handling enabled")
endif ()
if (DEFINED WAMR_BH_VPRINTF)
  add_definitions (-DBH_VPRINTF=${WAMR_BH_VPRINTF})
endif ()
if (DEFINED WAMR_BH_LOG)
  add_definitions (-DBH_LOG=${WAMR_BH_LOG})
endif ()
if (WAMR_DISABLE_APP_ENTRY EQUAL 1)
  message ("     WAMR application entry functions excluded")
endif ()
if (WAMR_BUILD_DEBUG_INTERP EQUAL 1)
    message ("     Debug Interpreter enabled")
endif ()
if (WAMR_BUILD_DEBUG_AOT EQUAL 1)
    message ("     Debug AOT enabled")
endif ()
if (WAMR_BUILD_DYNAMIC_AOT_DEBUG EQUAL 1)
    add_definitions (-DWASM_ENABLE_DYNAMIC_AOT_DEBUG=1)
    message ("     Dynamic AOT debug enabled")
endif ()
if (WAMR_BUILD_LOAD_CUSTOM_SECTION EQUAL 1)
    add_definitions (-DWASM_ENABLE_LOAD_CUSTOM_SECTION=1)
    message ("     Load custom section enabled")
endif ()
if (WAMR_BUILD_GLOBAL_HEAP_POOL EQUAL 1)
  add_definitions(-DWASM_ENABLE_GLOBAL_HEAP_POOL=1)
  message ("     Global heap pool enabled")
endif ()
if (WAMR_BUILD_GLOBAL_HEAP_SIZE GREATER 0)
  add_definitions (-DWASM_GLOBAL_HEAP_SIZE=${WAMR_BUILD_GLOBAL_HEAP_SIZE})
  message ("     Custom global heap size: " ${WAMR_BUILD_GLOBAL_HEAP_SIZE})
else ()
  # Spec test requires more heap pool size
  if (WAMR_BUILD_SPEC_TEST EQUAL 1)
    if (WAMR_BUILD_PLATFORM STREQUAL "linux-sgx")
      math(EXPR WAMR_BUILD_GLOBAL_HEAP_SIZE "100 * 1024 * 1024")
    else ()
      math(EXPR WAMR_BUILD_GLOBAL_HEAP_SIZE "300 * 1024 * 1024")
    endif ()
    add_definitions (-DWASM_GLOBAL_HEAP_SIZE=${WAMR_BUILD_GLOBAL_HEAP_SIZE})
  else ()
    # By default, the global heap size is of 10 MB
    math(EXPR WAMR_BUILD_GLOBAL_HEAP_SIZE "10 * 1024 * 1024")
    add_definitions (-DWASM_GLOBAL_HEAP_SIZE=${WAMR_BUILD_GLOBAL_HEAP_SIZE})
  endif ()
endif ()
if (WAMR_BUILD_STACK_GUARD_SIZE GREATER 0)
    add_definitions (-DWASM_STACK_GUARD_SIZE=${WAMR_BUILD_STACK_GUARD_SIZE})
    message ("     Custom stack guard size: " ${WAMR_BUILD_STACK_GUARD_SIZE})
endif ()
if (WAMR_BUILD_SGX_IPFS EQUAL 1)
    add_definitions (-DWASM_ENABLE_SGX_IPFS=1)
    message ("     SGX IPFS enabled")
endif ()
if (WAMR_BUILD_WASI_NN EQUAL 1)
  message ("     WASI-NN enabled")
  add_definitions (-DWASM_ENABLE_WASI_NN=1)
  # Variant backends
  if (NOT WAMR_BUILD_WASI_NN_TFLITE EQUAL 1 AND
      NOT WAMR_BUILD_WASI_NN_OPENVINO EQUAL 1 AND
      NOT WAMR_BUILD_WASI_NN_LLAMACPP EQUAL 1)
    message (FATAL_ERROR "   Need to select a backend for WASI-NN")
  endif ()

  if (WAMR_BUILD_WASI_NN_TFLITE EQUAL 1)
    message ("     WASI-NN: backend tflite enabled")
    add_definitions (-DWASM_ENABLE_WASI_NN_TFLITE)
  endif ()
  if (WAMR_BUILD_WASI_NN_OPENVINO EQUAL 1)
    message ("     WASI-NN: backend openvino enabled")
    add_definitions (-DWASM_ENABLE_WASI_NN_OPENVINO)
  endif ()
  if (WAMR_BUILD_WASI_NN_LLAMACPP EQUAL 1)
    message ("     WASI-NN: backend llamacpp enabled")
    add_definitions (-DWASM_ENABLE_WASI_NN_LLAMACPP)
  endif ()
  # Variant devices
  if (WAMR_BUILD_WASI_NN_ENABLE_GPU EQUAL 1)
      message ("     WASI-NN: GPU enabled")
      add_definitions (-DWASM_ENABLE_WASI_NN_GPU=1)
  endif ()
  if (WAMR_BUILD_WASI_NN_ENABLE_EXTERNAL_DELEGATE EQUAL 1)
      message ("     WASI-NN: External Delegation enabled")
      add_definitions (-DWASM_ENABLE_WASI_NN_EXTERNAL_DELEGATE=1)
  endif ()
  if (DEFINED WAMR_BUILD_WASI_NN_EXTERNAL_DELEGATE_PATH)
      add_definitions (-DWASM_WASI_NN_EXTERNAL_DELEGATE_PATH="${WAMR_BUILD_WASI_NN_EXTERNAL_DELEGATE_PATH}")
  endif ()
  if (NOT DEFINED WAMR_BUILD_WASI_EPHEMERAL_NN)
      set(WAMR_BUILD_WASI_EPHEMERAL_NN 1)
  endif()
  if (WAMR_BUILD_WASI_EPHEMERAL_NN EQUAL 1)
      message ("     WASI-NN: use 'wasi_ephemeral_nn' instead of 'wasi-nn'")
      add_definitions (-DWASM_ENABLE_WASI_EPHEMERAL_NN=1)
  endif()
endif ()
if (WAMR_BUILD_ALLOC_WITH_USER_DATA EQUAL 1)
  add_definitions(-DWASM_MEM_ALLOC_WITH_USER_DATA=1)
endif()
if (WAMR_BUILD_WASM_CACHE EQUAL 1)
  add_definitions (-DWASM_ENABLE_WASM_CACHE=1)
  message ("     Wasm files cache enabled")
endif ()
if (WAMR_BUILD_MODULE_INST_CONTEXT EQUAL 1)
  add_definitions (-DWASM_ENABLE_MODULE_INST_CONTEXT=1)
  message ("     Module instance context enabled")
endif ()
if (WAMR_BUILD_GC_HEAP_VERIFY EQUAL 1)
  add_definitions (-DBH_ENABLE_GC_VERIFY=1)
  message ("     GC heap verification enabled")
endif ()
if ("$ENV{COLLECT_CODE_COVERAGE}" STREQUAL "1" OR COLLECT_CODE_COVERAGE EQUAL 1)
  set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fprofile-arcs -ftest-coverage")
  set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fprofile-arcs -ftest-coverage")
  add_definitions (-DCOLLECT_CODE_COVERAGE)
  message ("     Collect code coverage enabled")
endif ()
if (WAMR_BUILD_STATIC_PGO EQUAL 1)
  add_definitions (-DWASM_ENABLE_STATIC_PGO=1)
  message ("     AOT static PGO enabled")
endif ()
if (WAMR_BUILD_TARGET STREQUAL "X86_64"
    AND WAMR_BUILD_PLATFORM STREQUAL "linux")
  if (WAMR_DISABLE_WRITE_GS_BASE EQUAL 1)
    # disabled by user
    set (DISABLE_WRITE_GS_BASE 1)
  elseif (WAMR_DISABLE_WRITE_GS_BASE EQUAL 0)
    # enabled by user
    set (DISABLE_WRITE_GS_BASE 0)
  elseif (CMAKE_CROSSCOMPILING)
    # disabled in cross compilation environment
    set (DISABLE_WRITE_GS_BASE 1)
  else ()
    # auto-detected by the compiler
    set (TEST_WRGSBASE_SOURCE "${CMAKE_BINARY_DIR}/test_wrgsbase.c")
    file (WRITE "${TEST_WRGSBASE_SOURCE}" "
    #include <stdio.h>
    #include <stdint.h>
    int main() {
        uint64_t value;
        asm volatile (\"wrgsbase %0\" : : \"r\"(value));
        printf(\"WRGSBASE instruction is available.\\n\");
        return 0;
    }")
    # Try to compile and run the test program
    try_run (TEST_WRGSBASE_RESULT
      TEST_WRGSBASE_COMPILED
      ${CMAKE_BINARY_DIR}/test_wrgsbase
      SOURCES ${TEST_WRGSBASE_SOURCE}
      CMAKE_FLAGS -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
    )
    #message("${TEST_WRGSBASE_COMPILED}, ${TEST_WRGSBASE_RESULT}")
    if (TEST_WRGSBASE_RESULT EQUAL 0)
      set (DISABLE_WRITE_GS_BASE 0)
    else ()
      set (DISABLE_WRITE_GS_BASE 1)
    endif ()
  endif ()
  if (DISABLE_WRITE_GS_BASE EQUAL 1)
    add_definitions (-DWASM_DISABLE_WRITE_GS_BASE=1)
    message ("     Write linear memory base addr to x86 GS register disabled")
  else ()
    add_definitions (-DWASM_DISABLE_WRITE_GS_BASE=0)
    message ("     Write linear memory base addr to x86 GS register enabled")
  endif ()
endif ()
if (WAMR_CONFIGURABLE_BOUNDS_CHECKS EQUAL 1)
  add_definitions (-DWASM_CONFIGURABLE_BOUNDS_CHECKS=1)
  message ("     Configurable bounds checks enabled")
endif ()
if (WAMR_BUILD_LINUX_PERF EQUAL 1)
  add_definitions (-DWASM_ENABLE_LINUX_PERF=1)
  message ("     Linux perf support enabled")
endif ()
if (WAMR_BUILD_AOT EQUAL 1 OR WAMR_BUILD_JIT EQUAL 1)
  if (NOT DEFINED WAMR_BUILD_QUICK_AOT_ENTRY)
    # Enable quick aot/jit entries by default
    set (WAMR_BUILD_QUICK_AOT_ENTRY 1)
  endif ()
  if (WAMR_BUILD_QUICK_AOT_ENTRY EQUAL 1)
    add_definitions (-DWASM_ENABLE_QUICK_AOT_ENTRY=1)
    message ("     Quick AOT/JIT entries enabled")
  else ()
    add_definitions (-DWASM_ENABLE_QUICK_AOT_ENTRY=0)
    message ("     Quick AOT/JIT entries disabled")
  endif ()
else ()
  # Disable quick aot/jit entries for interp and fast-jit
  add_definitions (-DWASM_ENABLE_QUICK_AOT_ENTRY=0)
endif ()
if (WAMR_BUILD_AOT EQUAL 1)
  if (NOT DEFINED WAMR_BUILD_AOT_INTRINSICS)
    # Enable aot intrinsics by default
    set (WAMR_BUILD_AOT_INTRINSICS 1)
  endif ()
  if (WAMR_BUILD_AOT_INTRINSICS EQUAL 1)
    add_definitions (-DWASM_ENABLE_AOT_INTRINSICS=1)
    message ("     AOT intrinsics enabled")
  else ()
    add_definitions (-DWASM_ENABLE_AOT_INTRINSICS=0)
    message ("     AOT intrinsics disabled")
  endif ()
else ()
  # Disable aot intrinsics for interp, fast-jit and llvm-jit
  add_definitions (-DWASM_ENABLE_AOT_INTRINSICS=0)
endif ()
if (WAMR_BUILD_ALLOC_WITH_USAGE EQUAL 1)
  add_definitions(-DWASM_MEM_ALLOC_WITH_USAGE=1)
endif()
if (NOT WAMR_BUILD_SANITIZER STREQUAL "")
  message ("     Sanitizer ${WAMR_BUILD_SANITIZER} enabled")
endif ()
if (WAMR_BUILD_SHRUNK_MEMORY EQUAL 1)
  add_definitions (-DWASM_ENABLE_SHRUNK_MEMORY=1)
  message ("     Shrunk memory enabled")
else ()
  add_definitions (-DWASM_ENABLE_SHRUNK_MEMORY=0)
  message ("     Shrunk memory disabled")
endif()
if (WAMR_BUILD_AOT_VALIDATOR EQUAL 1)
  message ("     AOT validator enabled")
  add_definitions (-DWASM_ENABLE_AOT_VALIDATOR=1)
endif ()
if (WAMR_BUILD_INSTRUCTION_METERING EQUAL 1)
  message ("     Instruction metering enabled")
  add_definitions (-DWASM_ENABLE_INSTRUCTION_METERING=1)
endif ()
if (WAMR_BUILD_EXTENDED_CONST_EXPR EQUAL 1)
  message ("     Extended constant expression enabled")
  add_definitions(-DWASM_ENABLE_EXTENDED_CONST_EXPR=1)
else()
  message ("     Extended constant expression disabled")
  add_definitions(-DWASM_ENABLE_EXTENDED_CONST_EXPR=0)
endif ()
########################################
# Show Phase4 Wasm proposals status.
########################################

message (
"-- About Wasm Proposals:\n"
"     Always-on:\n"
"       \"Multi-value\"\n"
"       \"Non-trapping float-to-int conversions\"\n"
"       \"Sign-extension operators\"\n"
"       \"WebAssembly C and C++ API\"\n"
"     Configurable. 0 is OFF. 1 is ON:\n"
"       \"Bulk Memory Operation\" via WAMR_BUILD_BULK_MEMORY: ${WAMR_BUILD_BULK_MEMORY}\n"
"       \"Extended Constant Expressions\" via WAMR_BUILD_EXTENDED_CONST_EXPR: ${WAMR_BUILD_EXTENDED_CONST_EXPR}\n"
"       \"Fixed-width SIMD\" via WAMR_BUILD_SIMD: ${WAMR_BUILD_SIMD}\n"
"       \"Garbage collection\" via WAMR_BUILD_GC: ${WAMR_BUILD_GC}\n"
"       \"Legacy Exception handling\" via WAMR_BUILD_EXCE_HANDLING: ${WAMR_BUILD_EXCE_HANDLING}\n"
"       \"Memory64\" via WAMR_BUILD_MEMORY64: ${WAMR_BUILD_MEMORY64}\n"
"       \"Multiple memories\" via WAMR_BUILD_MULTI_MEMORY: ${WAMR_BUILD_MULTI_MEMORY}\n"
"       \"Reference Types\" via WAMR_BUILD_REF_TYPES: ${WAMR_BUILD_REF_TYPES}\n"
"       \"Reference-Typed Strings\" via WAMR_BUILD_STRINGREF: ${WAMR_BUILD_STRINGREF}\n"
"       \"Tail call\" via WAMR_BUILD_TAIL_CALL: ${WAMR_BUILD_TAIL_CALL}\n"
"       \"Threads\" via WAMR_BUILD_SHARED_MEMORY: ${WAMR_BUILD_SHARED_MEMORY}\n"
"       \"Typed Function References\" via WAMR_BUILD_GC: ${WAMR_BUILD_GC}\n"
"     Unsupported (>= Phase4):\n"
"       \"Branch Hinting\"\n"
"       \"Custom Annotation Syntax in the Text Format\"\n"
"       \"Exception handling\"\n"
"       \"Import/Export of Mutable Globals\"\n"
"       \"JS String Builtins\"\n"
"       \"Relaxed SIMD\"\n"
)
