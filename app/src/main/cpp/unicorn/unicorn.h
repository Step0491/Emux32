#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void* uc_engine;
typedef size_t uc_hook;

typedef enum uc_err {
    UC_ERR_OK = 0,
    UC_ERR_NOMEM,
    UC_ERR_ARCH,
    UC_ERR_HANDLE,
    UC_ERR_MODE,
    UC_ERR_VERSION,
    UC_ERR_READ_UNMAPPED,
    UC_ERR_WRITE_UNMAPPED,
    UC_ERR_FETCH_UNMAPPED,
    UC_ERR_HOOK,
    UC_ERR_INSN_INVALID,
    UC_ERR_MAP,
    UC_ERR_WRITE_PROT,
    UC_ERR_READ_PROT,
    UC_ERR_FETCH_PROT,
    UC_ERR_ARG,
    UC_ERR_READ_UNALIGNED,
    UC_ERR_WRITE_UNALIGNED,
    UC_ERR_FETCH_UNALIGNED,
    UC_ERR_HOOK_EXIST,
    UC_ERR_RESOURCE,
    UC_ERR_EXCEPTION
} uc_err;

typedef enum uc_arch {
    UC_ARCH_ARM = 1,
    UC_ARCH_ARM64,
    UC_ARCH_MIPS,
    UC_ARCH_X86,
    UC_ARCH_PPC,
    UC_ARCH_SPARC,
    UC_ARCH_M68K,
    UC_ARCH_RISCV,
    UC_ARCH_MAX
} uc_arch;

typedef enum uc_mode {
    UC_MODE_LITTLE_ENDIAN = 0,
    UC_MODE_BIG_ENDIAN = 1 << 30,

    // ARM modes
    UC_MODE_ARM = 0,
    UC_MODE_THUMB = 1 << 4,
    UC_MODE_MCLASS = 1 << 5,
    UC_MODE_V8 = 1 << 6,
    UC_MODE_ARM926 = 1 << 7,
    UC_MODE_ARM946 = 1 << 8,
    UC_MODE_ARM1176 = 1 << 9,
} uc_mode;

typedef enum uc_mem_type {
    UC_MEM_READ = 16,
    UC_MEM_WRITE,
    UC_MEM_FETCH,
    UC_MEM_READ_UNMAPPED,
    UC_MEM_WRITE_UNMAPPED,
    UC_MEM_FETCH_UNMAPPED,
    UC_MEM_WRITE_PROT,
    UC_MEM_READ_PROT,
    UC_MEM_FETCH_PROT,
    UC_MEM_READ_AFTER
} uc_mem_type;

typedef enum uc_hook_type {
    UC_HOOK_INTR = 1 << 0,
    UC_HOOK_INSN = 1 << 1,
    UC_HOOK_CODE = 1 << 2,
    UC_HOOK_BLOCK = 1 << 3,
    UC_HOOK_MEM_READ_UNMAPPED = 1 << 4,
    UC_HOOK_MEM_WRITE_UNMAPPED = 1 << 5,
    UC_HOOK_MEM_FETCH_UNMAPPED = 1 << 6,
    UC_HOOK_MEM_READ_PROT = 1 << 7,
    UC_HOOK_MEM_WRITE_PROT = 1 << 8,
    UC_HOOK_MEM_FETCH_PROT = 1 << 9,
    UC_HOOK_MEM_READ = 1 << 10,
    UC_HOOK_MEM_WRITE = 1 << 11,
    UC_HOOK_MEM_FETCH = 1 << 12,
    UC_HOOK_MEM_UNMAPPED = UC_HOOK_MEM_READ_UNMAPPED | UC_HOOK_MEM_WRITE_UNMAPPED | UC_HOOK_MEM_FETCH_UNMAPPED,
    UC_HOOK_MEM_PROT = UC_HOOK_MEM_READ_PROT | UC_HOOK_MEM_WRITE_PROT | UC_HOOK_MEM_FETCH_PROT,
    UC_HOOK_MEM_READ_INVALID = UC_HOOK_MEM_READ_PROT | UC_HOOK_MEM_READ_UNMAPPED,
    UC_HOOK_MEM_WRITE_INVALID = UC_HOOK_MEM_WRITE_PROT | UC_HOOK_MEM_WRITE_UNMAPPED,
    UC_HOOK_MEM_FETCH_INVALID = UC_HOOK_MEM_FETCH_PROT | UC_HOOK_MEM_FETCH_UNMAPPED,
    UC_HOOK_MEM_INVALID = UC_HOOK_MEM_UNMAPPED | UC_HOOK_MEM_PROT,
    UC_HOOK_MEM_VALID = UC_HOOK_MEM_READ | UC_HOOK_MEM_WRITE | UC_HOOK_MEM_FETCH
} uc_hook_type;

typedef enum uc_prot {
    UC_PROT_NONE = 0,
    UC_PROT_READ = 1,
    UC_PROT_WRITE = 2,
    UC_PROT_EXEC = 4,
    UC_PROT_ALL = 7
} uc_prot;

typedef void (*uc_cb_hookintr_t)(uc_engine *uc, uint32_t intno, void *user_data);
typedef void (*uc_cb_hookcode_t)(uc_engine *uc, uint64_t address, uint32_t size, void *user_data);
typedef bool (*uc_cb_eventmem_t)(uc_engine *uc, uc_mem_type type, uint64_t address, int size, int64_t value, void *user_data);

uc_err uc_open(uc_arch arch, uc_mode mode, uc_engine **uc);
uc_err uc_close(uc_engine *uc);
const char *uc_strerror(uc_err code);

uc_err uc_reg_read(uc_engine *uc, int regid, void *value);
uc_err uc_reg_write(uc_engine *uc, int regid, const void *value);

uc_err uc_mem_map(uc_engine *uc, uint64_t address, size_t size, uint32_t perms);
uc_err uc_mem_unmap(uc_engine *uc, uint64_t address, size_t size);
uc_err uc_mem_write(uc_engine *uc, uint64_t address, const void *bytes, size_t size);
uc_err uc_mem_read(uc_engine *uc, uint64_t address, void *bytes, size_t size);

uc_err uc_hook_add(uc_engine *uc, uc_hook *hh, int type, void *callback, void *user_data, uint64_t begin, uint64_t end, ...);
uc_err uc_hook_del(uc_engine *uc, uc_hook hh);

uc_err uc_emu_start(uc_engine *uc, uint64_t begin, uint64_t until, uint64_t timeout, size_t count);
uc_err uc_emu_stop(uc_engine *uc);

#ifdef __cplusplus
}
#endif
