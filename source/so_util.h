/*
 * so_util.h – Android ARMv7 .so betöltő 32 bites Horizonra (Nintendo Switch)
 *
 * A TheFloW-féle so_util (Vita) felépítését követi, de a Switch memóriamodelljéhez igazítva:
 * a modul egy heap-pufferben készül el (relokálás, importok, hookok), és csak a végén
 * képződik le a kódrégióba (svcMapProcessCodeMemory), szegmensenkénti jogosultsággal.
 * Leképezés után a kód (RX) már nem írható – minden szövegfolt so_finalize() előtt kell.
 *
 * Copyright (C) 2021 Andy Nguyen, (C) 2021-2023 Rinnegatamante, (C) 2022-2023 Volodymyr Atamanenko
 * Copyright (C) 2026 Levi (Switch port)
 * MIT license, lásd LICENSE.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "elf.h"

typedef struct {
    const char *symbol;
    uintptr_t func;
} so_default_dynlib;

typedef struct {
    uintptr_t addr;        // a hookolt függvény címe (a leképezett modulban)
    uintptr_t trampoline;  // ezen át hívható az eredeti függvény (0, ha nincs)
} so_hook;

typedef struct so_module {
    uintptr_t base;        // végleges virtuális cím
    uint8_t *rw;           // előkészítő puffer, csak so_finalize() előtt érvényes
    size_t size;           // teljes leképezett méret (kép + trampolin-terület)
    size_t image_size;
    uintptr_t cave_base, cave_head, cave_end;
    int finalized;

    Elf32_Phdr *phdr;
    int num_phdr;
    // a dinamikus táblák eltolásai a modul elejétől
    uint32_t dynsym_off, dynstr_off, rel_off, jmprel_off, init_array_off;
    uint32_t num_dynsym, num_rel, num_jmprel, num_init_array;
} so_module;

// Fájl betöltése az előkészítő pufferbe (még nem futtatható).
int so_file_load(so_module *mod, const char *path);
// R_ARM_RELATIVE / ABS32 / GLOB_DAT / JUMP_SLOT relokációk a modul saját szimbólumaira.
void so_relocate(so_module *mod);
// Importok feloldása a táblából; a hiányzókat naplózza és egy hibajelző csonkra köti.
void so_resolve(so_module *mod, const so_default_dynlib *dynlib, size_t count);
// ARM-függvény átirányítása dst-re. A visszaadott hook.trampoline-on át az eredeti hívható.
so_hook hook_addr(so_module *mod, uintptr_t addr, uintptr_t dst);
// Leképezés a kódrégióba, jogosultságok, cache-ürítés. Utána a kód már nem írható.
void so_finalize(so_module *mod);
// DT_INIT_ARRAY futtatása (so_finalize után).
void so_initialize(so_module *mod);
// Exportált szimbólum címe (0, ha nincs). A cím a végleges (leképezett) virtuális cím.
uintptr_t so_symbol(so_module *mod, const char *symbol);
// Írható/olvasható mutató a modul egy virtuális címére: so_finalize() előtt az előkészítő
// pufferbe mutat, utána magára a címre (az RX-szegmens akkor már nem írható!).
void *so_va(so_module *mod, uintptr_t va);

// Az eredeti függvény hívása hookból, pontos függvénymutató-típussal (a „type (*)()” C23-ban
// paraméter nélküli prototípus, és a float-argumentumokat is elrontaná):
//   SO_CONTINUE(int (*)(void *, int), hook, this, x)
#define SO_CONTINUE(fn_ptr_type, h, ...) (((fn_ptr_type)(h).trampoline)(__VA_ARGS__))
