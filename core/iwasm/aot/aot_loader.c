/*
 * Copyright (C) 2019 Intel Corporation. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include "aot_runtime.h"
#include "aot_reloc.h"
#include "bh_platform.h"
#include "../common/wasm_runtime_common.h"
#include "../common/wasm_native.h"
#include "../common/wasm_loader_common.h"
#include "../compilation/aot.h"
#if WASM_ENABLE_AOT_VALIDATOR != 0
#include "aot_validator.h"
#endif

#if WASM_ENABLE_DEBUG_AOT != 0
#include "debug/elf_parser.h"
#include "debug/jit_debug.h"
#endif

#if WASM_ENABLE_LINUX_PERF != 0
#include "aot_perf_map.h"
#endif

#define YMM_PLT_PREFIX "__ymm@"
#define XMM_PLT_PREFIX "__xmm@"
#define REAL_PLT_PREFIX "__real@"

static void
set_error_buf(char *error_buf, uint32 error_buf_size, const char *string)
{
    if (error_buf != NULL) {
        snprintf(error_buf, error_buf_size, "AOT module load failed: %s",
                 string);
    }
}

static void
set_error_buf_v(char *error_buf, uint32 error_buf_size, const char *format, ...)
{
    va_list args;
    char buf[128];

    if (error_buf != NULL) {
        va_start(args, format);
        vsnprintf(buf, sizeof(buf), format, args);
        va_end(args);
        snprintf(error_buf, error_buf_size, "AOT module load failed: %s", buf);
    }
}

#define exchange_uint8(p_data) (void)0

static void
exchange_uint16(uint8 *p_data)
{
    uint8 value = *p_data;
    *p_data = *(p_data + 1);
    *(p_data + 1) = value;
}

static void
exchange_uint32(uint8 *p_data)
{
    uint8 value = *p_data;
    *p_data = *(p_data + 3);
    *(p_data + 3) = value;

    value = *(p_data + 1);
    *(p_data + 1) = *(p_data + 2);
    *(p_data + 2) = value;
}

static void
exchange_uint64(uint8 *p_data)
{
    uint32 value;

    value = *(uint32 *)p_data;
    *(uint32 *)p_data = *(uint32 *)(p_data + 4);
    *(uint32 *)(p_data + 4) = value;
    exchange_uint32(p_data);
    exchange_uint32(p_data + 4);
}

static union {
    int a;
    char b;
} __ue = { .a = 1 };

#define is_little_endian() (__ue.b == 1)

static bool
check_buf(const uint8 *buf, const uint8 *buf_end, uint32 length,
          char *error_buf, uint32 error_buf_size)
{
    if ((uintptr_t)buf + length < (uintptr_t)buf
        || (uintptr_t)buf + length > (uintptr_t)buf_end) {
        set_error_buf(error_buf, error_buf_size, "unexpected end");
        return false;
    }
    return true;
}

#define CHECK_BUF(buf, buf_end, length)                                    \
    do {                                                                   \
        if (!check_buf(buf, buf_end, length, error_buf, error_buf_size)) { \
            goto fail;                                                     \
        }                                                                  \
    } while (0)

static uint8 *
align_ptr(const uint8 *p, uint32 b)
{
    uintptr_t v = (uintptr_t)p;
    uintptr_t m = b - 1;
    return (uint8 *)((v + m) & ~m);
}

static inline uint64
GET_U64_FROM_ADDR(uint32 *addr)
{
    union {
        uint64 val;
        uint32 parts[2];
    } u;
    u.parts[0] = addr[0];
    u.parts[1] = addr[1];
    return u.val;
}

#if (WASM_ENABLE_WORD_ALIGN_READ != 0)

static inline uint8
GET_U8_FROM_ADDR(const uint8 *p)
{
    uint8 res = 0;
    bh_assert(p);

    const uint8 *p_aligned = align_ptr(p, 4);
    p_aligned = (p_aligned > p) ? p_aligned - 4 : p_aligned;

    uint32 buf32 = *(const uint32 *)p_aligned;
    const uint8 *pbuf = (const uint8 *)&buf32;

    res = *(uint8 *)(pbuf + (p - p_aligned));

    return res;
}

static inline uint16
GET_U16_FROM_ADDR(const uint8 *p)
{
    uint16 res = 0;
    bh_assert(p);

    const uint8 *p_aligned = align_ptr(p, 4);
    p_aligned = (p_aligned > p) ? p_aligned - 4 : p_aligned;

    uint32 buf32 = *(const uint32 *)p_aligned;
    const uint8 *pbuf = (const uint8 *)&buf32;

    res = *(uint16 *)(pbuf + (p - p_aligned));

    return res;
}

#define TEMPLATE_READ(p, p_end, res, type)              \
    do {                                                \
        if (sizeof(type) != sizeof(uint64))             \
            p = (uint8 *)align_ptr(p, sizeof(type));    \
        else                                            \
            /* align 4 bytes if type is uint64 */       \
            p = (uint8 *)align_ptr(p, sizeof(uint32));  \
        CHECK_BUF(p, p_end, sizeof(type));              \
        if (sizeof(type) == sizeof(uint8))              \
            res = GET_U8_FROM_ADDR(p);                  \
        else if (sizeof(type) == sizeof(uint16))        \
            res = GET_U16_FROM_ADDR(p);                 \
        else if (sizeof(type) == sizeof(uint32))        \
            res = *(type *)p;                           \
        else                                            \
            res = (type)GET_U64_FROM_ADDR((uint32 *)p); \
        if (!is_little_endian())                        \
            exchange_##type((uint8 *)&res);             \
        p += sizeof(type);                              \
    } while (0)

#define read_byte_array(p, p_end, addr, len) \
    do {                                     \
        CHECK_BUF(p, p_end, len);            \
        bh_memcpy_wa(addr, len, p, len);     \
        p += len;                            \
    } while (0)

#define read_string(p, p_end, str)                                      \
    do {                                                                \
        if (!(str = load_string((uint8 **)&p, p_end, module,            \
                                is_load_from_file_buf, true, error_buf, \
                                error_buf_size)))                       \
            goto fail;                                                  \
    } while (0)

#else /* else of (WASM_ENABLE_WORD_ALIGN_READ != 0) */

#define TEMPLATE_READ(p, p_end, res, type)              \
    do {                                                \
        if (sizeof(type) != sizeof(uint64))             \
            p = (uint8 *)align_ptr(p, sizeof(type));    \
        else                                            \
            /* align 4 bytes if type is uint64 */       \
            p = (uint8 *)align_ptr(p, sizeof(uint32));  \
        CHECK_BUF(p, p_end, sizeof(type));              \
        if (sizeof(type) != sizeof(uint64))             \
            res = *(type *)p;                           \
        else                                            \
            res = (type)GET_U64_FROM_ADDR((uint32 *)p); \
        if (!is_little_endian())                        \
            exchange_##type((uint8 *)&res);             \
        p += sizeof(type);                              \
    } while (0)

/* NOLINTBEGIN, disable lint for this region with clang-tidy */

#define read_byte_array(p, p_end, addr, len) \
    do {                                     \
        CHECK_BUF(p, p_end, len);            \
        bh_memcpy_s(addr, len, p, len);      \
        p += len;                            \
    } while (0)

#define read_string(p, p_end, str)                                \
    do {                                                          \
        if (!(str = load_string((uint8 **)&p, p_end, module,      \
                                is_load_from_file_buf, error_buf, \
                                error_buf_size)))                 \
            goto fail;                                            \
    } while (0)

#endif /* end of (WASM_ENABLE_WORD_ALIGN_READ != 0) */

#define read_uint8(p, p_end, res) TEMPLATE_READ(p, p_end, res, uint8)
#define read_uint16(p, p_end, res) TEMPLATE_READ(p, p_end, res, uint16)
#define read_uint32(p, p_end, res) TEMPLATE_READ(p, p_end, res, uint32)
#define read_uint64(p, p_end, res) TEMPLATE_READ(p, p_end, res, uint64)

/* NOLINTEND */

/* Legal values for bin_type */
#define BIN_TYPE_ELF32L 0 /* 32-bit little endian */
#define BIN_TYPE_ELF32B 1 /* 32-bit big endian */
#define BIN_TYPE_ELF64L 2 /* 64-bit little endian */
#define BIN_TYPE_ELF64B 3 /* 64-bit big endian */
#define BIN_TYPE_COFF32 4 /* 32-bit little endian */
#define BIN_TYPE_COFF64 6 /* 64-bit little endian */

/* Legal values for e_type (object file type). */
#define E_TYPE_NONE 0 /* No file type */
#define E_TYPE_REL 1  /* Relocatable file */
#define E_TYPE_EXEC 2 /* Executable file */
#define E_TYPE_DYN 3  /* Shared object file */
#define E_TYPE_XIP 4  /* eXecute In Place file */

/* Legal values for e_machine (architecture).  */
#define E_MACHINE_386 3             /* Intel 80386 */
#define E_MACHINE_MIPS 8            /* MIPS R3000 big-endian */
#define E_MACHINE_MIPS_RS3_LE 10    /* MIPS R3000 little-endian */
#define E_MACHINE_ARM 40            /* ARM/Thumb */
#define E_MACHINE_AARCH64 183       /* AArch64 */
#define E_MACHINE_ARC 45            /* Argonaut RISC Core */
#define E_MACHINE_IA_64 50          /* Intel Merced */
#define E_MACHINE_MIPS_X 51         /* Stanford MIPS-X */
#define E_MACHINE_X86_64 62         /* AMD x86-64 architecture */
#define E_MACHINE_ARC_COMPACT 93    /* ARC International ARCompact */
#define E_MACHINE_ARC_COMPACT2 195  /* Synopsys ARCompact V2 */
#define E_MACHINE_XTENSA 94         /* Tensilica Xtensa Architecture */
#define E_MACHINE_RISCV 243         /* RISC-V 32/64 */
#define E_MACHINE_WIN_I386 0x14c    /* Windows i386 architecture */
#define E_MACHINE_WIN_X86_64 0x8664 /* Windows x86-64 architecture */

/* Legal values for e_version */
#define E_VERSION_CURRENT 1 /* Current version */

static void *
loader_malloc(uint64 size, char *error_buf, uint32 error_buf_size)
{
    void *mem;

    if (size >= UINT32_MAX || !(mem = wasm_runtime_malloc((uint32)size))) {
        set_error_buf(error_buf, error_buf_size, "allocate memory failed");
        return NULL;
    }

    memset(mem, 0, (uint32)size);
    return mem;
}

static void *
loader_mmap(uint32 size, bool prot_exec, char *error_buf, uint32 error_buf_size)
{
    int map_prot =
        MMAP_PROT_READ | MMAP_PROT_WRITE | (prot_exec ? MMAP_PROT_EXEC : 0);
    int map_flags;
    void *mem;

#if defined(BUILD_TARGET_X86_64) || defined(BUILD_TARGET_AMD_64) \
    || defined(BUILD_TARGET_RISCV64_LP64D)                       \
    || defined(BUILD_TARGET_RISCV64_LP64)
#if !defined(__APPLE__) && !defined(BH_PLATFORM_LINUX_SGX)
    /* The mmapped AOT data and code in 64-bit targets had better be in
       range 0 to 2G, or aot loader may fail to apply some relocations,
       e.g., R_X86_64_32/R_X86_64_32S/R_X86_64_PC32/R_RISCV_32.
       We try to mmap with MMAP_MAP_32BIT flag first, and if fails, mmap
       again without the flag. */
    /* sgx_tprotect_rsrv_mem() and sgx_alloc_rsrv_mem() will ignore flags */
    map_flags = MMAP_MAP_32BIT;
    if ((mem = os_mmap(NULL, size, map_prot, map_flags,
                       os_get_invalid_handle()))) {
        /* Test whether the mmapped memory in the first 2 Gigabytes of the
           process address space */
        if ((uintptr_t)mem >= INT32_MAX)
            LOG_WARNING(
                "Warning: loader mmap memory address is not in the first 2 "
                "Gigabytes of the process address space.");
        return mem;
    }
#endif
#endif

    map_flags = MMAP_MAP_NONE;
    if (!(mem = os_mmap(NULL, size, map_prot, map_flags,
                        os_get_invalid_handle()))) {
        set_error_buf(error_buf, error_buf_size, "allocate memory failed");
        return NULL;
    }
    return mem;
}

static char *
load_string(uint8 **p_buf, const uint8 *buf_end, AOTModule *module,
            bool is_load_from_file_buf,
#if (WASM_ENABLE_WORD_ALIGN_READ != 0)
            bool is_vram_word_align,
#endif
            char *error_buf, uint32 error_buf_size)
{
    uint8 *p = *p_buf;
    const uint8 *p_end = buf_end;
    char *str;
    uint16 str_len;

    read_uint16(p, p_end, str_len);
    CHECK_BUF(p, p_end, str_len);

    if (str_len == 0) {
        str = "";
    }
#if (WASM_ENABLE_WORD_ALIGN_READ != 0)
    else if (is_vram_word_align) {
        if (!(str = aot_const_str_set_insert((uint8 *)p, str_len, module,
                                             is_vram_word_align, error_buf,
                                             error_buf_size))) {
            goto fail;
        }
    }
#endif
    else if (is_load_from_file_buf) {
        /* The string is always terminated with '\0', use it directly.
         * In this case, the file buffer can be referred to after loading.
         */
        if (p[str_len - 1] != '\0')
            goto fail;

        str = (char *)p;
    }
    else {
        /* Load from sections, the file buffer cannot be referred to
           after loading, we must create another string and insert it
           into const string set */
        if (p[str_len - 1] != '\0')
            goto fail;

        if (!(str = aot_const_str_set_insert((uint8 *)p, str_len, module,
#if (WASM_ENABLE_WORD_ALIGN_READ != 0)
                                             is_vram_word_align,
#endif
                                             error_buf, error_buf_size))) {
            goto fail;
        }
    }
    p += str_len;

    *p_buf = p;
    return str;
fail:
    return NULL;
}

static bool
get_aot_file_target(AOTTargetInfo *target_info, char *target_buf,
                    uint32 target_buf_size, char *error_buf,
                    uint32 error_buf_size)
{
    char *machine_type = NULL;
    switch (target_info->e_machine) {
        case E_MACHINE_X86_64:
        case E_MACHINE_WIN_X86_64:
            machine_type = "x86_64";
            break;
        case E_MACHINE_386:
        case E_MACHINE_WIN_I386:
            machine_type = "i386";
            break;
        case E_MACHINE_ARM:
        case E_MACHINE_AARCH64:
            /* TODO: this will make following `strncmp()` ~L392 unnecessary.
             * Use const strings here */
            machine_type = target_info->arch;
            break;
        case E_MACHINE_MIPS:
            machine_type = "mips";
            break;
        case E_MACHINE_XTENSA:
            machine_type = "xtensa";
            break;
        case E_MACHINE_RISCV:
            machine_type = "riscv";
            break;
        case E_MACHINE_ARC_COMPACT:
        case E_MACHINE_ARC_COMPACT2:
            machine_type = "arc";
            break;
        default:
            set_error_buf_v(error_buf, error_buf_size,
                            "unknown machine type %d", target_info->e_machine);
            return false;
    }
    if (strncmp(target_info->arch, machine_type, strlen(machine_type))) {
        set_error_buf_v(
            error_buf, error_buf_size,
            "machine type (%s) isn't consistent with target type (%s)",
            machine_type, target_info->arch);
        return false;
    }
    snprintf(target_buf, target_buf_size, "%s", target_info->arch);
    return true;
}

static bool
check_machine_info(AOTTargetInfo *target_info, char *error_buf,
                   uint32 error_buf_size)
{
    char target_expected[32], target_got[32];

    get_current_target(target_expected, sizeof(target_expected));

    if (!get_aot_file_target(target_info, target_got, sizeof(target_got),
                             error_buf, error_buf_size))
        return false;

    if (strncmp(target_expected, target_got, strlen(target_expected))) {
        set_error_buf_v(error_buf, error_buf_size,
                        "invalid target type, expected %s but got %s",
                        target_expected, target_got);
        return false;
    }

    return true;
}

static bool
check_feature_flags(char *error_buf, uint32 error_buf_size,
                    uint64 feature_flags)
{
#if WASM_ENABLE_SIMD == 0
    if (feature_flags & WASM_FEATURE_SIMD_128BIT) {
        set_error_buf(error_buf, error_buf_size,
                      "SIMD is not enabled in this build");
        return false;
    }
#endif

#if WASM_ENABLE_BULK_MEMORY == 0
    if (feature_flags & WASM_FEATURE_BULK_MEMORY) {
        set_error_buf(error_buf, error_buf_size,
                      "bulk memory is not enabled in this build");
        return false;
    }
#endif

#if WASM_ENABLE_THREAD_MGR == 0
    if (feature_flags & WASM_FEATURE_MULTI_THREAD) {
        set_error_buf(error_buf, error_buf_size,
                      "thread is not enabled in this build");
        return false;
    }
#endif

#if WASM_ENABLE_REF_TYPES == 0
    if (feature_flags & WASM_FEATURE_REF_TYPES) {
        set_error_buf(error_buf, error_buf_size,
                      "reference types is not enabled in this build");
        return false;
    }
#endif

#if WASM_ENABLE_GC == 0
    if (feature_flags & WASM_FEATURE_GARBAGE_COLLECTION) {
        set_error_buf(error_buf, error_buf_size,
                      "garbage collection is not enabled in this build");
        return false;
    }
#endif

    return true;
}

#if WASM_ENABLE_GC != 0
static WASMRefType *
reftype_set_insert(HashMap *ref_type_set, const WASMRefType *ref_type,
                   char *error_buf, uint32 error_buf_size)
{
    WASMRefType *ret = wasm_reftype_set_insert(ref_type_set, ref_type);

    if (!ret) {
        set_error_buf(error_buf, error_buf_size,
                      "insert ref type to hash set failed");
    }
    return ret;
}
#endif

static bool
load_target_info_section(const uint8 *buf, const uint8 *buf_end,
                         AOTModule *module, char *error_buf,
                         uint32 error_buf_size)
{
    AOTTargetInfo target_info;
    const uint8 *p = buf, *p_end = buf_end;
    bool is_target_little_endian, is_target_64_bit;

    read_uint16(p, p_end, target_info.bin_type);
    read_uint16(p, p_end, target_info.abi_type);
    read_uint16(p, p_end, target_info.e_type);
    read_uint16(p, p_end, target_info.e_machine);
    read_uint32(p, p_end, target_info.e_version);
    read_uint32(p, p_end, target_info.e_flags);
    read_uint64(p, p_end, target_info.feature_flags);
    read_uint64(p, p_end, target_info.reserved);
    read_byte_array(p, p_end, target_info.arch, sizeof(target_info.arch));

    if (target_info.arch[sizeof(target_info.arch) - 1] != '\0') {
        set_error_buf(error_buf, error_buf_size, "invalid arch string");
        return false;
    }

    if (p != buf_end) {
        set_error_buf(error_buf, error_buf_size, "invalid section size");
        return false;
    }

    /* Check target endian type */
    is_target_little_endian = target_info.bin_type & 1 ? false : true;
    if (is_little_endian() != is_target_little_endian) {
        set_error_buf_v(error_buf, error_buf_size,
                        "invalid target endian type, expected %s but got %s",
                        is_little_endian() ? "little endian" : "big endian",
                        is_target_little_endian ? "little endian"
                                                : "big endian");
        return false;
    }

    /* Check target bit width */
    is_target_64_bit = target_info.bin_type & 2 ? true : false;
    if ((sizeof(void *) == 8 ? true : false) != is_target_64_bit) {
        set_error_buf_v(error_buf, error_buf_size,
                        "invalid target bit width, expected %s but got %s",
                        sizeof(void *) == 8 ? "64-bit" : "32-bit",
                        is_target_64_bit ? "64-bit" : "32-bit");
        return false;
    }

    /* Check target elf file type */
    if (target_info.e_type != E_TYPE_REL && target_info.e_type != E_TYPE_XIP) {
        set_error_buf(error_buf, error_buf_size,
                      "invalid object file type, "
                      "expected relocatable or XIP file type but got others");
        return false;
    }

    /* for backwards compatibility with previous wamrc aot files */
    if (!strcmp(target_info.arch, "arm64")
        || !strcmp(target_info.arch, "aarch64"))
        bh_strcpy_s(target_info.arch, sizeof(target_info.arch), "aarch64v8");

    /* Check machine info */
    if (!check_machine_info(&target_info, error_buf, error_buf_size)) {
        return false;
    }

    if (target_info.e_version != E_VERSION_CURRENT) {
        set_error_buf(error_buf, error_buf_size, "invalid elf file version");
        return false;
    }

#if WASM_ENABLE_DUMP_CALL_STACK != 0
    module->feature_flags = target_info.feature_flags;
#endif

    /* Finally, check feature flags */
    return check_feature_flags(error_buf, error_buf_size,
                               target_info.feature_flags);
fail:
    return false;
}

