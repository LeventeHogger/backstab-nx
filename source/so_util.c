/*
 * so_util.c – Android ARMv7 .so betöltő 32 bites Horizonra (Nintendo Switch)
 *
 * Copyright (C) 2021 Andy Nguyen, (C) 2021-2023 Rinnegatamante, (C) 2022-2023 Volodymyr Atamanenko
 * Copyright (C) 2026 Levi (Switch port)
 * MIT license, lásd LICENSE.
 */
#include "so_util.h"

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "util.h"

#define PAGE_ALIGN(x) (((x) + 0xFFF) & ~0xFFF)
#define CAVE_SIZE 0x4000

// Írás/olvasás a modulba: leképezés előtt a pufferbe, utána a végleges címre.
static inline void *so_ptr(so_module *mod, uintptr_t off) {
    return mod->finalized ? (void *)(mod->base + off) : (void *)(mod->rw + off);
}
static inline void *so_va_to_ptr(so_module *mod, uintptr_t va) {
    return so_ptr(mod, va - mod->base);
}
static inline Elf32_Sym *so_dynsym(so_module *mod) { return so_ptr(mod, mod->dynsym_off); }
static inline const char *so_dynstr(so_module *mod) { return so_ptr(mod, mod->dynstr_off); }

// ---------------------------------------------------------------------------
// Saját folyamat-handle: az svcMapProcessCodeMemory-hez valódi handle kell.
// Az nx-hbloader trükkje: egy saját session-ön átküldjük a CUR_PROCESS_HANDLE pszeudo-handle-t
// másolt handle-ként, a kernel ezt valódi handle-lé alakítja.
static Handle g_own_process = INVALID_HANDLE;

static void proc_handle_thread(void *arg) {
    Handle session = (Handle)(uintptr_t)arg;
    void *base = armGetTls();
    hipcMakeRequestInline(base);
    s32 idx = 0;
    Result rc = svcReplyAndReceive(&idx, &session, 1, INVALID_HANDLE, UINT64_MAX);
    if (R_SUCCEEDED(rc)) {
        HipcParsedRequest r = hipcParseRequest(base);
        if (r.meta.num_copy_handles == 1)
            g_own_process = r.data.copy_handles[0];
    }
    svcCloseHandle(session);
}

static Handle get_own_process_handle(void) {
    if (g_own_process != INVALID_HANDLE)
        return g_own_process;

    Handle server, client;
    Result rc = svcCreateSession(&server, &client, 0, 0);
    if (R_FAILED(rc))
        fatal_error("svcCreateSession: 0x%x", rc);

    Thread t;
    rc = threadCreate(&t, proc_handle_thread, (void *)(uintptr_t)server, NULL, 0x1000, 0x20, -2);
    if (R_FAILED(rc))
        fatal_error("threadCreate (proc handle): 0x%x", rc);
    threadStart(&t);

    hipcMakeRequestInline(armGetTls(), .num_copy_handles = 1).copy_handles[0] = CUR_PROCESS_HANDLE;
    svcSendSyncRequest(client);
    svcCloseHandle(client);
    threadWaitForExit(&t);
    threadClose(&t);

    if (g_own_process == INVALID_HANDLE)
        fatal_error("Nem sikerült megszerezni a saját folyamat handle-jét.");
    return g_own_process;
}

// ---------------------------------------------------------------------------