static void *
get_native_symbol_by_name(const char *name)
{
    void *func = NULL;
    uint32 symnum = 0;
    SymbolMap *sym = NULL;

    sym = get_target_symbol_map(&symnum);

    while (symnum && symnum--) {
        if (strcmp(sym->symbol_name, name) == 0) {
            func = sym->symbol_addr;
            break;
        }
        sym++;
    }

    return func;
}

static bool
str2uint32(const char *buf, uint32 *p_res);

static bool
str2uint64(const char *buf, uint64 *p_res);

static bool
load_native_symbol_section(const uint8 *buf, const uint8 *buf_end,
                           AOTModule *module, bool is_load_from_file_buf,
                           char *error_buf, uint32 error_buf_size)
{
    const uint8 *p = buf, *p_end = buf_end;
    uint32 cnt;
    int32 i;
    const char *symbol;

    if (module->native_symbol_list) {
        set_error_buf(error_buf, error_buf_size,
                      "duplicated native symbol section");
        return false;
    }

    read_uint32(p, p_end, cnt);

    if (cnt > 0) {
        uint64 list_size = cnt * (uint64)sizeof(void *);
        module->native_symbol_list =
            loader_malloc(list_size, error_buf, error_buf_size);
        if (module->native_symbol_list == NULL) {
            goto fail;
        }

        for (i = cnt - 1; i >= 0; i--) {
            read_string(p, p_end, symbol);
            if (!strlen(symbol))
                continue;

            if (!strncmp(symbol, "f32#", 4) || !strncmp(symbol, "i32#", 4)) {
                uint32 u32;
                /* Resolve the raw int bits of f32 const */
                if (!str2uint32(symbol + 4, &u32)) {
                    set_error_buf_v(error_buf, error_buf_size,
                                    "resolve symbol %s failed", symbol);
                    goto fail;
                }
                *(uint32 *)(&module->native_symbol_list[i]) = u32;
            }
            else if (!strncmp(symbol, "f64#", 4)
                     || !strncmp(symbol, "i64#", 4)) {
                uint64 u64;
                /* Resolve the raw int bits of f64 const */
                if (!str2uint64(symbol + 4, &u64)) {
                    set_error_buf_v(error_buf, error_buf_size,
                                    "resolve symbol %s failed", symbol);
                    goto fail;
                }
                *(uint64 *)(&module->native_symbol_list[i]) = u64;
            }
            else if (!strncmp(symbol, "__ignore", 8)) {
                /* Padding bytes to make f64 on 8-byte aligned address,
                   or it is the second 32-bit slot in 32-bit system */
                continue;
            }
            else {
                module->native_symbol_list[i] =
                    get_native_symbol_by_name(symbol);
                if (module->native_symbol_list[i] == NULL) {
                    set_error_buf_v(error_buf, error_buf_size,
                                    "missing native symbol: %s", symbol);
                    goto fail;
                }
            }
        }
    }

    return true;
fail:
    return false;
}

static bool
load_name_section(const uint8 *buf, const uint8 *buf_end, AOTModule *module,
                  bool is_load_from_file_buf, char *error_buf,
                  uint32 error_buf_size)
{
#if WASM_ENABLE_CUSTOM_NAME_SECTION != 0
    const uint8 *p = buf, *p_end = buf_end;
    uint32 *aux_func_indexes;
    const char **aux_func_names;
    uint32 name_type, subsection_size;
    uint32 previous_name_type = 0;
    uint32 num_func_name;
    uint32 func_index;
    uint32 previous_func_index = ~0U;
    uint32 name_index;
    int i = 0;
    uint32 name_len;
    uint64 size;

    if (p >= p_end) {
        set_error_buf(error_buf, error_buf_size, "unexpected end");
        return false;
    }

    read_uint32(p, p_end, name_len);

    if (name_len != 4 || p + name_len > p_end) {
        set_error_buf(error_buf, error_buf_size, "unexpected end");
        return false;
    }

    if (memcmp(p, "name", 4) != 0) {
        set_error_buf(error_buf, error_buf_size, "invalid custom name section");
        return false;
    }
    p += name_len;

    while (p < p_end) {
        read_uint32(p, p_end, name_type);
        if (i != 0) {
            if (name_type == previous_name_type) {
                set_error_buf(error_buf, error_buf_size,
                              "duplicate sub-section");
                return false;
            }
            if (name_type < previous_name_type) {
                set_error_buf(error_buf, error_buf_size,
                              "out-of-order sub-section");
                return false;
            }
        }
        previous_name_type = name_type;
        read_uint32(p, p_end, subsection_size);
        CHECK_BUF(p, p_end, subsection_size);
        switch (name_type) {
            case SUB_SECTION_TYPE_FUNC:
                if (subsection_size) {
                    read_uint32(p, p_end, num_func_name);
                    if (num_func_name
                        > module->import_func_count + module->func_count) {
                        set_error_buf(error_buf, error_buf_size,
                                      "function name count out of bounds");
                        return false;
                    }
                    module->aux_func_name_count = num_func_name;

                    /* Allocate memory */
                    size = sizeof(uint32) * (uint64)module->aux_func_name_count;
                    if (!(aux_func_indexes = module->aux_func_indexes =
                              loader_malloc(size, error_buf, error_buf_size))) {
                        return false;
                    }
                    size =
                        sizeof(char **) * (uint64)module->aux_func_name_count;
                    if (!(aux_func_names = module->aux_func_names =
                              loader_malloc(size, error_buf, error_buf_size))) {
                        return false;
                    }

                    for (name_index = 0; name_index < num_func_name;
                         name_index++) {
                        read_uint32(p, p_end, func_index);
                        if (name_index != 0
                            && func_index == previous_func_index) {
                            set_error_buf(error_buf, error_buf_size,
                                          "duplicate function name");
                            return false;
                        }
                        if (name_index != 0
                            && func_index < previous_func_index) {
                            set_error_buf(error_buf, error_buf_size,
                                          "out-of-order function index ");
                            return false;
                        }
                        if (func_index
                            >= module->import_func_count + module->func_count) {
                            set_error_buf(error_buf, error_buf_size,
                                          "function index out of bounds");
                            return false;
                        }
                        previous_func_index = func_index;
                        *(aux_func_indexes + name_index) = func_index;
                        read_string(p, p_end, *(aux_func_names + name_index));
#if 0
                        LOG_DEBUG("func_index %u -> aux_func_name = %s\n",
                               func_index, *(aux_func_names + name_index));
#endif
                    }
                }
                break;
            case SUB_SECTION_TYPE_MODULE: /* TODO: Parse for module subsection
                                           */
            case SUB_SECTION_TYPE_LOCAL:  /* TODO: Parse for local subsection */
            default:
                p = p + subsection_size;
                break;
        }
        i++;
    }

    return true;
fail:
    return false;
#else
    return true;
#endif /* WASM_ENABLE_CUSTOM_NAME_SECTION != 0 */
}

#if WASM_ENABLE_STRINGREF != 0
static bool
load_string_literal_section(const uint8 *buf, const uint8 *buf_end,
                            AOTModule *module, bool is_load_from_file_buf,
                            char *error_buf, uint32 error_buf_size)
{
    const uint8 *p = buf, *p_end = buf_end;
    uint32 reserved = 0, string_count = 0, i;
    uint64 size;

    read_uint32(p, p_end, reserved);
    if (reserved != 0) {
        set_error_buf(error_buf, error_buf_size,
                      "invalid reserved slot in string literal count");
        goto fail;
    }

    read_uint32(p, p_end, string_count);
    if (string_count == 0) {
        set_error_buf(error_buf, error_buf_size,
                      "invalid string literal count");
        goto fail;
    }
    module->string_literal_count = string_count;

    size = (uint64)sizeof(char *) * string_count;
    if (!(module->string_literal_ptrs =
              loader_malloc(size, error_buf, error_buf_size))) {
        goto fail;
    }

    size = (uint64)sizeof(uint32) * string_count;
    if (!(module->string_literal_lengths =
              loader_malloc(size, error_buf, error_buf_size))) {
        goto fail;
    }

    for (i = 0; i < string_count; i++) {
        read_uint32(p, p_end, module->string_literal_lengths[i]);
    }

    for (i = 0; i < string_count; i++) {
        module->string_literal_ptrs[i] = p;
        p += module->string_literal_lengths[i];
    }

    return true;

fail:
    return false;
}
#endif /* end of WASM_ENABLE_STRINGREF != 0 */

static bool
load_custom_section(const uint8 *buf, const uint8 *buf_end, AOTModule *module,
                    bool is_load_from_file_buf, char *error_buf,
                    uint32 error_buf_size)
{
    const uint8 *p = buf, *p_end = buf_end;
    uint32 sub_section_type;

    read_uint32(p, p_end, sub_section_type);
    buf = p;

    switch (sub_section_type) {
        case AOT_CUSTOM_SECTION_NATIVE_SYMBOL:
            if (!load_native_symbol_section(buf, buf_end, module,
                                            is_load_from_file_buf, error_buf,
                                            error_buf_size))
                goto fail;
            break;
        case AOT_CUSTOM_SECTION_NAME:
            if (!load_name_section(buf, buf_end, module, is_load_from_file_buf,
                                   error_buf, error_buf_size))
                LOG_VERBOSE("Load name section failed.");
            else
                LOG_VERBOSE("Load name section success.");
            break;
#if WASM_ENABLE_STRINGREF != 0
        case AOT_CUSTOM_SECTION_STRING_LITERAL:
            if (!load_string_literal_section(buf, buf_end, module,
                                             is_load_from_file_buf, error_buf,
                                             error_buf_size))
                goto fail;
            break;
#endif
#if WASM_ENABLE_LOAD_CUSTOM_SECTION != 0
        case AOT_CUSTOM_SECTION_RAW:
        {
            const char *section_name;
            WASMCustomSection *section;

            if (p >= p_end) {
                set_error_buf(error_buf, error_buf_size, "unexpected end");
                goto fail;
            }

            read_string(p, p_end, section_name);

            section = loader_malloc(sizeof(WASMCustomSection), error_buf,
                                    error_buf_size);
            if (!section) {
                goto fail;
            }

            section->name_addr = (char *)section_name;
            section->name_len = (uint32)strlen(section_name);
            section->content_addr = (uint8 *)p;
            section->content_len = (uint32)(p_end - p);

            section->next = module->custom_section_list;
            module->custom_section_list = section;
            LOG_VERBOSE("Load custom section [%s] success.", section_name);
            break;
        }
#endif /* end of WASM_ENABLE_LOAD_CUSTOM_SECTION != 0 */
        default:
            break;
    }

    return true;
fail:
    return false;
}

#if WASM_ENABLE_GC != 0 || WASM_ENABLE_EXTENDED_CONST_EXPR != 0
static void
destroy_init_expr(InitializerExpression *expr)
{
#if WASM_ENABLE_GC != 0
    if (expr->init_expr_type == INIT_EXPR_TYPE_STRUCT_NEW
        || expr->init_expr_type == INIT_EXPR_TYPE_ARRAY_NEW
        || expr->init_expr_type == INIT_EXPR_TYPE_ARRAY_NEW_FIXED) {
        wasm_runtime_free(expr->u.unary.v.data);
    }
#endif

#if WASM_ENABLE_EXTENDED_CONST_EXPR != 0
    // free left expr and right expr for binary oprand
    if (!is_expr_binary_op(expr->init_expr_type)) {
        return;
    }
    if (expr->u.binary.l_expr) {
        destroy_init_expr_recursive(expr->u.binary.l_expr);
    }
    if (expr->u.binary.r_expr) {
        destroy_init_expr_recursive(expr->u.binary.r_expr);
    }
    expr->u.binary.l_expr = expr->u.binary.r_expr = NULL;
#endif
}
#endif /* end of WASM_ENABLE_GC != 0 || WASM_ENABLE_EXTENDED_CONST_EXPR != 0 \
        */

static void
destroy_import_memories(AOTImportMemory *import_memories)
{
    wasm_runtime_free(import_memories);
}

/**
 * Free memory initialization data segments.
 *
 * @param module the AOT module containing the data
 * @param data_list array of memory initialization data segments to free
 * @param count number of segments in the data_list array
 */

static void
destroy_mem_init_data_list(AOTModule *module, AOTMemInitData **data_list,
                           uint32 count)
{
    uint32 i;
    /* Free each memory initialization data segment */
    for (i = 0; i < count; i++)
        if (data_list[i]) {
            /* If the module owns the binary data, free the bytes buffer */
            if (module->is_binary_freeable && data_list[i]->bytes)
                wasm_runtime_free(data_list[i]->bytes);

#if WASM_ENABLE_EXTENDED_CONST_EXPR != 0
            destroy_init_expr(&data_list[i]->offset);
#endif
            /* Free the data segment structure itself */
            wasm_runtime_free(data_list[i]);
        }
    /* Free the array of data segment pointers */
    wasm_runtime_free(data_list);
}

static bool
load_init_expr(const uint8 **p_buf, const uint8 *buf_end, AOTModule *module,
               InitializerExpression *expr, char *error_buf,
               uint32 error_buf_size);

/**
 * Load memory initialization data segments from the AOT module.
 *
 * This function reads memory initialization data segments from the buffer and
 * creates AOTMemInitData structures for each segment. The data can either be
 * cloned into new memory or referenced directly from the buffer.
 *
 * @param p_buf pointer to buffer containing memory init data
 * @param buf_end end of buffer
 * @param module the AOT module being loaded
 * @param error_buf buffer for error messages
 * @param error_buf_size size of error buffer
 *
 * @return true if successful, false if error occurred
 */

static bool
load_mem_init_data_list(const uint8 **p_buf, const uint8 *buf_end,
                        AOTModule *module, char *error_buf,
                        uint32 error_buf_size)
{
    const uint8 *buf = *p_buf;
    AOTMemInitData **data_list;
    uint64 size;
    uint32 i;

    /* Allocate memory */
    size = sizeof(AOTMemInitData *) * (uint64)module->mem_init_data_count;
    if (!(module->mem_init_data_list = data_list =
              loader_malloc(size, error_buf, error_buf_size))) {
        return false;
    }

    /* Create each memory data segment */
    for (i = 0; i < module->mem_init_data_count; i++) {
        uint32 byte_count;
        uint32 is_passive;
        uint32 memory_index;
        InitializerExpression offset_expr;

        read_uint32(buf, buf_end, is_passive);
        read_uint32(buf, buf_end, memory_index);
        if (!load_init_expr(&buf, buf_end, module, &offset_expr, error_buf,
                            error_buf_size)) {
            return false;
        }
        read_uint32(buf, buf_end, byte_count);
        if (!(data_list[i] = loader_malloc(sizeof(AOTMemInitData), error_buf,
                                           error_buf_size))) {
            return false;
        }

#if WASM_ENABLE_BULK_MEMORY != 0
        /* is_passive and memory_index is only used in bulk memory mode */
        data_list[i]->is_passive = (bool)is_passive;
        data_list[i]->memory_index = memory_index;
#endif
        data_list[i]->offset = offset_expr;
        data_list[i]->byte_count = byte_count;
        data_list[i]->bytes = NULL;
        /* If the module owns the binary data, clone the bytes buffer */
        if (module->is_binary_freeable) {
            if (byte_count > 0) {
                if (!(data_list[i]->bytes = loader_malloc(byte_count, error_buf,
                                                          error_buf_size))) {
                    return false;
                }
                read_byte_array(buf, buf_end, data_list[i]->bytes,
                                data_list[i]->byte_count);
            }
        }
        else {
            data_list[i]->bytes = (uint8 *)buf;
            buf += byte_count;
        }
    }

    *p_buf = buf;
    return true;
fail:
    return false;
}

/**
 * Load memory information from the AOT module.
 *
 * This function reads memory-related data including import memory count,
 * memory count, memory flags, page sizes, and memory initialization data.
 *
 * @param p_buf pointer to buffer containing memory info
 * @param buf_end end of buffer
 * @param module the AOT module being loaded
 * @param error_buf buffer for error messages
 * @param error_buf_size size of error buffer
 *
 * @return true if successful, false if error occurred
 */

static bool
load_memory_info(const uint8 **p_buf, const uint8 *buf_end, AOTModule *module,
                 char *error_buf, uint32 error_buf_size)
{
    uint32 i;
    uint64 total_size;
    const uint8 *buf = *p_buf;

    read_uint32(buf, buf_end, module->import_memory_count);

    read_uint32(buf, buf_end, module->memory_count);
    total_size = sizeof(AOTMemory) * (uint64)module->memory_count;
    if (!(module->memories =
              loader_malloc(total_size, error_buf, error_buf_size))) {
        return false;
    }

    for (i = 0; i < module->memory_count; i++) {
        read_uint32(buf, buf_end, module->memories[i].flags);

        if (!wasm_memory_check_flags(module->memories[i].flags, error_buf,
                                     error_buf_size, true)) {
            return false;
        }

        read_uint32(buf, buf_end, module->memories[i].num_bytes_per_page);
        read_uint32(buf, buf_end, module->memories[i].init_page_count);
        read_uint32(buf, buf_end, module->memories[i].max_page_count);
    }

    read_uint32(buf, buf_end, module->mem_init_data_count);

    /* load memory init data list */
    if (module->mem_init_data_count > 0
        && !load_mem_init_data_list(&buf, buf_end, module, error_buf,
                                    error_buf_size))
        return false;

    *p_buf = buf;
    return true;
fail:
    return false;
}

static void
destroy_import_tables(AOTImportTable *import_tables)
{
    wasm_runtime_free(import_tables);
}

static void
destroy_tables(AOTTable *tables)
{
    wasm_runtime_free(tables);
}

static void
destroy_table_init_data_list(AOTTableInitData **data_list, uint32 count)
{
    uint32 i;
    for (i = 0; i < count; i++)
        if (data_list[i]) {
#if WASM_ENABLE_GC != 0
            uint32 j;
            for (j = 0; j < data_list[i]->value_count; j++) {
                destroy_init_expr(&data_list[i]->init_values[j]);
            }
#endif
#if WASM_ENABLE_EXTENDED_CONST_EXPR != 0
            destroy_init_expr(&data_list[i]->offset);
#endif
            wasm_runtime_free(data_list[i]);
        }
    wasm_runtime_free(data_list);
}