int so_file_load(so_module *mod, const char *path) {
    memset(mod, 0, sizeof(*mod));

    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    fseek(f, 0, SEEK_END);
    size_t file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *file = malloc(file_size);
    if (!file || fread(file, 1, file_size, f) != file_size) {
        fclose(f);
        free(file);
        return -2;
    }
    fclose(f);

    Elf32_Ehdr *ehdr = (Elf32_Ehdr *)file;
    if (memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0 || ehdr->e_machine != EM_ARM) {
        free(file);
        return -3;
    }
    Elf32_Phdr *phdr = (Elf32_Phdr *)(file + ehdr->e_phoff);

    // a kép mérete: a legmagasabb PT_LOAD vége
    size_t image_size = 0;
    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type == PT_LOAD && phdr[i].p_vaddr + phdr[i].p_memsz > image_size)
            image_size = phdr[i].p_vaddr + phdr[i].p_memsz;
    }
    mod->image_size = PAGE_ALIGN(image_size);
    mod->size = mod->image_size + CAVE_SIZE;

    mod->rw = memalign(0x1000, mod->size);
    if (!mod->rw) {
        free(file);
        return -4;
    }
    memset(mod->rw, 0, mod->size);

    // címtartomány foglalása a kódrégióban
    virtmemLock();
    void *va = virtmemFindCodeMemory(mod->size, 0x1000);
    VirtmemReservation *rv = va ? virtmemAddReservation(va, mod->size) : NULL;
    virtmemUnlock();
    if (!rv) {
        free(file);
        return -5;
    }
    mod->base = (uintptr_t)va;
    mod->cave_base = mod->cave_head = mod->base + mod->image_size;
    mod->cave_end = mod->base + mod->size;

    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type == PT_LOAD)
            memcpy(mod->rw + phdr[i].p_vaddr, file + phdr[i].p_offset, phdr[i].p_filesz);
    }

    // a programfejléceket megtartjuk (a jogosultságokhoz kellenek)
    mod->num_phdr = ehdr->e_phnum;
    mod->phdr = malloc(sizeof(Elf32_Phdr) * mod->num_phdr);
    memcpy(mod->phdr, phdr, sizeof(Elf32_Phdr) * mod->num_phdr);

    // dinamikus szegmens
    Elf32_Dyn *dyn = NULL;
    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type == PT_DYNAMIC)
            dyn = (Elf32_Dyn *)(mod->rw + phdr[i].p_vaddr);
    }
    free(file);
    if (!dyn)
        return -6;

    uint32_t relsz = 0, pltrelsz = 0, init_arraysz = 0, hash_off = 0;
    for (; dyn->d_tag != DT_NULL; dyn++) {
        switch (dyn->d_tag) {
            case DT_SYMTAB:        mod->dynsym_off = dyn->d_un.d_ptr; break;
            case DT_STRTAB:        mod->dynstr_off = dyn->d_un.d_ptr; break;
            case DT_HASH:          hash_off = dyn->d_un.d_ptr; break;
            case DT_REL:           mod->rel_off = dyn->d_un.d_ptr; break;
            case DT_RELSZ:         relsz = dyn->d_un.d_val; break;
            case DT_JMPREL:        mod->jmprel_off = dyn->d_un.d_ptr; break;
            case DT_PLTRELSZ:      pltrelsz = dyn->d_un.d_val; break;
            case DT_INIT_ARRAY:    mod->init_array_off = dyn->d_un.d_ptr; break;
            case DT_INIT_ARRAYSZ:  init_arraysz = dyn->d_un.d_val; break;
            default: break;
        }
    }
    mod->num_rel = relsz / sizeof(Elf32_Rel);
    mod->num_jmprel = pltrelsz / sizeof(Elf32_Rel);
    mod->num_init_array = init_arraysz / sizeof(uintptr_t);
    // a DT_HASH nchain mezője = a dinamikus szimbólumok száma
    mod->num_dynsym = hash_off ? ((uint32_t *)(mod->rw + hash_off))[1] : 0;

    log_info("so_file_load: %s @ 0x%08x, %u KB, %u szimbólum", path, (unsigned)mod->base,
             (unsigned)(mod->size / 1024), (unsigned)mod->num_dynsym);
    return 0;
}

static void relocate_table(so_module *mod, uint32_t off, uint32_t count) {
    Elf32_Rel *rel = so_ptr(mod, off);
    Elf32_Sym *syms = so_dynsym(mod);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t *ptr = so_ptr(mod, rel[i].r_offset);
        Elf32_Sym *sym = &syms[ELF32_R_SYM(rel[i].r_info)];
        switch (ELF32_R_TYPE(rel[i].r_info)) {
            case R_ARM_ABS32:
                if (sym->st_shndx != SHN_UNDEF)
                    *ptr += mod->base + sym->st_value;
                break;
            case R_ARM_RELATIVE:
                *ptr += mod->base;
                break;
            case R_ARM_GLOB_DAT:
            case R_ARM_JUMP_SLOT:
                if (sym->st_shndx != SHN_UNDEF)
                    *ptr = mod->base + sym->st_value;
                break;
            default:
                fatal_error("Ismeretlen relokáció: %d", (int)ELF32_R_TYPE(rel[i].r_info));
        }
    }
}