static bool
load_init_expr(const uint8 **p_buf, const uint8 *buf_end, AOTModule *module,
               InitializerExpression *expr, char *error_buf,
               uint32 error_buf_size)
{
    const uint8 *buf = *p_buf;
    uint32 init_expr_type = 0;
    uint64 *i64x2 = NULL;
    bool free_if_fail = false;

    buf = (uint8 *)align_ptr(buf, 4);

    read_uint32(buf, buf_end, init_expr_type);

    switch (init_expr_type) {
        case INIT_EXPR_NONE:
            break;
        case INIT_EXPR_TYPE_I32_CONST:
        case INIT_EXPR_TYPE_F32_CONST:
            read_uint32(buf, buf_end, expr->u.unary.v.i32);
            break;
        case INIT_EXPR_TYPE_I64_CONST:
        case INIT_EXPR_TYPE_F64_CONST:
            read_uint64(buf, buf_end, expr->u.unary.v.i64);
            break;
        case INIT_EXPR_TYPE_V128_CONST:
            i64x2 = (uint64 *)expr->u.unary.v.v128.i64x2;
            CHECK_BUF(buf, buf_end, sizeof(uint64) * 2);
            wasm_runtime_read_v128(buf, &i64x2[0], &i64x2[1]);
            buf += sizeof(uint64) * 2;
            break;
        case INIT_EXPR_TYPE_GET_GLOBAL:
            read_uint32(buf, buf_end, expr->u.unary.v.global_index);
            break;
        /* INIT_EXPR_TYPE_FUNCREF_CONST can be used when
           both reference types and GC are disabled */
        case INIT_EXPR_TYPE_FUNCREF_CONST:
            read_uint32(buf, buf_end, expr->u.unary.v.ref_index);
            break;
#if WASM_ENABLE_GC != 0 || WASM_ENABLE_REF_TYPES != 0
        case INIT_EXPR_TYPE_REFNULL_CONST:
            read_uint32(buf, buf_end, expr->u.unary.v.ref_index);
            break;
#endif /* end of WASM_ENABLE_GC != 0 || WASM_ENABLE_REF_TYPES != 0 */
#if WASM_ENABLE_GC != 0
        case INIT_EXPR_TYPE_I31_NEW:
            read_uint32(buf, buf_end, expr->u.unary.v.i32);
            break;
        case INIT_EXPR_TYPE_STRUCT_NEW:
        {
            uint64 size;
            uint32 type_idx, field_count;
            AOTStructType *struct_type = NULL;
            WASMStructNewInitValues *init_values = NULL;

            read_uint32(buf, buf_end, type_idx);
            read_uint32(buf, buf_end, field_count);

            size = offsetof(WASMStructNewInitValues, fields)
                   + sizeof(WASMValue) * (uint64)field_count;
            if (!(init_values =
                      loader_malloc(size, error_buf, error_buf_size))) {
                return false;
            }
            free_if_fail = true;
            init_values->count = field_count;
            init_values->type_idx = type_idx;
            expr->u.unary.v.data = init_values;

            if (type_idx >= module->type_count) {
                set_error_buf(error_buf, error_buf_size,
                              "unknown struct type.");
                goto fail;
            }

            struct_type = (AOTStructType *)module->types[type_idx];

            if (struct_type->field_count != field_count) {
                set_error_buf(error_buf, error_buf_size,
                              "invalid field count.");
                goto fail;
            }

            if (field_count > 0) {
                uint32 i;

                for (i = 0; i < field_count; i++) {
                    uint32 field_size =
                        wasm_value_type_size(struct_type->fields[i].field_type);
                    if (field_size <= sizeof(uint32))
                        read_uint32(buf, buf_end, init_values->fields[i].u32);
                    else if (field_size == sizeof(uint64))
                        read_uint64(buf, buf_end, init_values->fields[i].u64);
                    else if (field_size == sizeof(uint64) * 2)
                        read_byte_array(buf, buf_end, &init_values->fields[i],
                                        field_size);
                    else {
                        bh_assert(0);
                    }
                }
            }

            break;
        }
        case INIT_EXPR_TYPE_STRUCT_NEW_DEFAULT:
            read_uint32(buf, buf_end, expr->u.unary.v.type_index);
            break;
        case INIT_EXPR_TYPE_ARRAY_NEW:
        case INIT_EXPR_TYPE_ARRAY_NEW_DEFAULT:
        case INIT_EXPR_TYPE_ARRAY_NEW_FIXED:
        {
            uint32 array_elem_type;
            uint32 type_idx, length;
            WASMArrayNewInitValues *init_values = NULL;

            /* Note: at this time the aot types haven't been loaded */
            read_uint32(buf, buf_end, array_elem_type);
            read_uint32(buf, buf_end, type_idx);
            read_uint32(buf, buf_end, length);

            if (type_idx >= module->type_count
                || !wasm_type_is_array_type(module->types[type_idx])) {
                set_error_buf(error_buf, error_buf_size,
                              "invalid or non-array type index.");
                goto fail;
            }

            if (init_expr_type == INIT_EXPR_TYPE_ARRAY_NEW_DEFAULT) {
                expr->u.unary.v.array_new_default.type_index = type_idx;
                expr->u.unary.v.array_new_default.length = length;
            }
            else {
                uint32 i, elem_size, elem_data_count;
                uint64 size = offsetof(WASMArrayNewInitValues, elem_data)
                              + sizeof(WASMValue) * (uint64)length;
                if (!(init_values =
                          loader_malloc(size, error_buf, error_buf_size))) {
                    return false;
                }
                free_if_fail = true;
                expr->u.unary.v.data = init_values;

                init_values->type_idx = type_idx;
                init_values->length = length;

                elem_data_count =
                    (init_expr_type == INIT_EXPR_TYPE_ARRAY_NEW_FIXED) ? length
                                                                       : 1;
                elem_size = wasm_value_type_size((uint8)array_elem_type);

                for (i = 0; i < elem_data_count; i++) {
                    if (elem_size <= sizeof(uint32))
                        read_uint32(buf, buf_end,
                                    init_values->elem_data[i].u32);
                    else if (elem_size == sizeof(uint64))
                        read_uint64(buf, buf_end,
                                    init_values->elem_data[i].u64);
                    else if (elem_size == sizeof(uint64) * 2)
                        read_byte_array(buf, buf_end,
                                        &init_values->elem_data[i], elem_size);
                    else {
                        bh_assert(0);
                    }
                }
            }
            break;
        }
#endif /* end of WASM_ENABLE_GC != 0 */
#if WASM_ENABLE_EXTENDED_CONST_EXPR != 0
        case INIT_EXPR_TYPE_I32_ADD:
        case INIT_EXPR_TYPE_I32_SUB:
        case INIT_EXPR_TYPE_I32_MUL:
        case INIT_EXPR_TYPE_I64_ADD:
        case INIT_EXPR_TYPE_I64_SUB:
        case INIT_EXPR_TYPE_I64_MUL:
        {
            expr->u.binary.l_expr = expr->u.binary.r_expr = NULL;
            if (!(expr->u.binary.l_expr =
                      loader_malloc(sizeof(InitializerExpression), error_buf,
                                    error_buf_size))) {
                goto fail;
            }
            if (!load_init_expr(&buf, buf_end, module, expr->u.binary.l_expr,
                                error_buf, error_buf_size))
                goto fail;
            if (!(expr->u.binary.r_expr =
                      loader_malloc(sizeof(InitializerExpression), error_buf,
                                    error_buf_size))) {
                goto fail;
            }
            if (!load_init_expr(&buf, buf_end, module, expr->u.binary.r_expr,
                                error_buf, error_buf_size))
                goto fail;
            break;
        }
#endif /* end of WASM_ENABLE_EXTENDED_CONST_EXPR != 0 */
        default:
            set_error_buf(error_buf, error_buf_size, "invalid init expr type.");
            return false;
    }

    expr->init_expr_type = (uint8)init_expr_type;

    *p_buf = buf;
    return true;
fail:
#if WASM_ENABLE_GC != 0
    if (free_if_fail) {
        wasm_runtime_free(expr->u.unary.v.data);
    }
#else
    (void)free_if_fail;
#endif
#if WASM_ENABLE_EXTENDED_CONST_EXPR != 0
    destroy_init_expr(expr);
#endif
    return false;
}

static bool
load_import_table_list(const uint8 **p_buf, const uint8 *buf_end,
                       AOTModule *module, char *error_buf,
                       uint32 error_buf_size)
{
    const uint8 *buf = *p_buf;
    AOTImportTable *import_table;
#if WASM_ENABLE_GC != 0
    WASMRefType ref_type;
#endif
    uint64 size;
    uint32 i;

    /* Allocate memory */
    size = sizeof(AOTImportTable) * (uint64)module->import_table_count;
    if (!(module->import_tables = import_table =
              loader_malloc(size, error_buf, error_buf_size))) {
        return false;
    }

    /* keep sync with aot_emit_table_info() aot_emit_aot_file */
    for (i = 0; i < module->import_table_count; i++, import_table++) {
        read_uint8(buf, buf_end, import_table->table_type.elem_type);
        read_uint8(buf, buf_end, import_table->table_type.flags);
        read_uint8(buf, buf_end, import_table->table_type.possible_grow);
#if WASM_ENABLE_GC != 0
        if (wasm_is_type_multi_byte_type(import_table->table_type.elem_type)) {
            read_uint8(buf, buf_end, ref_type.ref_ht_common.nullable);
        }
        else
#endif
        {
            /* Skip 1 byte */
            buf += 1;
        }
        read_uint32(buf, buf_end, import_table->table_type.init_size);
        read_uint32(buf, buf_end, import_table->table_type.max_size);
#if WASM_ENABLE_GC != 0
        if (wasm_is_type_multi_byte_type(import_table->table_type.elem_type)) {
            read_uint32(buf, buf_end, ref_type.ref_ht_common.heap_type);

            ref_type.ref_type = import_table->table_type.elem_type;
            /* TODO: check ref_type */
            if (!(import_table->table_type.elem_ref_type =
                      wasm_reftype_set_insert(module->ref_type_set,
                                              &ref_type))) {
                set_error_buf(error_buf, error_buf_size,
                              "insert ref type to hash set failed");
                return false;
            }
        }
#endif
    }

    *p_buf = buf;
    return true;
fail:
    return false;
}

static bool
load_table_list(const uint8 **p_buf, const uint8 *buf_end, AOTModule *module,
                char *error_buf, uint32 error_buf_size)
{
    const uint8 *buf = *p_buf;
    AOTTable *table;
#if WASM_ENABLE_GC != 0
    WASMRefType ref_type;
#endif
    uint64 size;
    uint32 i;

    /* Allocate memory */
    size = sizeof(AOTTable) * (uint64)module->table_count;
    if (!(module->tables = table =
              loader_malloc(size, error_buf, error_buf_size))) {
        return false;
    }

    /* Create each table data segment */
    for (i = 0; i < module->table_count; i++, table++) {
        read_uint8(buf, buf_end, table->table_type.elem_type);
        read_uint8(buf, buf_end, table->table_type.flags);

        if (!wasm_table_check_flags(table->table_type.flags, error_buf,
                                    error_buf_size, true)) {
            return false;
        }

        read_uint8(buf, buf_end, table->table_type.possible_grow);
#if WASM_ENABLE_GC != 0
        if (wasm_is_type_multi_byte_type(table->table_type.elem_type)) {
            read_uint8(buf, buf_end, ref_type.ref_ht_common.nullable);
        }
        else
#endif
        {
            /* Skip 1 byte */
            buf += 1;
        }
        read_uint32(buf, buf_end, table->table_type.init_size);
        read_uint32(buf, buf_end, table->table_type.max_size);
#if WASM_ENABLE_GC != 0
        if (wasm_is_type_multi_byte_type(table->table_type.elem_type)) {
            read_uint32(buf, buf_end, ref_type.ref_ht_common.heap_type);

            ref_type.ref_type = table->table_type.elem_type;
            /* TODO: check ref_type */
            if (!(table->table_type.elem_ref_type = wasm_reftype_set_insert(
                      module->ref_type_set, &ref_type))) {
                set_error_buf(error_buf, error_buf_size,
                              "insert ref type to hash set failed");
                return false;
            }
        }
        if (!load_init_expr(&buf, buf_end, module, &table->init_expr, error_buf,
                            error_buf_size))
            return false;

        if (table->init_expr.init_expr_type >= INIT_EXPR_TYPE_STRUCT_NEW
            && table->init_expr.init_expr_type
                   <= INIT_EXPR_TYPE_EXTERN_CONVERT_ANY) {
            set_error_buf(error_buf, error_buf_size,
                          "unsupported initializer expression for table");
            return false;
        }
#endif
    }

    *p_buf = buf;
    return true;
fail:
    return false;
}

static bool
load_table_init_data_list(const uint8 **p_buf, const uint8 *buf_end,
                          AOTModule *module, char *error_buf,
                          uint32 error_buf_size)
{
    const uint8 *buf = *p_buf;
    AOTTableInitData **data_list;
#if WASM_ENABLE_GC != 0
    WASMRefType reftype;
#endif
    uint64 size;
    uint32 i, j;

    /* Allocate memory */
    size = sizeof(AOTTableInitData *) * (uint64)module->table_init_data_count;
    if (!(module->table_init_data_list = data_list =
              loader_malloc(size, error_buf, error_buf_size))) {
        return false;
    }

    /* Create each table data segment */
    for (i = 0; i < module->table_init_data_count; i++) {
        uint32 mode, elem_type;
        uint32 table_index, value_count;
        uint64 size1;
        InitializerExpression offset_expr;

        read_uint32(buf, buf_end, mode);
        read_uint32(buf, buf_end, elem_type);
        read_uint32(buf, buf_end, table_index);
        if (!load_init_expr(&buf, buf_end, module, &offset_expr, error_buf,
                            error_buf_size))
            return false;
#if WASM_ENABLE_GC != 0
        if (wasm_is_type_multi_byte_type(elem_type)) {
            uint16 ref_type, nullable;
            read_uint16(buf, buf_end, ref_type);
            if (elem_type != ref_type) {
                set_error_buf(error_buf, error_buf_size, "invalid elem type");
                return false;
            }
            reftype.ref_ht_common.ref_type = (uint8)ref_type;
            read_uint16(buf, buf_end, nullable);
            if (nullable != 0 && nullable != 1) {
                set_error_buf(error_buf, error_buf_size,
                              "invalid nullable value");
                return false;
            }
            reftype.ref_ht_common.nullable = (uint8)nullable;
            read_uint32(buf, buf_end, reftype.ref_ht_common.heap_type);
        }
        else
#endif
        {
            /* Skip 8 byte(2+2+4) for ref type info */
            buf += 8;
        }

        read_uint32(buf, buf_end, value_count);

        size1 = sizeof(InitializerExpression) * (uint64)value_count;
        size = offsetof(AOTTableInitData, init_values) + size1;
        if (!(data_list[i] = loader_malloc(size, error_buf, error_buf_size))) {
            return false;
        }

        data_list[i]->mode = mode;
        data_list[i]->elem_type = elem_type;
        data_list[i]->table_index = table_index;
#if WASM_ENABLE_GC != 0
        if (wasm_is_type_multi_byte_type(elem_type)) {
            if (!(data_list[i]->elem_ref_type =
                      reftype_set_insert(module->ref_type_set, &reftype,
                                         error_buf, error_buf_size))) {
                goto fail;
            }
        }
#endif
        data_list[i]->offset = offset_expr;
        data_list[i]->value_count = value_count;
        for (j = 0; j < data_list[i]->value_count; j++) {
            if (!load_init_expr(&buf, buf_end, module,
                                &data_list[i]->init_values[j], error_buf,
                                error_buf_size))
                return false;
        }
    }

    *p_buf = buf;
    return true;
fail:
    return false;
}

static bool
load_table_info(const uint8 **p_buf, const uint8 *buf_end, AOTModule *module,
                char *error_buf, uint32 error_buf_size)
{
    const uint8 *buf = *p_buf;

    read_uint32(buf, buf_end, module->import_table_count);
    if (module->import_table_count > 0
        && !load_import_table_list(&buf, buf_end, module, error_buf,
                                   error_buf_size))
        return false;

    read_uint32(buf, buf_end, module->table_count);
    if (module->table_count > 0
        && !load_table_list(&buf, buf_end, module, error_buf, error_buf_size))
        return false;

    read_uint32(buf, buf_end, module->table_init_data_count);

    /* load table init data list */
    if (module->table_init_data_count > 0
        && !load_table_init_data_list(&buf, buf_end, module, error_buf,
                                      error_buf_size))
        return false;

    *p_buf = buf;
    return true;
fail:
    return false;
}

static void
destroy_type(AOTType *type)
{
#if WASM_ENABLE_GC != 0
    if (type->ref_count > 1) {
        /* The type is referenced by other types
           of current aot module */
        type->ref_count--;
        return;
    }

    if (type->type_flag == WASM_TYPE_FUNC) {
        AOTFuncType *func_type = (AOTFuncType *)type;
        if (func_type->ref_type_maps != NULL) {
            bh_assert(func_type->ref_type_map_count > 0);
            wasm_runtime_free(func_type->ref_type_maps);
        }
    }
    else if (type->type_flag == WASM_TYPE_STRUCT) {
        AOTStructType *struct_type = (AOTStructType *)type;
        if (struct_type->ref_type_maps != NULL) {
            bh_assert(struct_type->ref_type_map_count > 0);
            wasm_runtime_free(struct_type->ref_type_maps);
        }
    }
#endif
    wasm_runtime_free(type);
}

static void
destroy_types(AOTType **types, uint32 count)
{
    uint32 i;
    for (i = 0; i < count; i++) {
        if (types[i]) {
            destroy_type(types[i]);
        }
    }
    wasm_runtime_free(types);
}

#if WASM_ENABLE_GC != 0
static void
init_base_type(AOTType *base_type, uint32 type_idx, uint16 type_flag,
               bool is_sub_final, uint32 parent_type_idx, uint16 rec_count,
               uint16 rec_idx)
{
    base_type->type_flag = type_flag;
    base_type->ref_count = 1;
    base_type->is_sub_final = is_sub_final;
    base_type->parent_type_idx = parent_type_idx;
    base_type->rec_count = rec_count;
    base_type->rec_idx = rec_idx;
    base_type->rec_begin_type_idx = type_idx - rec_idx;
}

static bool
load_types(const uint8 **p_buf, const uint8 *buf_end, AOTModule *module,
           char *error_buf, uint32 error_buf_size)
{
    const uint8 *buf = *p_buf;
    AOTType **types;
    uint64 size;
    uint32 i, j;
    uint32 type_flag, param_cell_num, ret_cell_num;
    uint16 param_count, result_count, ref_type_map_count, rec_count, rec_idx;
    bool is_equivalence_type, is_sub_final;
    uint32 parent_type_idx;
    WASMRefType ref_type;

    /* Allocate memory */
    size = sizeof(AOTFuncType *) * (uint64)module->type_count;
    if (!(types = loader_malloc(size, error_buf, error_buf_size))) {
        return false;
    }

    module->types = types;

    /* Create each type */
    for (i = 0; i < module->type_count; i++) {
        buf = align_ptr(buf, 4);

        /* Read base type info */
        read_uint16(buf, buf_end, type_flag);

        read_uint8(buf, buf_end, is_equivalence_type);
        /* If there is an equivalence type, reuse it */
        if (is_equivalence_type) {
            uint8 u8;
            /* padding */
            read_uint8(buf, buf_end, u8);
            (void)u8;

            read_uint32(buf, buf_end, j);
#if WASM_ENABLE_AOT_VALIDATOR != 0
            /* an equivalence type should be before the type it refers to */
            if (j > i) {
                set_error_buf(error_buf, error_buf_size, "invalid type index");
                goto fail;
            }
#endif
            if (module->types[j]->ref_count == UINT16_MAX) {
                set_error_buf(error_buf, error_buf_size,
                              "wasm type's ref count too large");
                goto fail;
            }
            module->types[j]->ref_count++;
            module->types[i] = module->types[j];
            continue;
        }

        read_uint8(buf, buf_end, is_sub_final);
        read_uint32(buf, buf_end, parent_type_idx);
        read_uint16(buf, buf_end, rec_count);
        read_uint16(buf, buf_end, rec_idx);
#if WASM_ENABLE_AOT_VALIDATOR != 0
        if (rec_idx > i) {
            set_error_buf(error_buf, error_buf_size, "invalid rec_idx");
            goto fail;
        }
#endif
        if (type_flag == WASM_TYPE_FUNC) {
            AOTFuncType *func_type;

            /* Read param count */
            read_uint16(buf, buf_end, param_count);
            /* Read result count */
            read_uint16(buf, buf_end, result_count);
            /* Read ref_type_map_count */
            read_uint16(buf, buf_end, ref_type_map_count);

            func_type =
                loader_malloc(sizeof(AOTFuncType) + param_count + result_count,
                              error_buf, error_buf_size);

            if (!func_type) {
                goto fail;
            }

            types[i] = (AOTType *)func_type;

            init_base_type((AOTType *)func_type, i, type_flag, is_sub_final,
                           parent_type_idx, rec_count, rec_idx);
            func_type->param_count = param_count;
            func_type->result_count = result_count;

            /* Read types of params */
            read_byte_array(buf, buf_end, func_type->types,
                            func_type->param_count + func_type->result_count);

            func_type->ref_type_map_count = ref_type_map_count;

            if (!is_valid_func_type(func_type))
                goto fail;

            param_cell_num = wasm_get_cell_num(func_type->types, param_count);
            ret_cell_num =
                wasm_get_cell_num(func_type->types + param_count, result_count);
            if (param_cell_num > UINT16_MAX || ret_cell_num > UINT16_MAX) {
                set_error_buf(error_buf, error_buf_size,
                              "param count or result count too large");
                goto fail;
            }

            func_type->param_cell_num = param_cell_num;
            func_type->ret_cell_num = ret_cell_num;

#if WASM_ENABLE_QUICK_AOT_ENTRY != 0
            func_type->quick_aot_entry =
                wasm_native_lookup_quick_aot_entry(func_type);
#endif

            LOG_VERBOSE("type %u: func, param count: %d, result count: %d, "
                        "ref type map count: %d",
                        i, param_count, result_count, ref_type_map_count);

            /* If ref_type_map is not empty, read ref_type_map */
            if (ref_type_map_count > 0) {
                bh_assert(func_type->ref_type_map_count
                          <= func_type->param_count + func_type->result_count);

                /* align to 4 since param_count + result_count may be odd */
                buf = align_ptr(buf, 4);

                if (!(func_type->ref_type_maps =
                          loader_malloc(sizeof(WASMRefTypeMap)
                                            * func_type->ref_type_map_count,
                                        error_buf, error_buf_size))) {
                    goto fail;
                }

                for (j = 0; j < func_type->ref_type_map_count; j++) {
                    read_uint16(buf, buf_end,
                                func_type->ref_type_maps[j].index);
                    read_uint8(buf, buf_end, ref_type.ref_ht_common.ref_type);
                    read_uint8(buf, buf_end, ref_type.ref_ht_common.nullable);
                    read_uint32(buf, buf_end, ref_type.ref_ht_common.heap_type);
                    /* TODO: check ref_type */
                    if (!(func_type->ref_type_maps[j].ref_type =
                              wasm_reftype_set_insert(module->ref_type_set,
                                                      &ref_type))) {
                        set_error_buf(error_buf, error_buf_size,
                                      "insert ref type to hash set failed");
                        goto fail;
                    }
                }

                func_type->result_ref_type_maps = func_type->ref_type_maps;
                for (j = 0; j < func_type->param_count; j++) {
                    if (wasm_is_type_multi_byte_type(func_type->types[j]))
                        func_type->result_ref_type_maps++;
                }
            }
        }
        else if (type_flag == WASM_TYPE_STRUCT) {
            AOTStructType *struct_type;
            const uint8 *buf_org;
            uint16 *reference_table;
            uint16 field_count, ref_field_count = 0;
            uint32 offset;

            read_uint16(buf, buf_end, field_count);
            read_uint16(buf, buf_end, ref_type_map_count);

            buf_org = buf;

            /* Traverse first time to get ref_field_count */
            for (j = 0; j < field_count; j++) {
                uint8 field_flags, field_type;

                read_uint8(buf, buf_end, field_flags);
                read_uint8(buf, buf_end, field_type);
                if (wasm_is_type_reftype(field_type))
                    ref_field_count++;

                (void)field_flags;
            }

            struct_type = loader_malloc(
                sizeof(AOTStructType)
                    + sizeof(WASMStructFieldType) * (uint64)field_count
                    + sizeof(uint16) * (uint64)(ref_field_count + 1),
                error_buf, error_buf_size);
            if (!struct_type) {
                goto fail;
            }

            offset = (uint32)offsetof(WASMStructObject, field_data);
            types[i] = (AOTType *)struct_type;

            init_base_type((AOTType *)struct_type, i, type_flag, is_sub_final,
                           parent_type_idx, rec_count, rec_idx);
            struct_type->field_count = field_count;
            struct_type->ref_type_map_count = ref_type_map_count;

            struct_type->reference_table = reference_table =
                (uint16 *)((uint8 *)struct_type
                           + offsetof(AOTStructType, fields)
                           + sizeof(WASMStructFieldType) * field_count);
            *reference_table++ = ref_field_count;

            LOG_VERBOSE(
                "type %u: struct, field count: %d, ref type map count: %d", i,
                field_count, ref_type_map_count);

            buf = buf_org;

            /* Traverse again to read each field */
            for (j = 0; j < field_count; j++) {
                uint8 field_type, field_size;

                read_uint8(buf, buf_end, struct_type->fields[j].field_flags);
                read_uint8(buf, buf_end, field_type);
                struct_type->fields[j].field_type = field_type;
                struct_type->fields[j].field_size = field_size =
                    (uint8)wasm_reftype_size(field_type);
#if !(defined(BUILD_TARGET_X86_64) || defined(BUILD_TARGET_AMD_64) \
      || defined(BUILD_TARGET_X86_32))
                if (field_size == 2)
                    offset = align_uint(offset, 2);
                else if (field_size >= 4) /* field size is 4 or 8 */
                    offset = align_uint(offset, 4);
#endif
                struct_type->fields[j].field_offset = offset;
                if (wasm_is_type_reftype(field_type))
                    *reference_table++ = offset;
                offset += field_size;
                LOG_VERBOSE("                field: %d, flags: %d, type: %d", j,
                            struct_type->fields[j].field_flags,
                            struct_type->fields[j].field_type);
            }

            struct_type->total_size = offset;
            buf = align_ptr(buf, 4);

            /* If ref_type_map is not empty, read ref_type_map */
            if (ref_type_map_count > 0) {

                bh_assert(struct_type->ref_type_map_count <= field_count);

                if (!(struct_type->ref_type_maps =
                          loader_malloc(sizeof(WASMRefTypeMap)
                                            * struct_type->ref_type_map_count,
                                        error_buf, error_buf_size))) {
                    goto fail;
                }

                for (j = 0; j < struct_type->ref_type_map_count; j++) {
                    read_uint16(buf, buf_end,
                                struct_type->ref_type_maps[j].index);
                    read_uint8(buf, buf_end, ref_type.ref_ht_common.ref_type);
                    read_uint8(buf, buf_end, ref_type.ref_ht_common.nullable);
                    read_uint32(buf, buf_end, ref_type.ref_ht_common.heap_type);
                    /* TODO: check ref_type */
                    if (!(struct_type->ref_type_maps[j].ref_type =
                              wasm_reftype_set_insert(module->ref_type_set,
                                                      &ref_type))) {
                        set_error_buf(error_buf, error_buf_size,
                                      "insert ref type to hash set failed");
                        goto fail;
                    }
                }
            }
        }
        else if (type_flag == WASM_TYPE_ARRAY) {
            AOTArrayType *array_type;

            array_type =
                loader_malloc(sizeof(AOTArrayType), error_buf, error_buf_size);

            if (!array_type) {
                goto fail;
            }

            types[i] = (AOTType *)array_type;

            init_base_type((AOTType *)array_type, i, type_flag, is_sub_final,
                           parent_type_idx, rec_count, rec_idx);
            read_uint16(buf, buf_end, array_type->elem_flags);
            read_uint8(buf, buf_end, array_type->elem_type);
            if (wasm_is_type_multi_byte_type(array_type->elem_type)) {
                read_uint8(buf, buf_end, ref_type.ref_ht_common.nullable);
                read_uint32(buf, buf_end, ref_type.ref_ht_common.heap_type);
                ref_type.ref_type = array_type->elem_type;
                /* TODO: check ref_type */
                if (!(array_type->elem_ref_type = wasm_reftype_set_insert(
                          module->ref_type_set, &ref_type))) {
                    set_error_buf(error_buf, error_buf_size,
                                  "insert ref type to hash set failed");
                    goto fail;
                }
            }

            LOG_VERBOSE("type %u: array", i);
        }
        else {
            set_error_buf_v(error_buf, error_buf_size,
                            "invalid type flag: %" PRIu32, type_flag);
            goto fail;
        }

        if ((rec_count == 0) || (rec_idx == rec_count - 1)) {
            if (rec_count == 0) {
                bh_assert(rec_idx == 0);
            }
            for (j = i - rec_idx; j <= i; j++) {
                AOTType *cur_type = module->types[j];
                parent_type_idx = cur_type->parent_type_idx;
                if (parent_type_idx != (uint32)-1) { /* has parent */
#if WASM_ENABLE_AOT_VALIDATOR != 0
                    if (parent_type_idx >= module->type_count) {
                        set_error_buf(error_buf, error_buf_size,
                                      "invalid parent type index");
                        goto fail;
                    }
#endif
                    AOTType *parent_type = module->types[parent_type_idx];

                    module->types[j]->parent_type = parent_type;
                    module->types[j]->root_type = parent_type->root_type;
                    if (parent_type->inherit_depth == UINT16_MAX) {
                        set_error_buf(error_buf, error_buf_size,
                                      "parent type's inherit depth too large");
                        goto fail;
                    }
                    module->types[j]->inherit_depth =
                        parent_type->inherit_depth + 1;
                }
                else {
                    module->types[j]->parent_type = NULL;
                    module->types[j]->root_type = module->types[j];
                    module->types[j]->inherit_depth = 0;
                }
            }

            for (j = i - rec_idx; j <= i; j++) {
                AOTType *cur_type = module->types[j];
                parent_type_idx = cur_type->parent_type_idx;
                if (parent_type_idx != (uint32)-1) { /* has parent */
#if WASM_ENABLE_AOT_VALIDATOR != 0
                    if (parent_type_idx >= module->type_count) {
                        set_error_buf(error_buf, error_buf_size,
                                      "invalid parent type index");
                        goto fail;
                    }
#endif
                    AOTType *parent_type = module->types[parent_type_idx];
                    /* subtyping has been checked during compilation */
                    bh_assert(wasm_type_is_subtype_of(
                        module->types[j], parent_type, module->types, i + 1));
                    (void)parent_type;
                }
            }
        }
    }

    if (module->type_count) {
        if (!(module->rtt_types = loader_malloc((uint64)sizeof(WASMRttType *)
                                                    * module->type_count,
                                                error_buf, error_buf_size))) {
            goto fail;
        }
    }

    *p_buf = buf;
    return true;

fail:
    /* Destroy all types */
    destroy_types(types, module->type_count);
    module->types = NULL;
    return false;
}
#else
static bool
load_types(const uint8 **p_buf, const uint8 *buf_end, AOTModule *module,
           char *error_buf, uint32 error_buf_size)
{
    const uint8 *buf = *p_buf;
    AOTFuncType **func_types;
    uint64 size;
    uint32 i;

    /* Allocate memory */
    size = sizeof(AOTFuncType *) * (uint64)module->type_count;
    if (!(func_types = loader_malloc(size, error_buf, error_buf_size))) {
        return false;
    }

    module->types = (AOTType **)func_types;

    /* Create each function type */
    for (i = 0; i < module->type_count; i++) {
        uint32 type_flag;
        uint32 param_count, result_count;
        uint32 param_cell_num, ret_cell_num;
        uint64 size1;

        buf = align_ptr(buf, 4);
        read_uint16(buf, buf_end, type_flag);
        read_uint16(buf, buf_end, param_count);
        read_uint16(buf, buf_end, result_count);

        size1 = (uint64)param_count + (uint64)result_count;
        size = offsetof(AOTFuncType, types) + size1;
        if (!(func_types[i] = loader_malloc(size, error_buf, error_buf_size))) {
            return false;
        }

        func_types[i]->param_count = (uint16)param_count;
        func_types[i]->result_count = (uint16)result_count;
        read_byte_array(buf, buf_end, func_types[i]->types, (uint32)size1);

        if (!is_valid_func_type(func_types[i]))
            goto fail;

        param_cell_num = wasm_get_cell_num(func_types[i]->types, param_count);
        ret_cell_num =
            wasm_get_cell_num(func_types[i]->types + param_count, result_count);
        if (param_cell_num > UINT16_MAX || ret_cell_num > UINT16_MAX) {
            set_error_buf(error_buf, error_buf_size,
                          "param count or result count too large");
            return false;
        }

        func_types[i]->param_cell_num = (uint16)param_cell_num;
        func_types[i]->ret_cell_num = (uint16)ret_cell_num;

#if WASM_ENABLE_QUICK_AOT_ENTRY != 0
        func_types[i]->quick_aot_entry =
            wasm_native_lookup_quick_aot_entry(func_types[i]);
#endif
    }

    *p_buf = buf;
    return true;
fail:
    return false;
}
#endif /* end of WASM_ENABLE_GC != 0 */

static bool
load_type_info(const uint8 **p_buf, const uint8 *buf_end, AOTModule *module,
               char *error_buf, uint32 error_buf_size)
{
    const uint8 *buf = *p_buf;

    read_uint32(buf, buf_end, module->type_count);

    /* load function type */
    if (module->type_count > 0
        && !load_types(&buf, buf_end, module, error_buf, error_buf_size))
        return false;

    *p_buf = buf;
    return true;
fail:
    return false;
}

static void
destroy_import_globals(AOTImportGlobal *import_globals)
{
    wasm_runtime_free(import_globals);
}

static bool
load_import_globals(const uint8 **p_buf, const uint8 *buf_end,
                    AOTModule *module, bool is_load_from_file_buf,
                    char *error_buf, uint32 error_buf_size)
{
    const uint8 *buf = *p_buf;
    AOTImportGlobal *import_globals;
    uint64 size;
    uint32 i, data_offset = 0;
#if WASM_ENABLE_LIBC_BUILTIN != 0
    WASMGlobalImport tmp_global;
#endif

    /* Allocate memory */
    size = sizeof(AOTImportGlobal) * (uint64)module->import_global_count;
    if (!(module->import_globals = import_globals =
              loader_malloc(size, error_buf, error_buf_size))) {
        return false;
    }

    /* Create each import global */
    for (i = 0; i < module->import_global_count; i++) {
        buf = (uint8 *)align_ptr(buf, 2);
        read_uint8(buf, buf_end, import_globals[i].type.val_type);
        read_uint8(buf, buf_end, import_globals[i].type.is_mutable);
        read_string(buf, buf_end, import_globals[i].module_name);
        read_string(buf, buf_end, import_globals[i].global_name);

        if (!is_valid_value_type(import_globals[i].type.val_type)) {
            return false;
        }

#if WASM_ENABLE_LIBC_BUILTIN != 0
        if (wasm_native_lookup_libc_builtin_global(
                import_globals[i].module_name, import_globals[i].global_name,
                &tmp_global)) {
            if (tmp_global.type.val_type != import_globals[i].type.val_type
                || tmp_global.type.is_mutable
                       != import_globals[i].type.is_mutable) {
                set_error_buf(error_buf, error_buf_size,
                              "incompatible import type");
                return false;
            }
            import_globals[i].global_data_linked =
                tmp_global.global_data_linked;
            import_globals[i].is_linked = true;
        }
#else
        import_globals[i].is_linked = false;
#endif

        import_globals[i].size =
            wasm_value_type_size(import_globals[i].type.val_type);
        import_globals[i].data_offset = data_offset;
        data_offset += import_globals[i].size;
        module->global_data_size += import_globals[i].size;
    }

    *p_buf = buf;
    return true;
fail:
    return false;
}

static bool
load_import_global_info(const uint8 **p_buf, const uint8 *buf_end,
                        AOTModule *module, bool is_load_from_file_buf,
                        char *error_buf, uint32 error_buf_size)
{
    const uint8 *buf = *p_buf;

    read_uint32(buf, buf_end, module->import_global_count);

    /* load import globals */
    if (module->import_global_count > 0
        && !load_import_globals(&buf, buf_end, module, is_load_from_file_buf,
                                error_buf, error_buf_size))
        return false;

    *p_buf = buf;
    return true;
fail:
    return false;
}

static void
destroy_globals(AOTGlobal *globals)
{
    wasm_runtime_free(globals);
}

static bool
load_globals(const uint8 **p_buf, const uint8 *buf_end, AOTModule *module,
             char *error_buf, uint32 error_buf_size)
{
    const uint8 *buf = *p_buf;
    AOTGlobal *globals;
    uint64 size;
    uint32 i, data_offset = 0;
    AOTImportGlobal *last_import_global;

    /* Allocate memory */
    size = sizeof(AOTGlobal) * (uint64)module->global_count;
    if (!(module->globals = globals =
              loader_malloc(size, error_buf, error_buf_size))) {
        return false;
    }

    if (module->import_global_count > 0) {
        last_import_global =
            &module->import_globals[module->import_global_count - 1];
        data_offset =
            last_import_global->data_offset + last_import_global->size;
    }

    /* Create each global */
    for (i = 0; i < module->global_count; i++) {
        read_uint8(buf, buf_end, globals[i].type.val_type);
        read_uint8(buf, buf_end, globals[i].type.is_mutable);

        if (!is_valid_value_type(globals[i].type.val_type))
            return false;

        buf = align_ptr(buf, 4);

        if (!load_init_expr(&buf, buf_end, module, &globals[i].init_expr,
                            error_buf, error_buf_size))
            return false;

        globals[i].size = wasm_value_type_size(globals[i].type.val_type);
        globals[i].data_offset = data_offset;
        data_offset += globals[i].size;
        module->global_data_size += globals[i].size;
    }

    *p_buf = buf;
    return true;
fail:
    return false;
}

static bool
load_global_info(const uint8 **p_buf, const uint8 *buf_end, AOTModule *module,
                 char *error_buf, uint32 error_buf_size)
{
    const uint8 *buf = *p_buf;

    read_uint32(buf, buf_end, module->global_count);
    if (is_indices_overflow(module->import_global_count, module->global_count,
                            error_buf, error_buf_size))
        return false;

    /* load globals */
    if (module->global_count > 0
        && !load_globals(&buf, buf_end, module, error_buf, error_buf_size))
        return false;

    *p_buf = buf;
    return true;
fail:
    return false;
}

static void
destroy_import_funcs(AOTImportFunc *import_funcs)
{
    wasm_runtime_free(import_funcs);
}