void so_relocate(so_module *mod) {
    relocate_table(mod, mod->rel_off, mod->num_rel);
    relocate_table(mod, mod->jmprel_off, mod->num_jmprel);
}

// Hiányzó import hívásakor ide fut a vezérlés.
static void so_unresolved_import(void) {
    uintptr_t lr = (uintptr_t)__builtin_return_address(0);
    fatal_error("Feloldatlan import hívása (LR=0x%08x).", (unsigned)lr);
}

static int resolve_table(so_module *mod, uint32_t off, uint32_t count,
                         const so_default_dynlib *dynlib, size_t n) {
    Elf32_Rel *rel = so_ptr(mod, off);
    Elf32_Sym *syms = so_dynsym(mod);
    const char *strtab = so_dynstr(mod);
    int missing = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t type = ELF32_R_TYPE(rel[i].r_info);
        if (type != R_ARM_ABS32 && type != R_ARM_GLOB_DAT && type != R_ARM_JUMP_SLOT)
            continue;
        Elf32_Sym *sym = &syms[ELF32_R_SYM(rel[i].r_info)];
        if (sym->st_shndx != SHN_UNDEF)
            continue;
        const char *name = strtab + sym->st_name;
        uint32_t *ptr = so_ptr(mod, rel[i].r_offset);
        size_t k;
        for (k = 0; k < n; k++) {
            if (strcmp(name, dynlib[k].symbol) == 0) {
                *ptr = dynlib[k].func;
                break;
            }
        }
        if (k == n) {
            if (ELF32_ST_BIND(sym->st_info) == STB_WEAK) {
                *ptr = 0;
            } else {
                *ptr = (uintptr_t)&so_unresolved_import;
                log_error("feloldatlan import: %s", name);
                missing++;
            }
        }
    }
    return missing;
}

void so_resolve(so_module *mod, const so_default_dynlib *dynlib, size_t count) {
    int missing = resolve_table(mod, mod->rel_off, mod->num_rel, dynlib, count);
    missing += resolve_table(mod, mod->jmprel_off, mod->num_jmprel, dynlib, count);
    log_info("so_resolve: %d feloldatlan import", missing);
}

// ---------------------------------------------------------------------------
// Hookok. A felülírt első két ARM-utasítás egy trampolinba kerül (a kódüreg-területen),
// a PC-relatív literálbetöltéseket abszolút literállá alakítva.

static uint32_t *cave_alloc(so_module *mod, size_t words) {
    uintptr_t va = mod->cave_head;
    if (va + words * 4 > mod->cave_end)
        fatal_error("Elfogyott a trampolin-terület.");
    mod->cave_head += words * 4;
    return so_va_to_ptr(mod, va);
}

static uintptr_t build_arm_trampoline(so_module *mod, uintptr_t addr) {
    uint32_t *orig = so_va_to_ptr(mod, addr);
    uint32_t code[16];
    int n = 0;

    for (int i = 0; i < 2; i++) {
        uint32_t insn = orig[i];
        uintptr_t pc = addr + i * 4 + 8;
        if ((insn & 0x0F7F0000) == 0x051F0000) {
            // LDR Rt, [PC, #±imm12] → LDR Rt, [PC]; B +0; .word <a literál értéke>
            uint32_t imm = insn & 0xFFF;
            uintptr_t lit = (insn & (1 << 23)) ? pc + imm : pc - imm;
            uint32_t value = *(uint32_t *)so_va_to_ptr(mod, lit);
            code[n++] = 0xE59F0000 | (insn & 0xF000);
            code[n++] = 0xEA000000;
            code[n++] = value;
        } else if ((insn & 0x0E000000) == 0x0A000000 ||    // B/BL/BLX imm
                   ((insn >> 16) & 0xF) == 0xF ||           // Rn == PC
                   ((insn >> 12) & 0xF) == 0xF ||           // Rd == PC
                   (insn & 0xF0000000) != 0xE0000000) {     // feltételes utasítás
            fatal_error("Hook 0x%08x: nem kezelt utasítás a prológban: 0x%08x",
                        (unsigned)(addr - mod->base), (unsigned)insn);
        } else {
            code[n++] = insn;
        }
    }
    code[n++] = 0xE51FF004;          // LDR PC, [PC, #-4]
    code[n++] = addr + 8;

    uintptr_t tramp_va = mod->cave_head;
    uint32_t *tramp = cave_alloc(mod, n);
    memcpy(tramp, code, n * 4);
    return tramp_va;
}