static bool
load_import_funcs(const uint8 **p_buf, const uint8 *buf_end, AOTModule *module,
                  bool is_load_from_file_buf, bool no_resolve, char *error_buf,
                  uint32 error_buf_size)
{
    const uint8 *buf = *p_buf;
    AOTImportFunc *import_funcs;
    uint64 size;
    uint32 i;

    /* Allocate memory */
    size = sizeof(AOTImportFunc) * (uint64)module->import_func_count;
    if (!(module->import_funcs = import_funcs =
              loader_malloc(size, error_buf, error_buf_size))) {
        return false;
    }

    /* Create each import func */
    for (i = 0; i < module->import_func_count; i++) {
        read_uint16(buf, buf_end, import_funcs[i].func_type_index);
        if (import_funcs[i].func_type_index >= module->type_count) {
            set_error_buf(error_buf, error_buf_size, "unknown type");
            return false;
        }

        import_funcs[i].func_type =
            (AOTFuncType *)module->types[import_funcs[i].func_type_index];
        read_string(buf, buf_end, import_funcs[i].module_name);
        read_string(buf, buf_end, import_funcs[i].func_name);
        import_funcs[i].attachment = NULL;
        import_funcs[i].signature = NULL;
        import_funcs[i].call_conv_raw = false;

        if (!no_resolve) {
            aot_resolve_import_func(module, &import_funcs[i]);
        }

#if WASM_ENABLE_LIBC_WASI != 0
        if (!strcmp(import_funcs[i].module_name, "wasi_unstable")
            || !strcmp(import_funcs[i].module_name, "wasi_snapshot_preview1"))
            module->import_wasi_api = true;
#endif
    }

    *p_buf = buf;
    return true;
fail:
    return false;
}

static bool
load_import_func_info(const uint8 **p_buf, const uint8 *buf_end,
                      AOTModule *module, bool is_load_from_file_buf,
                      bool no_resolve, char *error_buf, uint32 error_buf_size)
{
    const uint8 *buf = *p_buf;

    read_uint32(buf, buf_end, module->import_func_count);

    /* load import funcs */
    if (module->import_func_count > 0
        && !load_import_funcs(&buf, buf_end, module, is_load_from_file_buf,
                              no_resolve, error_buf, error_buf_size))
        return false;

    *p_buf = buf;
    return true;
fail:
    return false;
}

static void
destroy_object_data_sections(AOTObjectDataSection *data_sections,
                             uint32 data_section_count)
{
    uint32 i;
    AOTObjectDataSection *data_section = data_sections;
    for (i = 0; i < data_section_count; i++, data_section++)
        if (data_section->data) {
#if WASM_ENABLE_STATIC_PGO != 0
            if (!strncmp(data_section->name, "__llvm_prf_data", 15)) {
                LLVMProfileData *data = (LLVMProfileData *)data_section->data;
                if (data->values) {
                    uint32 num_value_sites =
                        data->num_value_sites[0] + data->num_value_sites[1];
                    uint32 j;
                    for (j = 0; j < num_value_sites; j++) {
                        ValueProfNode *node = data->values[j], *node_next;
                        while (node) {
                            node_next = node->next;
                            wasm_runtime_free(node);
                            node = node_next;
                        }
                    }
                    wasm_runtime_free(data->values);
                }
            }
#endif
        }
    wasm_runtime_free(data_sections);
}

static bool
load_object_data_sections(const uint8 **p_buf, const uint8 *buf_end,
                          AOTModule *module, bool is_load_from_file_buf,
                          char *error_buf, uint32 error_buf_size)
{
    const uint8 *buf = *p_buf;
    AOTObjectDataSection *data_sections;
    uint64 size;
    uint32 i;
    uint64 total_size = 0;
    uint32 page_size = os_getpagesize();
    uint8 *merged_sections = NULL;

    /* Allocate memory */
    size = sizeof(AOTObjectDataSection) * (uint64)module->data_section_count;
    if (!(module->data_sections = data_sections =
              loader_malloc(size, error_buf, error_buf_size))) {
        return false;
    }

    /* First iteration: read data from buf, and calculate total memory needed */
    for (i = 0; i < module->data_section_count; i++) {
        read_string(buf, buf_end, data_sections[i].name);
        read_uint32(buf, buf_end, data_sections[i].size);
        CHECK_BUF(buf, buf_end, data_sections[i].size);
        /* Temporary record data ptr for merge, will be replaced after the
           merged_data_sections is mmapped */
        if (data_sections[i].size > 0)
            data_sections[i].data = (uint8 *)buf;
        buf += data_sections[i].size;
        total_size += align_uint64((uint64)data_sections[i].size, page_size);
    }
    if (total_size > UINT32_MAX) {
        set_error_buf(error_buf, error_buf_size, "data sections too large");
        return false;
    }
    if (total_size > 0) {
        /* Allocate memory for data */
        merged_sections = module->merged_data_sections =
            loader_mmap((uint32)total_size, false, error_buf, error_buf_size);
        if (!merged_sections) {
            return false;
        }
        module->merged_data_sections_size = (uint32)total_size;
    }

    /* Second iteration: Create each data section */
    for (i = 0; i < module->data_section_count; i++) {
        if (data_sections[i].size > 0) {
            bh_memcpy_s(merged_sections, data_sections[i].size,
                        data_sections[i].data, data_sections[i].size);
            data_sections[i].data = merged_sections;
            merged_sections += align_uint(data_sections[i].size, page_size);
        }
    }

    *p_buf = buf;
    return true;
fail:
    return false;
}

static bool
load_object_data_sections_info(const uint8 **p_buf, const uint8 *buf_end,
                               AOTModule *module, bool is_load_from_file_buf,
                               char *error_buf, uint32 error_buf_size)
{
    const uint8 *buf = *p_buf;

    read_uint32(buf, buf_end, module->data_section_count);

    /* load object data sections */
    if (module->data_section_count > 0
        && !load_object_data_sections(&buf, buf_end, module,
                                      is_load_from_file_buf, error_buf,
                                      error_buf_size))
        return false;

    *p_buf = buf;
    return true;
fail:
    return false;
}

static bool
load_init_data_section(const uint8 *buf, const uint8 *buf_end,
                       AOTModule *module, bool is_load_from_file_buf,
                       bool no_resolve, char *error_buf, uint32 error_buf_size)
{
    const uint8 *p = buf, *p_end = buf_end;

    if (!load_memory_info(&p, p_end, module, error_buf, error_buf_size)
        || !load_table_info(&p, p_end, module, error_buf, error_buf_size)
        || !load_type_info(&p, p_end, module, error_buf, error_buf_size)
        || !load_import_global_info(&p, p_end, module, is_load_from_file_buf,
                                    error_buf, error_buf_size)
        || !load_global_info(&p, p_end, module, error_buf, error_buf_size)
        || !load_import_func_info(&p, p_end, module, is_load_from_file_buf,
                                  no_resolve, error_buf, error_buf_size))
        return false;

    /* load function count and start function index */
    read_uint32(p, p_end, module->func_count);
    if (is_indices_overflow(module->import_func_count, module->func_count,
                            error_buf, error_buf_size))
        return false;

    read_uint32(p, p_end, module->start_func_index);

    /* check start function index */
    if (module->start_func_index != (uint32)-1
        && (module->start_func_index
            >= module->import_func_count + module->func_count)) {
        set_error_buf(error_buf, error_buf_size,
                      "invalid start function index");
        return false;
    }

    read_uint32(p, p_end, module->aux_data_end_global_index);
    read_uint64(p, p_end, module->aux_data_end);
    read_uint32(p, p_end, module->aux_heap_base_global_index);
    read_uint64(p, p_end, module->aux_heap_base);
    read_uint32(p, p_end, module->aux_stack_top_global_index);
    read_uint64(p, p_end, module->aux_stack_bottom);
    read_uint32(p, p_end, module->aux_stack_size);

    if (module->aux_data_end >= MAX_LINEAR_MEMORY_SIZE
        || module->aux_heap_base >= MAX_LINEAR_MEMORY_SIZE
        || module->aux_stack_bottom >= MAX_LINEAR_MEMORY_SIZE) {
        set_error_buf(
            error_buf, error_buf_size,
            "invalid range of aux_date_end/aux_heap_base/aux_stack_bottom");
        return false;
    }

    if (!load_object_data_sections_info(&p, p_end, module,
                                        is_load_from_file_buf, error_buf,
                                        error_buf_size))
        return false;

    if (p != p_end) {
        set_error_buf(error_buf, error_buf_size,
                      "invalid init data section size");
        return false;
    }

    return true;
fail:
    return false;
}

#if !defined(BH_PLATFORM_NUTTX) && !defined(BH_PLATFORM_ESP_IDF)
static bool
try_merge_data_and_text(const uint8 **buf, const uint8 **buf_end,
                        AOTModule *module, char *error_buf,
                        uint32 error_buf_size)
{
    uint8 *old_buf = (uint8 *)*buf;
    uint8 *old_end = (uint8 *)*buf_end;
    size_t code_size = (size_t)(old_end - old_buf);
    uint32 page_size = os_getpagesize();
    uint64 total_size = 0;
    uint32 i;
    uint8 *sections;

    if (code_size == 0) {
        return true;
    }

    /* calculate the total memory needed */
    total_size += align_uint64((uint64)code_size, page_size);
    for (i = 0; i < module->data_section_count; ++i) {
        total_size +=
            align_uint64((uint64)module->data_sections[i].size, page_size);
    }
    /* distance between .data and .text should not be greater than 4GB
       for some targets (e.g. arm64 reloc need < 4G distance) */
    if (total_size > UINT32_MAX) {
        return false;
    }
    /* code_size was checked and must be larger than 0 here */
    bh_assert(total_size > 0);

    sections = loader_mmap((uint32)total_size, false, NULL, 0);
    if (!sections) {
        /* merge failed but may be not critical for some targets */
        return false;
    }

#ifdef BH_PLATFORM_WINDOWS
    if (!os_mem_commit(sections, code_size,
                       MMAP_PROT_READ | MMAP_PROT_WRITE | MMAP_PROT_EXEC)) {
        os_munmap(sections, (uint32)total_size);
        return false;
    }
#endif

    /* change the code part to be executable */
    if (os_mprotect(sections, code_size,
                    MMAP_PROT_READ | MMAP_PROT_WRITE | MMAP_PROT_EXEC)
        != 0) {
        os_munmap(sections, (uint32)total_size);
        return false;
    }

    module->merged_data_text_sections = sections;
    module->merged_data_text_sections_size = (uint32)total_size;

    /* order not essential just as compiler does: .text section first */
    *buf = sections;
    *buf_end = sections + code_size;
    bh_memcpy_s(sections, (uint32)code_size, old_buf, (uint32)code_size);
    os_munmap(old_buf, code_size);
    sections += align_uint((uint32)code_size, page_size);

    /* then migrate .data sections */
    for (i = 0; i < module->data_section_count; ++i) {
        AOTObjectDataSection *data_section = module->data_sections + i;
        uint8 *old_data = data_section->data;
        data_section->data = sections;
        bh_memcpy_s(data_section->data, data_section->size, old_data,
                    data_section->size);
        sections += align_uint(data_section->size, page_size);
    }
    /* free the original data sections */
    if (module->merged_data_sections) {
        os_munmap(module->merged_data_sections,
                  module->merged_data_sections_size);
        module->merged_data_sections = NULL;
        module->merged_data_sections_size = 0;
    }

    return true;
}
#endif /* ! defined(BH_PLATFORM_NUTTX) && !defined(BH_PLATFORM_ESP_IDF) */

static bool
load_text_section(const uint8 *buf, const uint8 *buf_end, AOTModule *module,
                  char *error_buf, uint32 error_buf_size)
{
    uint8 *plt_base;

    if (module->func_count > 0 && buf_end == buf) {
        set_error_buf(error_buf, error_buf_size, "invalid code size");
        return false;
    }

    /* The layout is: literal size + literal + code (with plt table) */
    read_uint32(buf, buf_end, module->literal_size);

    /* literal data is at beginning of the text section */
    module->literal = (uint8 *)buf;
    module->code = (void *)(buf + module->literal_size);
    module->code_size = (uint32)(buf_end - (uint8 *)module->code);

#if WASM_ENABLE_DEBUG_AOT != 0
    module->elf_size = module->code_size;

    if (is_ELF(module->code)) {
        /* Now code points to an ELF object, we pull it down to .text section */
        uint64 offset;
        uint64 size;
        char *code_buf = module->code;
        module->elf_hdr = code_buf;
        if (!get_text_section(code_buf, &offset, &size)) {
            set_error_buf(error_buf, error_buf_size,
                          "get text section of ELF failed");
            return false;
        }
        module->code = code_buf + offset;
        module->code_size -= (uint32)offset;
    }
#endif

    if ((module->code_size > 0) && !module->is_indirect_mode) {
        plt_base = (uint8 *)buf_end - get_plt_table_size();
        init_plt_table(plt_base);
    }
    return true;
fail:
    return false;
}

static bool
load_function_section(const uint8 *buf, const uint8 *buf_end, AOTModule *module,
                      char *error_buf, uint32 error_buf_size)
{
    const uint8 *p = buf, *p_end = buf_end;
    uint32 i;
    uint64 size, text_offset;

    size = sizeof(void *) * (uint64)module->func_count;
    if (size > 0
        && !(module->func_ptrs =
                 loader_malloc(size, error_buf, error_buf_size))) {
        return false;
    }

    for (i = 0; i < module->func_count; i++) {
        if (sizeof(void *) == 8) {
            read_uint64(p, p_end, text_offset);
        }
        else {
            uint32 text_offset32;
            read_uint32(p, p_end, text_offset32);
            text_offset = text_offset32;
        }
        if (text_offset >= module->code_size) {
            set_error_buf(error_buf, error_buf_size,
                          "invalid function code offset");
            return false;
        }
        module->func_ptrs[i] = (uint8 *)module->code + text_offset;
#if defined(BUILD_TARGET_THUMB) || defined(BUILD_TARGET_THUMB_VFP)
        /* bits[0] of thumb function address must be 1 */
        module->func_ptrs[i] = (void *)((uintptr_t)module->func_ptrs[i] | 1);
#endif
    }

    /* Set start function when function pointers are resolved */
    if (module->start_func_index != (uint32)-1) {
        if (module->start_func_index >= module->import_func_count)
            module->start_function =
                module->func_ptrs[module->start_func_index
                                  - module->import_func_count];
        else
            /* TODO: fix start function can be import function issue */
            module->start_function = NULL;
    }
    else {
        module->start_function = NULL;
    }

    size = sizeof(uint32) * (uint64)module->func_count;
    if (size > 0
        && !(module->func_type_indexes =
                 loader_malloc(size, error_buf, error_buf_size))) {
        return false;
    }

    for (i = 0; i < module->func_count; i++) {
        read_uint32(p, p_end, module->func_type_indexes[i]);
        if (module->func_type_indexes[i] >= module->type_count) {
            set_error_buf(error_buf, error_buf_size, "unknown type");
            return false;
        }
    }

    size = sizeof(uint32) * (uint64)module->func_count;

    if (size > 0) {
#if WASM_ENABLE_AOT_STACK_FRAME != 0
        if (!(module->max_local_cell_nums =
                  loader_malloc(size, error_buf, error_buf_size))) {
            return false;
        }

        for (i = 0; i < module->func_count; i++) {
            read_uint32(p, p_end, module->max_local_cell_nums[i]);
        }

        if (!(module->max_stack_cell_nums =
                  loader_malloc(size, error_buf, error_buf_size))) {
            return false;
        }

        for (i = 0; i < module->func_count; i++) {
            read_uint32(p, p_end, module->max_stack_cell_nums[i]);
        }
#else
        /* Ignore max_local_cell_num and max_stack_cell_num of each function */
        CHECK_BUF(p, p_end, ((uint32)size * 2));
        p += (uint32)size * 2;
#endif
    }

#if WASM_ENABLE_GC != 0
    /* Local(params and locals) ref flags for all import and non-imported
     * functions. The flags indicate whether each cell in the AOTFrame local
     * area is a GC reference. */
    size = sizeof(LocalRefFlag)
           * (uint64)(module->import_func_count + module->func_count);
    if (size > 0) {
        if (!(module->func_local_ref_flags =
                  loader_malloc(size, error_buf, error_buf_size))) {
            return false;
        }

        for (i = 0; i < module->import_func_count + module->func_count; i++) {
            uint32 local_ref_flag_cell_num;

            buf = (uint8 *)align_ptr(buf, sizeof(uint32));
            read_uint32(
                p, p_end,
                module->func_local_ref_flags[i].local_ref_flag_cell_num);

            local_ref_flag_cell_num =
                module->func_local_ref_flags[i].local_ref_flag_cell_num;
            size = sizeof(uint8) * (uint64)local_ref_flag_cell_num;
            if (size > 0) {
                if (!(module->func_local_ref_flags[i].local_ref_flags =
                          loader_malloc(size, error_buf, error_buf_size))) {
                    return false;
                }
                read_byte_array(p, p_end,
                                module->func_local_ref_flags[i].local_ref_flags,
                                local_ref_flag_cell_num);
            }
        }
    }
#endif /* end of WASM_ENABLE_GC != 0 */

    if (p != buf_end) {
        set_error_buf(error_buf, error_buf_size,
                      "invalid function section size");
        return false;
    }

    return true;
fail:
    return false;
}

static void
destroy_exports(AOTExport *exports)
{
    wasm_runtime_free(exports);
}

static int
cmp_export_name(const void *a, const void *b)
{
    return strcmp(*(char **)a, *(char **)b);
}

static bool
check_duplicate_exports(AOTModule *module, char *error_buf,
                        uint32 error_buf_size)
{
    uint32 i;
    bool result = false;
    char *names_buf[32], **names = names_buf;
    if (module->export_count > 32) {
        names = loader_malloc(module->export_count * sizeof(char *), error_buf,
                              error_buf_size);
        if (!names) {
            return result;
        }
    }

    for (i = 0; i < module->export_count; i++) {
        names[i] = module->exports[i].name;
    }

    qsort(names, module->export_count, sizeof(char *), cmp_export_name);

    for (i = 1; i < module->export_count; i++) {
        if (!strcmp(names[i], names[i - 1])) {
            set_error_buf(error_buf, error_buf_size, "duplicate export name");
            goto cleanup;
        }
    }

    result = true;
cleanup:
    if (module->export_count > 32) {
        wasm_runtime_free(names);
    }
    return result;
}

static bool
load_exports(const uint8 **p_buf, const uint8 *buf_end, AOTModule *module,
             bool is_load_from_file_buf, char *error_buf, uint32 error_buf_size)
{
    const uint8 *buf = *p_buf;
    AOTExport *exports;
    uint64 size;
    uint32 i;

    /* Allocate memory */
    size = sizeof(AOTExport) * (uint64)module->export_count;
    if (!(module->exports = exports =
              loader_malloc(size, error_buf, error_buf_size))) {
        return false;
    }

    /* Create each export */
    for (i = 0; i < module->export_count; i++) {
        read_uint32(buf, buf_end, exports[i].index);
        read_uint8(buf, buf_end, exports[i].kind);
        read_string(buf, buf_end, exports[i].name);

        /* Check export kind and index */
        switch (exports[i].kind) {
            case EXPORT_KIND_FUNC:
                if (exports[i].index
                    >= module->import_func_count + module->func_count) {
                    set_error_buf(error_buf, error_buf_size,
                                  "unknown function");
                    return false;
                }
                break;
            case EXPORT_KIND_TABLE:
                if (exports[i].index
                    >= module->import_table_count + module->table_count) {
                    set_error_buf(error_buf, error_buf_size, "unknown table");
                    return false;
                }
                break;
            case EXPORT_KIND_MEMORY:
                if (exports[i].index
                    >= module->import_memory_count + module->memory_count) {
                    set_error_buf(error_buf, error_buf_size, "unknown memory");
                    return false;
                }
                break;
            case EXPORT_KIND_GLOBAL:
                if (exports[i].index
                    >= module->import_global_count + module->global_count) {
                    set_error_buf(error_buf, error_buf_size, "unknown global");
                    return false;
                }
                break;
#if WASM_ENABLE_TAGS != 0
                /* TODO
                case EXPORT_KIND_TAG:
                    if (index >= module->import_tag_count + module->tag_count) {
                        set_error_buf(error_buf, error_buf_size, "unknown tag");
                        return false;
                    }
                    break;
                */
#endif
            default:
                set_error_buf(error_buf, error_buf_size, "invalid export kind");
                return false;
        }
    }

    if (module->export_count > 0) {
        if (!check_duplicate_exports(module, error_buf, error_buf_size)) {
            return false;
        }
    }

    *p_buf = buf;
    return true;
fail:
    return false;
}

static bool
load_export_section(const uint8 *buf, const uint8 *buf_end, AOTModule *module,
                    bool is_load_from_file_buf, char *error_buf,
                    uint32 error_buf_size)
{
    const uint8 *p = buf, *p_end = buf_end;

    /* load export functions */
    read_uint32(p, p_end, module->export_count);
    if (module->export_count > 0
        && !load_exports(&p, p_end, module, is_load_from_file_buf, error_buf,
                         error_buf_size))
        return false;

    if (p != p_end) {
        set_error_buf(error_buf, error_buf_size, "invalid export section size");
        return false;
    }

    return true;
fail:
    return false;
}

static void *
get_data_section_addr(AOTModule *module, const char *section_name,
                      uint32 *p_data_size)
{
    uint32 i;
    AOTObjectDataSection *data_section = module->data_sections;

    for (i = 0; i < module->data_section_count; i++, data_section++) {
        if (!strcmp(data_section->name, section_name)) {
            if (p_data_size)
                *p_data_size = data_section->size;
            return data_section->data;
        }
    }

    return NULL;
}

const void *
aot_get_data_section_addr(AOTModule *module, const char *section_name,
                          uint32 *p_data_size)
{
    return get_data_section_addr(module, section_name, p_data_size);
}

static void *
resolve_target_sym(const char *symbol, int32 *p_index)
{
    uint32 i, num = 0;
    SymbolMap *target_sym_map;

    if (!(target_sym_map = get_target_symbol_map(&num)))
        return NULL;

    for (i = 0; i < num; i++) {
        if (!strcmp(target_sym_map[i].symbol_name, symbol)
#if defined(_WIN32) || defined(_WIN32_)
            /* In Win32, the symbol name of function added by
               LLVMAddFunction() is prefixed by '_', ignore it */
            || (strlen(symbol) > 1 && symbol[0] == '_'
                && !strcmp(target_sym_map[i].symbol_name, symbol + 1))
#endif
        ) {
            *p_index = (int32)i;
            return target_sym_map[i].symbol_addr;
        }
    }
    return NULL;
}

static bool
is_literal_relocation(const char *reloc_sec_name)
{
    return !strcmp(reloc_sec_name, ".rela.literal");
}

static bool
str2uint32(const char *buf, uint32 *p_res)
{
    uint32 res = 0, val;
    const char *buf_end = buf + 8;
    char ch;

    while (buf < buf_end) {
        ch = *buf++;
        if (ch >= '0' && ch <= '9')
            val = ch - '0';
        else if (ch >= 'a' && ch <= 'f')
            val = ch - 'a' + 0xA;
        else if (ch >= 'A' && ch <= 'F')
            val = ch - 'A' + 0xA;
        else
            return false;
        res = (res << 4) | val;
    }
    *p_res = res;
    return true;
}

static bool
str2uint64(const char *buf, uint64 *p_res)
{
    uint64 res = 0, val;
    const char *buf_end = buf + 16;
    char ch;

    while (buf < buf_end) {
        ch = *buf++;
        if (ch >= '0' && ch <= '9')
            val = ch - '0';
        else if (ch >= 'a' && ch <= 'f')
            val = ch - 'a' + 0xA;
        else if (ch >= 'A' && ch <= 'F')
            val = ch - 'A' + 0xA;
        else
            return false;
        res = (res << 4) | val;
    }
    *p_res = res;
    return true;
}

#define R_X86_64_GOTPCREL 9 /* 32 bit signed PC relative offset to GOT */

static bool
is_text_section(const char *section_name)
{
    return !strcmp(section_name, ".text") || !strcmp(section_name, ".ltext");
}

static bool
do_text_relocation(AOTModule *module, AOTRelocationGroup *group,
                   char *error_buf, uint32 error_buf_size)
{
    bool is_literal = is_literal_relocation(group->section_name);
    uint8 *aot_text = is_literal ? module->literal : module->code;
    uint32 aot_text_size =
        is_literal ? module->literal_size : module->code_size;
    uint32 i, func_index, symbol_len;
#if defined(BH_PLATFORM_WINDOWS)
    uint32 ymm_plt_index = 0, xmm_plt_index = 0;
    uint32 real_plt_index = 0, float_plt_index = 0, j;
#endif
    char symbol_buf[128] = { 0 }, *symbol, *p;
    void *symbol_addr;
    AOTRelocation *relocation = group->relocations;

    if (group->relocation_count > 0 && !aot_text) {
        set_error_buf(error_buf, error_buf_size,
                      "invalid text relocation count");
        return false;
    }

    for (i = 0; i < group->relocation_count; i++, relocation++) {
        int32 symbol_index = -1;
        symbol_len = (uint32)strlen(relocation->symbol_name);
        if (symbol_len + 1 <= sizeof(symbol_buf))
            symbol = symbol_buf;
        else {
            if (!(symbol = loader_malloc(symbol_len + 1, error_buf,
                                         error_buf_size))) {
                return false;
            }
        }
        bh_memcpy_s(symbol, symbol_len, relocation->symbol_name, symbol_len);
        symbol[symbol_len] = '\0';

#if WASM_ENABLE_STATIC_PGO != 0
        if (!strcmp(symbol, "__llvm_profile_runtime")
            || !strcmp(symbol, "__llvm_profile_register_function")
            || !strcmp(symbol, "__llvm_profile_register_names_function")) {
            continue;
        }
#endif

        if (!strncmp(symbol, AOT_FUNC_PREFIX, strlen(AOT_FUNC_PREFIX))) {
            p = symbol + strlen(AOT_FUNC_PREFIX);
            if (*p == '\0'
                || (func_index = (uint32)atoi(p)) > module->func_count) {
                set_error_buf_v(error_buf, error_buf_size,
                                "invalid import symbol %s", symbol);
                goto check_symbol_fail;
            }
#if (defined(BUILD_TARGET_X86_64) || defined(BUILD_TARGET_AMD_64)) \
    && !defined(BH_PLATFORM_WINDOWS)
            if (relocation->relocation_type == R_X86_64_GOTPCREL) {
                GOTItem *got_item = module->got_item_list;
                uint32 got_item_idx = 0;

                while (got_item) {
                    if (got_item->func_idx == func_index)
                        break;
                    got_item_idx++;
                    got_item = got_item->next;
                }
                /* Calculate `GOT + G` */
                symbol_addr = module->got_func_ptrs + got_item_idx;
            }
            else
                symbol_addr = module->func_ptrs[func_index];
#else
            symbol_addr = module->func_ptrs[func_index];
#endif
        }
#if defined(BH_PLATFORM_WINDOWS) && defined(BUILD_TARGET_X86_32)
        /* AOT function name starts with '_' in windows x86-32 */
        else if (!strncmp(symbol, "_" AOT_FUNC_PREFIX,
                          strlen("_" AOT_FUNC_PREFIX))) {
            p = symbol + strlen("_" AOT_FUNC_PREFIX);
            if (*p == '\0'
                || (func_index = (uint32)atoi(p)) > module->func_count) {
                set_error_buf_v(error_buf, error_buf_size, "invalid symbol %s",
                                symbol);
                goto check_symbol_fail;
            }
            symbol_addr = module->func_ptrs[func_index];
        }
        else if (!strncmp(symbol, "_" AOT_FUNC_INTERNAL_PREFIX,
                          strlen("_" AOT_FUNC_INTERNAL_PREFIX))) {
            p = symbol + strlen("_" AOT_FUNC_INTERNAL_PREFIX);
            if (*p == '\0'
                || (func_index = (uint32)atoi(p)) > module->func_count) {
                set_error_buf_v(error_buf, error_buf_size, "invalid symbol %s",
                                symbol);
                goto check_symbol_fail;
            }
            symbol_addr = module->func_ptrs[func_index];
        }
#endif
        else if (is_text_section(symbol)) {
            symbol_addr = module->code;
        }
        else if (!strcmp(symbol, ".data") || !strcmp(symbol, ".sdata")
                 || !strcmp(symbol, ".rdata") || !strcmp(symbol, ".rodata")
                 || !strcmp(symbol, ".srodata")
                 /* ".rodata.cst4/8/16/.." */
                 || !strncmp(symbol, ".rodata.cst", strlen(".rodata.cst"))
                 /* ".srodata.cst4/8/16/.." */
                 || !strncmp(symbol, ".srodata.cst", strlen(".srodata.cst"))
                 /* ".rodata.strn.m" */
                 || !strncmp(symbol, ".rodata.str", strlen(".rodata.str"))
                 || !strcmp(symbol, AOT_STACK_SIZES_SECTION_NAME)
#if WASM_ENABLE_STATIC_PGO != 0
                 || !strncmp(symbol, "__llvm_prf_cnts", 15)
                 || !strncmp(symbol, "__llvm_prf_data", 15)
                 || !strncmp(symbol, "__llvm_prf_names", 16)
#endif
        ) {
            symbol_addr = get_data_section_addr(module, symbol, NULL);
            if (!symbol_addr) {
                set_error_buf_v(error_buf, error_buf_size,
                                "invalid data section (%s)", symbol);
                goto check_symbol_fail;
            }
        }
        else if (!strcmp(symbol, ".literal")) {
            symbol_addr = module->literal;
        }
#if defined(BH_PLATFORM_WINDOWS)
        /* Relocation for symbols which start with "__ymm@", "__xmm@" or
           "__real@" and end with the ymm value, xmm value or real value.
           In Windows PE file, the data is stored in some individual ".rdata"
           sections. We simply create extra plt data, parse the values from
           the symbols and stored them into the extra plt data. */
        else if (!strcmp(group->section_name, ".text")
                 && !strncmp(symbol, YMM_PLT_PREFIX, strlen(YMM_PLT_PREFIX))
                 && strlen(symbol) == strlen(YMM_PLT_PREFIX) + 64) {
            char ymm_buf[17] = { 0 };

            symbol_addr = module->extra_plt_data + ymm_plt_index * 32;
            for (j = 0; j < 4; j++) {
                bh_memcpy_s(ymm_buf, sizeof(ymm_buf),
                            symbol + strlen(YMM_PLT_PREFIX) + 48 - 16 * j, 16);
                if (!str2uint64(ymm_buf,
                                (uint64 *)((uint8 *)symbol_addr + 8 * j))) {
                    set_error_buf_v(error_buf, error_buf_size,
                                    "resolve symbol %s failed", symbol);
                    goto check_symbol_fail;
                }
            }
            ymm_plt_index++;
        }
        else if (!strcmp(group->section_name, ".text")
                 && !strncmp(symbol, XMM_PLT_PREFIX, strlen(XMM_PLT_PREFIX))
                 && strlen(symbol) == strlen(XMM_PLT_PREFIX) + 32) {
            char xmm_buf[17] = { 0 };

            symbol_addr = module->extra_plt_data + module->ymm_plt_count * 32
                          + xmm_plt_index * 16;
            for (j = 0; j < 2; j++) {
                bh_memcpy_s(xmm_buf, sizeof(xmm_buf),
                            symbol + strlen(XMM_PLT_PREFIX) + 16 - 16 * j, 16);
                if (!str2uint64(xmm_buf,
                                (uint64 *)((uint8 *)symbol_addr + 8 * j))) {
                    set_error_buf_v(error_buf, error_buf_size,
                                    "resolve symbol %s failed", symbol);
                    goto check_symbol_fail;
                }
            }
            xmm_plt_index++;
        }
        else if (!strcmp(group->section_name, ".text")
                 && !strncmp(symbol, REAL_PLT_PREFIX, strlen(REAL_PLT_PREFIX))
                 && strlen(symbol) == strlen(REAL_PLT_PREFIX) + 16) {
            char real_buf[17] = { 0 };

            symbol_addr = module->extra_plt_data + module->ymm_plt_count * 32
                          + module->xmm_plt_count * 16 + real_plt_index * 8;
            bh_memcpy_s(real_buf, sizeof(real_buf),
                        symbol + strlen(REAL_PLT_PREFIX), 16);
            if (!str2uint64(real_buf, (uint64 *)symbol_addr)) {
                set_error_buf_v(error_buf, error_buf_size,
                                "resolve symbol %s failed", symbol);
                goto check_symbol_fail;
            }
            real_plt_index++;
        }
        else if (!strcmp(group->section_name, ".text")
                 && !strncmp(symbol, REAL_PLT_PREFIX, strlen(REAL_PLT_PREFIX))
                 && strlen(symbol) == strlen(REAL_PLT_PREFIX) + 8) {
            char float_buf[9] = { 0 };

            symbol_addr = module->extra_plt_data + module->ymm_plt_count * 32
                          + module->xmm_plt_count * 16
                          + module->real_plt_count * 8 + float_plt_index * 4;
            bh_memcpy_s(float_buf, sizeof(float_buf),
                        symbol + strlen(REAL_PLT_PREFIX), 8);
            if (!str2uint32(float_buf, (uint32 *)symbol_addr)) {
                set_error_buf_v(error_buf, error_buf_size,
                                "resolve symbol %s failed", symbol);
                goto check_symbol_fail;
            }
            float_plt_index++;
        }
#endif /* end of defined(BH_PLATFORM_WINDOWS) */
        else if (!(symbol_addr = resolve_target_sym(symbol, &symbol_index))) {
            set_error_buf_v(error_buf, error_buf_size,
                            "resolve symbol %s failed", symbol);
            goto check_symbol_fail;
        }

        if (symbol != symbol_buf)
            wasm_runtime_free(symbol);

        if (!apply_relocation(
                module, aot_text, aot_text_size, relocation->relocation_offset,
                relocation->relocation_addend, relocation->relocation_type,
                symbol_addr, symbol_index, error_buf, error_buf_size))
            return false;
    }

    return true;

check_symbol_fail:
    if (symbol != symbol_buf)
        wasm_runtime_free(symbol);
    return false;
}

static bool
do_data_relocation(AOTModule *module, AOTRelocationGroup *group,
                   char *error_buf, uint32 error_buf_size)

{
    uint8 *data_addr;
    uint32 data_size = 0, i;
    AOTRelocation *relocation = group->relocations;
    void *symbol_addr = NULL;
    char *symbol, *data_section_name;

    if (!strncmp(group->section_name, ".rela.", 6)) {
        data_section_name = group->section_name + strlen(".rela");
    }
    else if (!strncmp(group->section_name, ".rel.", 5)) {
        data_section_name = group->section_name + strlen(".rel");
    }
    else if (!strcmp(group->section_name, ".rdata")) {
        data_section_name = group->section_name;
    }
#if WASM_ENABLE_STATIC_PGO != 0
    else if (!strncmp(group->section_name, ".rel__llvm_prf_data", 19)) {
        data_section_name = group->section_name + strlen(".rel");
    }
    else if (!strncmp(group->section_name, ".rela__llvm_prf_data", 20)) {
        data_section_name = group->section_name + strlen(".rela");
    }
#endif
    else {
        set_error_buf(error_buf, error_buf_size,
                      "invalid data relocation section name");
        return false;
    }

    data_addr = get_data_section_addr(module, data_section_name, &data_size);

    if (group->relocation_count > 0 && !data_addr) {
        set_error_buf(error_buf, error_buf_size,
                      "invalid data relocation count");
        return false;
    }

    for (i = 0; i < group->relocation_count; i++, relocation++) {
        symbol = relocation->symbol_name;
        if (is_text_section(symbol)) {
            symbol_addr = module->code;
        }
#if WASM_ENABLE_STATIC_PGO != 0
        else if (!strncmp(symbol, AOT_FUNC_PREFIX, strlen(AOT_FUNC_PREFIX))) {
            char *p = symbol + strlen(AOT_FUNC_PREFIX);
            uint32 func_index;
            if (*p == '\0'
                || (func_index = (uint32)atoi(p)) > module->func_count) {
                set_error_buf_v(error_buf, error_buf_size,
                                "invalid relocation symbol %s", symbol);
                return false;
            }
            symbol_addr = module->func_ptrs[func_index];
        }
        else if (!strcmp(symbol, "__llvm_prf_cnts")) {
            uint32 j;
            for (j = 0; j < module->data_section_count; j++) {
                if (!strncmp(module->data_sections[j].name, symbol, 15)) {
                    bh_assert(relocation->relocation_addend + sizeof(uint64)
                              <= module->data_sections[j].size);
                    symbol_addr = module->data_sections[j].data;
                    break;
                }
            }
            if (j == module->data_section_count) {
                set_error_buf_v(error_buf, error_buf_size,
                                "invalid relocation symbol %s", symbol);
                return false;
            }
        }
        else if (!strncmp(symbol, "__llvm_prf_cnts", 15)) {
            uint32 j;
            for (j = 0; j < module->data_section_count; j++) {
                if (!strcmp(module->data_sections[j].name, symbol)) {
                    symbol_addr = module->data_sections[j].data;
                    break;
                }
            }
            if (j == module->data_section_count) {
                set_error_buf_v(error_buf, error_buf_size,
                                "invalid relocation symbol %s", symbol);
                return false;
            }
        }
#endif /* end of WASM_ENABLE_STATIC_PGO != 0 */
        else {
            set_error_buf_v(error_buf, error_buf_size,
                            "invalid relocation symbol %s", symbol);
            return false;
        }

        if (!apply_relocation(
                module, data_addr, data_size, relocation->relocation_offset,
                relocation->relocation_addend, relocation->relocation_type,
                symbol_addr, -1, error_buf, error_buf_size))
            return false;
    }

    return true;
}

static bool
validate_symbol_table(uint8 *buf, uint8 *buf_end, uint32 *offsets, uint32 count,
                      char *error_buf, uint32 error_buf_size)
{
    uint32 i, str_len_addr = 0;
    uint16 str_len;

    for (i = 0; i < count; i++) {
        if (offsets[i] != str_len_addr)
            return false;

        read_uint16(buf, buf_end, str_len);
        str_len_addr += (uint32)sizeof(uint16) + str_len;
        str_len_addr = align_uint(str_len_addr, 2);
        buf += str_len;
        buf = (uint8 *)align_ptr(buf, 2);
    }

    if (buf == buf_end)
        return true;
fail:
    return false;
}