so_hook hook_addr(so_module *mod, uintptr_t addr, uintptr_t dst) {
    so_hook h = { addr, 0 };
    if (!addr)
        fatal_error("hook_addr: NULL cím");
    if (mod->finalized)
        fatal_error("hook_addr a leképezés után: 0x%08x", (unsigned)addr);
    if (addr & 1)
        fatal_error("Thumb-hook nincs támogatva: 0x%08x", (unsigned)(addr - mod->base));

    h.trampoline = build_arm_trampoline(mod, addr);
    uint32_t *p = so_va_to_ptr(mod, addr);
    p[0] = 0xE51FF004;               // LDR PC, [PC, #-4]
    p[1] = dst;
    return h;
}

// ---------------------------------------------------------------------------

void so_finalize(so_module *mod) {
    Handle proc = get_own_process_handle();

    armDCacheFlush(mod->rw, mod->size);
    Result rc = svcMapProcessCodeMemory(proc, (u64)mod->base, (u64)(uintptr_t)mod->rw, (u64)mod->size);
    if (R_FAILED(rc))
        fatal_error("svcMapProcessCodeMemory: 0x%x", rc);
    mod->finalized = 1;

    for (int i = 0; i < mod->num_phdr; i++) {
        Elf32_Phdr *ph = &mod->phdr[i];
        if (ph->p_type != PT_LOAD)
            continue;
        uintptr_t start = (mod->base + ph->p_vaddr) & ~0xFFF;
        uintptr_t end = PAGE_ALIGN(mod->base + ph->p_vaddr + ph->p_memsz);
        u32 perm = (ph->p_flags & PF_X) ? Perm_Rx : (ph->p_flags & PF_W) ? Perm_Rw : Perm_R;
        rc = svcSetProcessMemoryPermission(proc, (u64)start, (u64)(end - start), perm);
        if (R_FAILED(rc))
            fatal_error("svcSetProcessMemoryPermission(0x%08x, %x): 0x%x", (unsigned)start, perm, rc);
    }
    rc = svcSetProcessMemoryPermission(proc, (u64)mod->cave_base, (u64)(mod->cave_end - mod->cave_base), Perm_Rx);
    if (R_FAILED(rc))
        fatal_error("svcSetProcessMemoryPermission(cave): 0x%x", rc);

    armICacheInvalidate((void *)mod->base, mod->size);
    mod->rw = NULL;
    log_info("so_finalize: modul leképezve, 0x%08x-0x%08x", (unsigned)mod->base, (unsigned)(mod->base + mod->size));
}

void so_initialize(so_module *mod) {
    uintptr_t *init_array = so_ptr(mod, mod->init_array_off);
    for (uint32_t i = 0; i < mod->num_init_array; i++) {
        if (init_array[i] != 0 && init_array[i] != (uintptr_t)-1)
            ((void (*)(void))init_array[i])();
    }
}

void *so_va(so_module *mod, uintptr_t va) {
    return so_va_to_ptr(mod, va);
}

uintptr_t so_symbol(so_module *mod, const char *symbol) {
    Elf32_Sym *syms = so_dynsym(mod);
    const char *strtab = so_dynstr(mod);
    for (uint32_t i = 0; i < mod->num_dynsym; i++) {
        if (syms[i].st_shndx != SHN_UNDEF && strcmp(strtab + syms[i].st_name, symbol) == 0)
            return mod->base + syms[i].st_value;
    }
    return 0;
}