static bool
load_relocation_section(const uint8 *buf, const uint8 *buf_end,
                        AOTModule *module, bool is_load_from_file_buf,
                        char *error_buf, uint32 error_buf_size)
{
    AOTRelocationGroup *groups = NULL, *group;
    uint32 symbol_count = 0;
    uint32 group_count = 0, i, j, got_item_count = 0;
    uint64 size;
    uint32 *symbol_offsets, total_string_len;
    uint8 *symbol_buf, *symbol_buf_end;
    int map_prot, map_flags;
    bool ret = false;
    char **symbols = NULL;

    read_uint32(buf, buf_end, symbol_count);

    symbol_offsets = (uint32 *)buf;
    for (i = 0; i < symbol_count; i++) {
        CHECK_BUF(buf, buf_end, sizeof(uint32));
        buf += sizeof(uint32);
    }

    read_uint32(buf, buf_end, total_string_len);
    symbol_buf = (uint8 *)buf;
    symbol_buf_end = symbol_buf + total_string_len;

    if (!validate_symbol_table(symbol_buf, symbol_buf_end, symbol_offsets,
                               symbol_count, error_buf, error_buf_size)) {
        set_error_buf(error_buf, error_buf_size,
                      "validate symbol table failed");
        goto fail;
    }

    if (symbol_count > 0) {
        symbols = loader_malloc((uint64)sizeof(*symbols) * symbol_count,
                                error_buf, error_buf_size);
        if (symbols == NULL) {
            goto fail;
        }
    }

#if defined(BH_PLATFORM_WINDOWS)
    buf = symbol_buf_end;
    read_uint32(buf, buf_end, group_count);

    for (i = 0; i < group_count; i++) {
        uint32 name_index, relocation_count;
        uint16 group_name_len;
        uint8 *group_name;

        /* section name address is 4 bytes aligned. */
        buf = (uint8 *)align_ptr(buf, sizeof(uint32));
        read_uint32(buf, buf_end, name_index);

        if (name_index >= symbol_count) {
            set_error_buf(error_buf, error_buf_size,
                          "symbol index out of range");
            goto fail;
        }

        group_name = symbol_buf + symbol_offsets[name_index];
        group_name_len = *(uint16 *)group_name;
        group_name += sizeof(uint16);

        read_uint32(buf, buf_end, relocation_count);

        for (j = 0; j < relocation_count; j++) {
            AOTRelocation relocation = { 0 };
            char group_name_buf[128] = { 0 };
            char symbol_name_buf[128] = { 0 };
            uint32 symbol_index, offset32;
            int32 addend32;
            uint16 symbol_name_len;
            uint8 *symbol_name;

            if (sizeof(void *) == 8) {
                read_uint64(buf, buf_end, relocation.relocation_offset);
                read_uint64(buf, buf_end, relocation.relocation_addend);
            }
            else {
                read_uint32(buf, buf_end, offset32);
                relocation.relocation_offset = (uint64)offset32;
                read_uint32(buf, buf_end, addend32);
                relocation.relocation_addend = (int64)addend32;
            }
            read_uint32(buf, buf_end, relocation.relocation_type);
            read_uint32(buf, buf_end, symbol_index);

            if (symbol_index >= symbol_count) {
                set_error_buf(error_buf, error_buf_size,
                              "symbol index out of range");
                goto fail;
            }

            symbol_name = symbol_buf + symbol_offsets[symbol_index];
            symbol_name_len = *(uint16 *)symbol_name;
            symbol_name += sizeof(uint16);

            bh_memcpy_s(group_name_buf, (uint32)sizeof(group_name_buf),
                        group_name, group_name_len);
            bh_memcpy_s(symbol_name_buf, (uint32)sizeof(symbol_name_buf),
                        symbol_name, symbol_name_len);

            /* aot compiler emits string with '\0' since 2.0.0 */
            if (group_name_len == strlen(".text") + 1
                && !strncmp(group_name, ".text", strlen(".text"))) {
                /* aot compiler emits string with '\0' since 2.0.0 */
                if (symbol_name_len == strlen(YMM_PLT_PREFIX) + 64 + 1
                    && !strncmp(symbol_name, YMM_PLT_PREFIX,
                                strlen(YMM_PLT_PREFIX))) {
                    module->ymm_plt_count++;
                }
                else if (symbol_name_len == strlen(XMM_PLT_PREFIX) + 32 + 1
                         && !strncmp(symbol_name, XMM_PLT_PREFIX,
                                     strlen(XMM_PLT_PREFIX))) {
                    module->xmm_plt_count++;
                }
                else if (symbol_name_len == strlen(REAL_PLT_PREFIX) + 16 + 1
                         && !strncmp(symbol_name, REAL_PLT_PREFIX,
                                     strlen(REAL_PLT_PREFIX))) {
                    module->real_plt_count++;
                }
                else if (symbol_name_len >= strlen(REAL_PLT_PREFIX) + 8 + 1
                         && !strncmp(symbol_name, REAL_PLT_PREFIX,
                                     strlen(REAL_PLT_PREFIX))) {
                    module->float_plt_count++;
                }
            }
        }
    }

    /* Allocate memory for extra plt data */
    size = sizeof(uint64) * 4 * module->ymm_plt_count
           + sizeof(uint64) * 2 * module->xmm_plt_count
           + sizeof(uint64) * module->real_plt_count
           + sizeof(uint32) * module->float_plt_count;
    if (size > 0) {
        if (size > UINT32_MAX
            || !(module->extra_plt_data = loader_mmap(
                     (uint32)size, true, error_buf, error_buf_size))) {
            goto fail;
        }
        module->extra_plt_data_size = (uint32)size;
    }
#endif /* end of defined(BH_PLATFORM_WINDOWS) */

#if (defined(BUILD_TARGET_X86_64) || defined(BUILD_TARGET_AMD_64)) \
    && !defined(BH_PLATFORM_WINDOWS)
    buf = symbol_buf_end;
    read_uint32(buf, buf_end, group_count);

    /* Resolve the relocations of type R_X86_64_GOTPCREL */
    for (i = 0; i < group_count; i++) {
        uint32 name_index, relocation_count;
        uint16 group_name_len;
        uint8 *group_name;

        /* section name address is 4 bytes aligned. */
        buf = (uint8 *)align_ptr(buf, sizeof(uint32));
        read_uint32(buf, buf_end, name_index);

        if (name_index >= symbol_count) {
            set_error_buf(error_buf, error_buf_size,
                          "symbol index out of range");
            goto fail;
        }

        group_name = symbol_buf + symbol_offsets[name_index];
        group_name_len = *(uint16 *)group_name;
        group_name += sizeof(uint16);

        read_uint32(buf, buf_end, relocation_count);

        for (j = 0; j < relocation_count; j++) {
            AOTRelocation relocation = { 0 };
            char group_name_buf[128] = { 0 };
            char symbol_name_buf[128] = { 0 };
            uint32 symbol_index;
            uint16 symbol_name_len;
            uint8 *symbol_name;

            /* relocation offset and addend */
            buf += sizeof(void *) * 2;

            read_uint32(buf, buf_end, relocation.relocation_type);
            read_uint32(buf, buf_end, symbol_index);

            if (symbol_index >= symbol_count) {
                set_error_buf(error_buf, error_buf_size,
                              "symbol index out of range");
                goto fail;
            }

            symbol_name = symbol_buf + symbol_offsets[symbol_index];
            symbol_name_len = *(uint16 *)symbol_name;
            symbol_name += sizeof(uint16);

            bh_memcpy_s(group_name_buf, (uint32)sizeof(group_name_buf),
                        group_name, group_name_len);
            bh_memcpy_s(symbol_name_buf, (uint32)sizeof(symbol_name_buf),
                        symbol_name, symbol_name_len);

            if (relocation.relocation_type == R_X86_64_GOTPCREL
                && !strncmp(symbol_name_buf, AOT_FUNC_PREFIX,
                            strlen(AOT_FUNC_PREFIX))) {
                uint32 func_idx =
                    atoi(symbol_name_buf + strlen(AOT_FUNC_PREFIX));
                GOTItem *got_item = module->got_item_list;

                if (func_idx >= module->func_count) {
                    set_error_buf(error_buf, error_buf_size,
                                  "func index out of range");
                    goto fail;
                }

                while (got_item) {
                    if (got_item->func_idx == func_idx)
                        break;
                    got_item = got_item->next;
                }

                if (!got_item) {
                    /* Create the got item and append to the list */
                    got_item = wasm_runtime_malloc(sizeof(GOTItem));
                    if (!got_item) {
                        set_error_buf(error_buf, error_buf_size,
                                      "allocate memory failed");
                        goto fail;
                    }

                    got_item->func_idx = func_idx;
                    got_item->next = NULL;
                    if (!module->got_item_list) {
                        module->got_item_list = module->got_item_list_end =
                            got_item;
                    }
                    else {
                        module->got_item_list_end->next = got_item;
                        module->got_item_list_end = got_item;
                    }

                    got_item_count++;
                }
            }
        }
    }

    if (got_item_count) {
        GOTItem *got_item = module->got_item_list;
        uint32 got_item_idx = 0;

        /* Create the GOT for func_ptrs, note that it is different from
           the .got section of a dynamic object file */
        size = (uint64)sizeof(void *) * got_item_count;
        if (size > UINT32_MAX
            || !(module->got_func_ptrs = loader_mmap(
                     (uint32)size, false, error_buf, error_buf_size))) {
            goto fail;
        }

        while (got_item) {
            module->got_func_ptrs[got_item_idx++] =
                module->func_ptrs[got_item->func_idx];
            got_item = got_item->next;
        }

        module->got_item_count = got_item_count;
    }
#else
    (void)got_item_count;
#endif /* (defined(BUILD_TARGET_X86_64) || defined(BUILD_TARGET_AMD_64)) && \
          !defined(BH_PLATFORM_WINDOWS) */

    buf = symbol_buf_end;
    read_uint32(buf, buf_end, group_count);

    /* Allocate memory for relocation groups */
    size = sizeof(AOTRelocationGroup) * (uint64)group_count;
    if (size > 0
        && !(groups = loader_malloc(size, error_buf, error_buf_size))) {
        goto fail;
    }

    /* Load each relocation group */
    for (i = 0, group = groups; i < group_count; i++, group++) {
        AOTRelocation *relocation;
        uint32 name_index;

        /* section name address is 4 bytes aligned. */
        buf = (uint8 *)align_ptr(buf, sizeof(uint32));
        read_uint32(buf, buf_end, name_index);

        if (name_index >= symbol_count) {
            set_error_buf(error_buf, error_buf_size,
                          "symbol index out of range");
            goto fail;
        }

        if (symbols[name_index] == NULL) {
            uint8 *name_addr = symbol_buf + symbol_offsets[name_index];

            read_string(name_addr, buf_end, symbols[name_index]);
        }
        group->section_name = symbols[name_index];

        read_uint32(buf, buf_end, group->relocation_count);

        /* Allocate memory for relocations */
        size = sizeof(AOTRelocation) * (uint64)group->relocation_count;
        if (!(group->relocations = relocation =
                  loader_malloc(size, error_buf, error_buf_size))) {
            ret = false;
            goto fail;
        }

        /* Load each relocation */
        for (j = 0; j < group->relocation_count; j++, relocation++) {
            uint32 symbol_index;

            if (sizeof(void *) == 8) {
                read_uint64(buf, buf_end, relocation->relocation_offset);
                read_uint64(buf, buf_end, relocation->relocation_addend);
            }
            else {
                uint32 offset32, addend32;
                read_uint32(buf, buf_end, offset32);
                relocation->relocation_offset = (uint64)offset32;
                read_uint32(buf, buf_end, addend32);
                relocation->relocation_addend = (uint64)addend32;
            }
            read_uint32(buf, buf_end, relocation->relocation_type);
            read_uint32(buf, buf_end, symbol_index);

            if (symbol_index >= symbol_count) {
                set_error_buf(error_buf, error_buf_size,
                              "symbol index out of range");
                goto fail;
            }

            if (symbols[symbol_index] == NULL) {
                uint8 *symbol_addr = symbol_buf + symbol_offsets[symbol_index];

                read_string(symbol_addr, buf_end, symbols[symbol_index]);
            }
            relocation->symbol_name = symbols[symbol_index];
        }

        if (!strcmp(group->section_name, ".rel.text")
            || !strcmp(group->section_name, ".rela.text")
            || !strcmp(group->section_name, ".rel.ltext")
            || !strcmp(group->section_name, ".rela.ltext")
            || !strcmp(group->section_name, ".rela.literal")
#ifdef BH_PLATFORM_WINDOWS
            || !strcmp(group->section_name, ".text")
#endif
        ) {
#if !defined(BH_PLATFORM_LINUX) && !defined(BH_PLATFORM_LINUX_SGX) \
    && !defined(BH_PLATFORM_DARWIN) && !defined(BH_PLATFORM_WINDOWS)
            if (module->is_indirect_mode) {
                set_error_buf(error_buf, error_buf_size,
                              "cannot apply relocation to text section "
                              "for aot file generated with "
                              "\"--enable-indirect-mode\" flag");
                goto fail;
            }
#endif
            if (!do_text_relocation(module, group, error_buf, error_buf_size))
                goto fail;
        }
        else {
            if (!do_data_relocation(module, group, error_buf, error_buf_size))
                goto fail;
        }
    }

    /* Set read only for AOT code and some data sections */
    map_prot = MMAP_PROT_READ | MMAP_PROT_EXEC;

    if (module->code) {
        /* The layout is: literal size + literal + code (with plt table) */
        uint8 *mmap_addr = module->literal - sizeof(uint32);
        uint32 total_size =
            sizeof(uint32) + module->literal_size + module->code_size;
        os_mprotect(mmap_addr, total_size, map_prot);
    }

    map_prot = MMAP_PROT_READ;

#if defined(BH_PLATFORM_WINDOWS)
    if (module->extra_plt_data) {
        os_mprotect(module->extra_plt_data, module->extra_plt_data_size,
                    map_prot);
    }
#endif

    for (i = 0; i < module->data_section_count; i++) {
        AOTObjectDataSection *data_section = module->data_sections + i;
        if (!strcmp(data_section->name, ".rdata")
            || !strcmp(data_section->name, ".rodata")
            /* ".rodata.cst4/8/16/.." */
            || !strncmp(data_section->name, ".rodata.cst",
                        strlen(".rodata.cst"))
            /* ".rodata.strn.m" */
            || !strncmp(data_section->name, ".rodata.str",
                        strlen(".rodata.str"))) {
            os_mprotect(data_section->data, data_section->size, map_prot);
        }
    }

    ret = true;

fail:
    if (symbols) {
        wasm_runtime_free(symbols);
    }
    if (groups) {
        for (i = 0, group = groups; i < group_count; i++, group++)
            if (group->relocations)
                wasm_runtime_free(group->relocations);
        wasm_runtime_free(groups);
    }

    (void)map_flags;
    return ret;
}

#if WASM_ENABLE_MEMORY64 != 0
static bool
has_module_memory64(AOTModule *module)
{
    /* TODO: multi-memories for now assuming the memory idx type is consistent
     * across multi-memories */
    if (module->import_memory_count > 0)
        return !!(module->import_memories[0].mem_type.flags & MEMORY64_FLAG);
    else if (module->memory_count > 0)
        return !!(module->memories[0].flags & MEMORY64_FLAG);

    return false;
}
#endif

static bool
load_from_sections(AOTModule *module, AOTSection *sections,
                   bool is_load_from_file_buf, bool no_resolve, char *error_buf,
                   uint32 error_buf_size)
{
    AOTSection *section = sections;
    const uint8 *buf, *buf_end;
    uint32 last_section_type = (uint32)-1, section_type;
    uint32 i, func_index, func_type_index;
    AOTFuncType *func_type;
    AOTExport *exports;
    uint8 malloc_free_io_type = VALUE_TYPE_I32;

    while (section) {
        buf = section->section_body;
        buf_end = buf + section->section_body_size;
        /* Check sections */
        section_type = (uint32)section->section_type;
        if ((last_section_type == (uint32)-1
             && section_type != AOT_SECTION_TYPE_TARGET_INFO)
            || (last_section_type != (uint32)-1
                && (section_type != last_section_type + 1
                    && section_type != AOT_SECTION_TYPE_CUSTOM))) {
            set_error_buf(error_buf, error_buf_size, "invalid section order");
            return false;
        }
        last_section_type = section_type;
        switch (section_type) {
            case AOT_SECTION_TYPE_TARGET_INFO:
                if (!load_target_info_section(buf, buf_end, module, error_buf,
                                              error_buf_size))
                    return false;
                break;
            case AOT_SECTION_TYPE_INIT_DATA:
                if (!load_init_data_section(buf, buf_end, module,
                                            is_load_from_file_buf, no_resolve,
                                            error_buf, error_buf_size))
                    return false;
                break;
            case AOT_SECTION_TYPE_TEXT:
#if !defined(BH_PLATFORM_NUTTX) && !defined(BH_PLATFORM_ESP_IDF)
                /* try to merge .data and .text, with exceptions:
                 * 1. XIP mode
                 * 2. pre-mmapped module load from aot_load_from_sections()
                 * 3. nuttx & esp-idf: have separate region for MMAP_PROT_EXEC
                 */
                if (!module->is_indirect_mode && is_load_from_file_buf)
                    if (!try_merge_data_and_text(&buf, &buf_end, module,
                                                 error_buf, error_buf_size))
                        LOG_WARNING("merge .data and .text sections failed");
#endif /* ! defined(BH_PLATFORM_NUTTX) && !defined(BH_PLATFORM_ESP_IDF) */
                if (!load_text_section(buf, buf_end, module, error_buf,
                                       error_buf_size))
                    return false;
                break;
            case AOT_SECTION_TYPE_FUNCTION:
                if (!load_function_section(buf, buf_end, module, error_buf,
                                           error_buf_size))
                    return false;
                break;
            case AOT_SECTION_TYPE_EXPORT:
                if (!load_export_section(buf, buf_end, module,
                                         is_load_from_file_buf, error_buf,
                                         error_buf_size))
                    return false;
                break;
            case AOT_SECTION_TYPE_RELOCATION:
                if (!load_relocation_section(buf, buf_end, module,
                                             is_load_from_file_buf, error_buf,
                                             error_buf_size))
                    return false;
                break;
            case AOT_SECTION_TYPE_CUSTOM:
                if (!load_custom_section(buf, buf_end, module,
                                         is_load_from_file_buf, error_buf,
                                         error_buf_size))
                    return false;
                break;
            default:
                set_error_buf(error_buf, error_buf_size,
                              "invalid aot section type");
                return false;
        }

        section = section->next;
    }

    if (last_section_type != AOT_SECTION_TYPE_RELOCATION
        && last_section_type != AOT_SECTION_TYPE_CUSTOM) {
        set_error_buf(error_buf, error_buf_size, "section missing");
        return false;
    }

    /* Resolve malloc and free function */
    module->malloc_func_index = (uint32)-1;
    module->free_func_index = (uint32)-1;
    module->retain_func_index = (uint32)-1;
#if WASM_ENABLE_MEMORY64 != 0
    if (has_module_memory64(module))
        malloc_free_io_type = VALUE_TYPE_I64;
#endif
    exports = module->exports;
    for (i = 0; i < module->export_count; i++) {
        if (exports[i].kind == EXPORT_KIND_FUNC
            && exports[i].index >= module->import_func_count) {
            if (!strcmp(exports[i].name, "malloc")) {
                func_index = exports[i].index - module->import_func_count;
                func_type_index = module->func_type_indexes[func_index];
                func_type = (AOTFuncType *)module->types[func_type_index];
                if (func_type->param_count == 1 && func_type->result_count == 1
                    && func_type->types[0] == malloc_free_io_type
                    && func_type->types[1] == malloc_free_io_type) {
                    bh_assert(module->malloc_func_index == (uint32)-1);
                    module->malloc_func_index = func_index;
                    LOG_VERBOSE("Found malloc function, name: %s, index: %u",
                                exports[i].name, exports[i].index);
                }
            }
            else if (!strcmp(exports[i].name, "__new")) {
                func_index = exports[i].index - module->import_func_count;
                func_type_index = module->func_type_indexes[func_index];
                func_type = (AOTFuncType *)module->types[func_type_index];
                if (func_type->param_count == 2 && func_type->result_count == 1
                    && func_type->types[0] == malloc_free_io_type
                    && func_type->types[1] == VALUE_TYPE_I32
                    && func_type->types[2] == malloc_free_io_type) {
                    uint32 j;
                    WASMExport *export_tmp;

                    bh_assert(module->malloc_func_index == (uint32)-1);
                    module->malloc_func_index = func_index;
                    LOG_VERBOSE("Found malloc function, name: %s, index: %u",
                                exports[i].name, exports[i].index);

                    /* resolve retain function.
                        If not find, reset malloc function index */
                    export_tmp = module->exports;
                    for (j = 0; j < module->export_count; j++, export_tmp++) {
                        if ((export_tmp->kind == EXPORT_KIND_FUNC)
                            && (!strcmp(export_tmp->name, "__retain")
                                || !strcmp(export_tmp->name, "__pin"))) {
                            func_index =
                                export_tmp->index - module->import_func_count;
                            func_type_index =
                                module->func_type_indexes[func_index];
                            func_type =
                                (AOTFuncType *)module->types[func_type_index];
                            if (func_type->param_count == 1
                                && func_type->result_count == 1
                                && func_type->types[0] == malloc_free_io_type
                                && func_type->types[1] == malloc_free_io_type) {
                                bh_assert(module->retain_func_index
                                          == (uint32)-1);
                                module->retain_func_index = export_tmp->index;
                                LOG_VERBOSE("Found retain function, name: %s, "
                                            "index: %u",
                                            export_tmp->name,
                                            export_tmp->index);
                                break;
                            }
                        }
                    }
                    if (j == module->export_count) {
                        module->malloc_func_index = (uint32)-1;
                        LOG_VERBOSE("Can't find retain function,"
                                    "reset malloc function index to -1");
                    }
                }
            }
            else if ((!strcmp(exports[i].name, "free"))
                     || (!strcmp(exports[i].name, "__release"))
                     || (!strcmp(exports[i].name, "__unpin"))) {
                func_index = exports[i].index - module->import_func_count;
                func_type_index = module->func_type_indexes[func_index];
                func_type = (AOTFuncType *)module->types[func_type_index];
                if (func_type->param_count == 1 && func_type->result_count == 0
                    && func_type->types[0] == malloc_free_io_type) {
                    bh_assert(module->free_func_index == (uint32)-1);
                    module->free_func_index = func_index;
                    LOG_VERBOSE("Found free function, name: %s, index: %u",
                                exports[i].name, exports[i].index);
                }
            }
        }
    }

    /* Flush data cache before executing AOT code,
     * otherwise unpredictable behavior can occur. */
    os_dcache_flush();

#if WASM_ENABLE_MEMORY_TRACING != 0
    wasm_runtime_dump_module_mem_consumption((WASMModuleCommon *)module);
#endif

#if WASM_ENABLE_DEBUG_AOT != 0
    if (!jit_code_entry_create(module->elf_hdr, module->elf_size)) {
        set_error_buf(error_buf, error_buf_size,
                      "create jit code entry failed");
        return false;
    }
#endif
    return true;
}

static AOTModule *
create_module(char *name, char *error_buf, uint32 error_buf_size)
{
    AOTModule *module =
        loader_malloc(sizeof(AOTModule), error_buf, error_buf_size);
    bh_list_status ret;

    if (!module) {
        return NULL;
    }

    module->module_type = Wasm_Module_AoT;

    module->name = name;
    module->is_binary_freeable = false;

#if WASM_ENABLE_MULTI_MODULE != 0
    module->import_module_list = &module->import_module_list_head;
    ret = bh_list_init(module->import_module_list);
    bh_assert(ret == BH_LIST_SUCCESS);
#endif
    (void)ret;

#if WASM_ENABLE_GC != 0
    if (!(module->ref_type_set =
              wasm_reftype_set_create(GC_REFTYPE_MAP_SIZE_DEFAULT))) {
        set_error_buf(error_buf, error_buf_size, "create reftype map failed");
        goto fail1;
    }

    if (os_mutex_init(&module->rtt_type_lock)) {
        set_error_buf(error_buf, error_buf_size, "init rtt type lock failed");
        goto fail2;
    }
#endif

#if WASM_ENABLE_LIBC_WASI != 0
#if WASM_ENABLE_UVWASI == 0
    module->wasi_args.stdio[0] = os_invalid_raw_handle();
    module->wasi_args.stdio[1] = os_invalid_raw_handle();
    module->wasi_args.stdio[2] = os_invalid_raw_handle();
#else
    module->wasi_args.stdio[0] = os_get_invalid_handle();
    module->wasi_args.stdio[1] = os_get_invalid_handle();
    module->wasi_args.stdio[2] = os_get_invalid_handle();
#endif /* WASM_ENABLE_UVWASI == 0 */
#endif /* WASM_ENABLE_LIBC_WASI != 0 */

    return module;
#if WASM_ENABLE_GC != 0
fail2:
    bh_hash_map_destroy(module->ref_type_set);
fail1:
#endif
    wasm_runtime_free(module);
    return NULL;
}

AOTModule *
aot_load_from_sections(AOTSection *section_list, char *error_buf,
                       uint32 error_buf_size)
{
    AOTModule *module = create_module("", error_buf, error_buf_size);

    if (!module)
        return NULL;

    if (!load_from_sections(module, section_list, false, false, error_buf,
                            error_buf_size)) {
        aot_unload(module);
        return NULL;
    }

    LOG_VERBOSE("Load module from sections success.\n");
    return module;
}

static void
destroy_sections(AOTSection *section_list, bool destroy_aot_text)
{
    AOTSection *section = section_list, *next;
    while (section) {
        next = section->next;
        if (destroy_aot_text && section->section_type == AOT_SECTION_TYPE_TEXT
            && section->section_body)
            os_munmap((uint8 *)section->section_body,
                      section->section_body_size);
        wasm_runtime_free(section);
        section = next;
    }
}

static bool
resolve_execute_mode(const uint8 *buf, uint32 size, bool *p_mode,
                     char *error_buf, uint32 error_buf_size)
{
    const uint8 *p = buf, *p_end = buf + size;
    uint32 section_type;
    uint32 section_size = 0;
    uint16 e_type = 0;

    p += 8;
    while (p < p_end) {
        read_uint32(p, p_end, section_type);
        if (section_type <= AOT_SECTION_TYPE_SIGNATURE) {
            read_uint32(p, p_end, section_size);
            CHECK_BUF(p, p_end, section_size);
            if (section_type == AOT_SECTION_TYPE_TARGET_INFO) {
                p += 4;
                read_uint16(p, p_end, e_type);
                if (e_type == E_TYPE_XIP) {
                    *p_mode = true;
                }
                else {
                    *p_mode = false;
                }
                break;
            }
        }
        else { /* section_type > AOT_SECTION_TYPE_SIGNATURE */
            set_error_buf(error_buf, error_buf_size,
                          "resolve execute mode failed");
            break;
        }
        p += section_size;
    }
    return true;
fail:
    return false;
}

static bool
create_sections(AOTModule *module, const uint8 *buf, uint32 size,
                AOTSection **p_section_list, char *error_buf,
                uint32 error_buf_size)
{
    AOTSection *section_list = NULL, *section_list_end = NULL, *section;
    const uint8 *p = buf, *p_end = buf + size;
    bool destroy_aot_text = false;
    bool is_indirect_mode = false;
    uint32 section_type;
    uint32 section_size;
    uint64 total_size;
    uint8 *aot_text;
#if (WASM_MEM_DUAL_BUS_MIRROR != 0)
    uint8 *mirrored_text;
#endif

    if (!resolve_execute_mode(buf, size, &is_indirect_mode, error_buf,
                              error_buf_size)) {
        goto fail;
    }

    module->is_indirect_mode = is_indirect_mode;

    p += 8;
    while (p < p_end) {
        read_uint32(p, p_end, section_type);
        if (section_type < AOT_SECTION_TYPE_SIGNATURE
            || section_type == AOT_SECTION_TYPE_CUSTOM) {
            read_uint32(p, p_end, section_size);
            CHECK_BUF(p, p_end, section_size);

            if (!(section = loader_malloc(sizeof(AOTSection), error_buf,
                                          error_buf_size))) {
                goto fail;
            }

            memset(section, 0, sizeof(AOTSection));
            section->section_type = (int32)section_type;
            section->section_body = (uint8 *)p;
            section->section_body_size = section_size;

            if (section_type == AOT_SECTION_TYPE_TEXT) {
                if ((section_size > 0) && !module->is_indirect_mode) {
                    total_size =
                        (uint64)section_size + aot_get_plt_table_size();
                    total_size = (total_size + 3) & ~((uint64)3);
                    if (total_size >= UINT32_MAX
                        || !(aot_text =
                                 loader_mmap((uint32)total_size, true,
                                             error_buf, error_buf_size))) {
                        wasm_runtime_free(section);
                        goto fail;
                    }

#if (WASM_MEM_DUAL_BUS_MIRROR != 0)
                    mirrored_text = os_get_dbus_mirror(aot_text);
                    bh_assert(mirrored_text != NULL);
                    bh_memcpy_s(mirrored_text, (uint32)total_size,
                                section->section_body, (uint32)section_size);
                    os_dcache_flush();
#else
                    bh_memcpy_s(aot_text, (uint32)total_size,
                                section->section_body, (uint32)section_size);
#endif
                    section->section_body = aot_text;
                    destroy_aot_text = true;

                    if ((uint32)total_size > section->section_body_size) {
                        memset(aot_text + (uint32)section_size, 0,
                               (uint32)total_size - section_size);
                        section->section_body_size = (uint32)total_size;
                    }
                }
            }

            if (!section_list)
                section_list = section_list_end = section;
            else {
                section_list_end->next = section;
                section_list_end = section;
            }

            p += section_size;
        }
        else {
            set_error_buf(error_buf, error_buf_size, "invalid section id");
            goto fail;
        }
    }

    if (!section_list) {
        set_error_buf(error_buf, error_buf_size, "create section list failed");
        return false;
    }

    *p_section_list = section_list;
    return true;
fail:
    if (section_list)
        destroy_sections(section_list, destroy_aot_text);
    return false;
}

static bool
aot_compatible_version(uint32 version)
{
    /*
     * refer to "AoT-compiled module compatibility among WAMR versions" in
     * ./doc/biuld_wasm_app.md
     */
    return version == AOT_CURRENT_VERSION;
}

static bool
load(const uint8 *buf, uint32 size, AOTModule *module,
     bool wasm_binary_freeable, bool no_resolve, char *error_buf,
     uint32 error_buf_size)
{
    const uint8 *buf_end = buf + size;
    const uint8 *p = buf, *p_end = buf_end;
    uint32 magic_number, version;
    AOTSection *section_list = NULL;
    bool ret;

    read_uint32(p, p_end, magic_number);
    if (magic_number != AOT_MAGIC_NUMBER) {
        set_error_buf(error_buf, error_buf_size, "magic header not detected");
        return false;
    }

    read_uint32(p, p_end, version);
    if (!aot_compatible_version(version)) {
        set_error_buf(error_buf, error_buf_size, "unknown binary version");
        return false;
    }

    module->package_version = version;

    if (!create_sections(module, buf, size, &section_list, error_buf,
                         error_buf_size))
        return false;

    ret = load_from_sections(module, section_list, !wasm_binary_freeable,
                             no_resolve, error_buf, error_buf_size);
    if (!ret) {
        /* If load_from_sections() fails, then aot text is destroyed
           in destroy_sections() */
        destroy_sections(section_list,
                         module->is_indirect_mode
                                 || module->merged_data_text_sections
                             ? false
                             : true);
        /* aot_unload() won't destroy aot text again */
        module->code = NULL;
    }
    else {
        /* If load_from_sections() succeeds, then aot text is set to
           module->code and will be destroyed in aot_unload() */
        destroy_sections(section_list, false);
    }

#if 0
    {
        uint32 i;
        for (i = 0; i < module->func_count; i++) {
            LOG_VERBOSE("AOT func %u, addr: %p\n", i, module->func_ptrs[i]);
        }
    }
#endif

#if WASM_ENABLE_LINUX_PERF != 0
    if (wasm_runtime_get_linux_perf())
        if (!aot_create_perf_map(module, error_buf, error_buf_size))
            goto fail;
#endif

    return ret;
fail:
    return false;
}

AOTModule *
aot_load_from_aot_file(const uint8 *buf, uint32 size, const LoadArgs *args,
                       char *error_buf, uint32 error_buf_size)
{
    AOTModule *module = create_module(args->name, error_buf, error_buf_size);

    if (!module)
        return NULL;

    os_thread_jit_write_protect_np(false); /* Make memory writable */
    if (!load(buf, size, module, args->wasm_binary_freeable, args->no_resolve,
              error_buf, error_buf_size)) {
        aot_unload(module);
        return NULL;
    }
    os_thread_jit_write_protect_np(true); /* Make memory executable */
    os_icache_flush(module->code, module->code_size);

#if WASM_ENABLE_AOT_VALIDATOR != 0
    if (!aot_module_validate(module, error_buf, error_buf_size)) {
        aot_unload(module);
        return NULL;
    }
#endif /* WASM_ENABLE_AOT_VALIDATOR != 0 */

    LOG_VERBOSE("Load module success.\n");
    return module;
}

void
aot_unload(AOTModule *module)
{
    if (module->import_memories)
        destroy_import_memories(module->import_memories);

    if (module->memories)
        wasm_runtime_free(module->memories);

    if (module->mem_init_data_list)
        destroy_mem_init_data_list(module, module->mem_init_data_list,
                                   module->mem_init_data_count);

    if (module->native_symbol_list)
        wasm_runtime_free(module->native_symbol_list);

    if (module->import_tables)
        destroy_import_tables(module->import_tables);

    if (module->tables) {
#if WASM_ENABLE_GC != 0
        uint32 i;
        for (i = 0; i < module->table_count; i++) {
            destroy_init_expr(&module->tables[i].init_expr);
        }
#endif
        destroy_tables(module->tables);
    }

    if (module->table_init_data_list)
        destroy_table_init_data_list(module->table_init_data_list,
                                     module->table_init_data_count);

    if (module->types)
        destroy_types(module->types, module->type_count);

    if (module->import_globals)
        destroy_import_globals(module->import_globals);

    if (module->globals) {
#if WASM_ENABLE_GC != 0 || WASM_ENABLE_EXTENDED_CONST_EXPR != 0
        uint32 i;
        for (i = 0; i < module->global_count; i++) {
            destroy_init_expr(&module->globals[i].init_expr);
        }
#endif
        destroy_globals(module->globals);
    }

    if (module->import_funcs)
        destroy_import_funcs(module->import_funcs);

    if (module->exports)
        destroy_exports(module->exports);

    if (module->func_type_indexes)
        wasm_runtime_free(module->func_type_indexes);

#if WASM_ENABLE_AOT_STACK_FRAME != 0
    if (module->max_local_cell_nums)
        wasm_runtime_free(module->max_local_cell_nums);
    if (module->max_stack_cell_nums)
        wasm_runtime_free(module->max_stack_cell_nums);
#endif

#if WASM_ENABLE_GC != 0
    if (module->func_local_ref_flags) {
        uint32 i;
        for (i = 0; i < module->import_func_count + module->func_count; i++) {
            if (module->func_local_ref_flags[i].local_ref_flags) {
                wasm_runtime_free(
                    module->func_local_ref_flags[i].local_ref_flags);
            }
        }

        wasm_runtime_free(module->func_local_ref_flags);
    }
#endif

    if (module->func_ptrs)
        wasm_runtime_free(module->func_ptrs);

    if (module->const_str_set)
        bh_hash_map_destroy(module->const_str_set);
#if WASM_ENABLE_MULTI_MODULE != 0
    /* just release the sub module list */
    if (module->import_module_list) {
        WASMRegisteredModule *node =
            bh_list_first_elem(module->import_module_list);
        while (node) {
            WASMRegisteredModule *next = bh_list_elem_next(node);
            bh_list_remove(module->import_module_list, node);
            wasm_runtime_free(node);
            node = next;
        }
    }
#endif

    if (module->code && !module->is_indirect_mode
        && !module->merged_data_text_sections) {
        /* The layout is: literal size + literal + code (with plt table) */
        uint8 *mmap_addr = module->literal - sizeof(uint32);
        uint32 total_size =
            sizeof(uint32) + module->literal_size + module->code_size;
        os_munmap(mmap_addr, total_size);
    }

#if defined(BH_PLATFORM_WINDOWS)
    if (module->extra_plt_data) {
        os_munmap(module->extra_plt_data, module->extra_plt_data_size);
    }
#endif

#if (defined(BUILD_TARGET_X86_64) || defined(BUILD_TARGET_AMD_64)) \
    && !defined(BH_PLATFORM_WINDOWS)
    {
        GOTItem *got_item = module->got_item_list, *got_item_next;

        if (module->got_func_ptrs) {
            os_munmap(module->got_func_ptrs,
                      sizeof(void *) * module->got_item_count);
        }
        while (got_item) {
            got_item_next = got_item->next;
            wasm_runtime_free(got_item);
            got_item = got_item_next;
        }
    }
#endif

    if (module->data_sections)
        destroy_object_data_sections(module->data_sections,
                                     module->data_section_count);

    if (module->merged_data_sections)
        os_munmap(module->merged_data_sections,
                  module->merged_data_sections_size);

    if (module->merged_data_text_sections)
        os_munmap(module->merged_data_text_sections,
                  module->merged_data_text_sections_size);

#if WASM_ENABLE_DEBUG_AOT != 0
    jit_code_entry_destroy(module->elf_hdr);
#endif

#if WASM_ENABLE_CUSTOM_NAME_SECTION != 0
    if (module->aux_func_indexes) {
        wasm_runtime_free(module->aux_func_indexes);
    }
    if (module->aux_func_names) {
        wasm_runtime_free((void *)module->aux_func_names);
    }
#endif

#if WASM_ENABLE_LOAD_CUSTOM_SECTION != 0
    wasm_runtime_destroy_custom_sections(module->custom_section_list);
#endif

#if WASM_ENABLE_GC != 0
    if (module->ref_type_set) {
        bh_hash_map_destroy(module->ref_type_set);
    }
    os_mutex_destroy(&module->rtt_type_lock);
    if (module->rtt_types) {
        uint32 i;
        for (i = 0; i < module->type_count; i++) {
            if (module->rtt_types[i])
                wasm_runtime_free(module->rtt_types[i]);
        }
        wasm_runtime_free(module->rtt_types);
    }
#if WASM_ENABLE_STRINGREF != 0
    {
        uint32 i;
        for (i = 0; i < WASM_TYPE_STRINGVIEWITER - WASM_TYPE_STRINGREF + 1;
             i++) {
            if (module->stringref_rtts[i])
                wasm_runtime_free(module->stringref_rtts[i]);
        }

        if (module->string_literal_lengths) {
            wasm_runtime_free(module->string_literal_lengths);
        }

        if (module->string_literal_ptrs) {
            wasm_runtime_free((void *)module->string_literal_ptrs);
        }
    }
#endif
#endif

    wasm_runtime_free(module);
}

uint32
aot_get_plt_table_size(void)
{
    return get_plt_table_size();
}

#if WASM_ENABLE_LOAD_CUSTOM_SECTION != 0
const uint8 *
aot_get_custom_section(const AOTModule *module, const char *name, uint32 *len)
{
    WASMCustomSection *section = module->custom_section_list;

    while (section) {
        if (strcmp(section->name_addr, name) == 0) {
            if (len) {
                *len = section->content_len;
            }
            return section->content_addr;
        }

        section = section->next;
    }

    return NULL;
}
#endif /* end of WASM_ENABLE_LOAD_CUSTOM_SECTION */

#if WASM_ENABLE_STATIC_PGO != 0
void
aot_exchange_uint16(uint8 *p_data)
{
    return exchange_uint16(p_data);
}

void
aot_exchange_uint32(uint8 *p_data)
{
    return exchange_uint32(p_data);
}

void
aot_exchange_uint64(uint8 *p_data)
{
    return exchange_uint64(p_data);
}
#endif
