#define _GNU_SOURCE
#include "so_util.h"
#include <dlfcn.h>
#include <elf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define EM_X86_64 62
#define R_X86_64_64 1
#define R_X86_64_GLOB_DAT 6
#define R_X86_64_JUMP_SLOT 7
#define R_X86_64_RELATIVE 8

#ifndef R_X86_64_DTPMOD64
#define R_X86_64_DTPMOD64 16
#endif
#ifndef R_X86_64_DTPOFF64
#define R_X86_64_DTPOFF64 17
#endif
#ifndef R_X86_64_TPOFF64
#define R_X86_64_TPOFF64 18
#endif
#ifndef R_X86_64_TPOFF32
#define R_X86_64_TPOFF32 23
#endif

void *text_base, *data_base;
size_t text_size, data_size;
void *tls_template = NULL;
size_t tls_init_size = 0;
size_t tls_total_size = 0;
size_t tls_align = 1;

LoadedModule g_modules[MAX_MODULES];
int g_num_modules = 0;

static void *load_base;
static size_t load_size;
static void *so_base;
static Elf64_Ehdr *elf_hdr;
static Elf64_Phdr *prog_hdr;
static Elf64_Shdr *sec_hdr;
static Elf64_Sym *syms;
static int num_syms;
static char *shstrtab, *dynstrtab;

static void *g_libc_handle = NULL;
static void *g_libpthread_handle = NULL;

void so_flush_caches(void) {
  __builtin___clear_cache((char *)load_base, (char *)load_base + load_size);
}

void hook_x64(uintptr_t addr, uintptr_t dst) {
  uint8_t *hook = (uint8_t *)addr;
  size_t page_size = sysconf(_SC_PAGESIZE);
  uintptr_t page_start = addr & ~(page_size - 1);

  int64_t diff = (int64_t)dst - (int64_t)(addr + 5);
  if (diff >= -2147483648LL && diff <= 2147483647LL) {
    size_t len = ((addr + 5) - page_start + page_size - 1) & ~(page_size - 1);
    mprotect((void *)page_start, len, PROT_READ | PROT_WRITE | PROT_EXEC);
    hook[0] = 0xE9;
    *(int32_t *)(hook + 1) = (int32_t)diff;
    mprotect((void *)page_start, len, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)hook, (char *)hook + 5);
  } else {
    size_t len = ((addr + 14) - page_start + page_size - 1) & ~(page_size - 1);
    mprotect((void *)page_start, len, PROT_READ | PROT_WRITE | PROT_EXEC);
    hook[0] = 0xFF;
    hook[1] = 0x25;
    hook[2] = 0x00;
    hook[3] = 0x00;
    hook[4] = 0x00;
    hook[5] = 0x00;
    *(uint64_t *)(hook + 6) = dst;
    mprotect((void *)page_start, len, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)hook, (char *)hook + 14);
  }
}

void so_finalize(void) {
  size_t ps = sysconf(_SC_PAGESIZE);
  uintptr_t align_text = (uintptr_t)text_base & ~(ps - 1);
  size_t align_size =
  ((uintptr_t)text_base + text_size - align_text + ps - 1) & ~(ps - 1);
  mprotect((void *)align_text, align_size, PROT_READ | PROT_EXEC);
}

extern void __register_frame(const void *begin);

int so_load(const char *filename, void *base, size_t max_size) {
  FILE *fd = fopen(filename, "rb");
  if (!fd)
    return -1;
  fseek(fd, 0, SEEK_END);
  size_t so_size = ftell(fd);
  fseek(fd, 0, SEEK_SET);

  so_base = malloc(so_size);
  if (!so_base) {
    fclose(fd);
    return -1;
  }
  fread(so_base, so_size, 1, fd);
  fclose(fd);

  elf_hdr = (Elf64_Ehdr *)so_base;
  prog_hdr = (Elf64_Phdr *)((uintptr_t)so_base + elf_hdr->e_phoff);
  sec_hdr = (Elf64_Shdr *)((uintptr_t)so_base + elf_hdr->e_shoff);
  shstrtab = (char *)((uintptr_t)so_base + sec_hdr[elf_hdr->e_shstrndx].sh_offset);

  size_t max_end = 0;
  int exec_seg = -1;
  for (int i = 0; i < elf_hdr->e_phnum; i++) {
    if (prog_hdr[i].p_type == PT_LOAD) {
      size_t end = prog_hdr[i].p_vaddr + prog_hdr[i].p_memsz;
      if (end > max_end)
        max_end = end;
      if ((prog_hdr[i].p_flags & PF_X) == PF_X)
        exec_seg = i;
    } else if (prog_hdr[i].p_type == PT_TLS) {
      tls_template = (void *)((uintptr_t)so_base + prog_hdr[i].p_offset);
      tls_init_size = prog_hdr[i].p_filesz;
      tls_total_size = prog_hdr[i].p_memsz;
      tls_align = prog_hdr[i].p_align ? prog_hdr[i].p_align : 1;
    }
  }

  load_size = ALIGN_MEM(max_end, 0x1000);
  load_base = base;
  memset(load_base, 0, load_size);

  for (int i = 0; i < elf_hdr->e_phnum; i++) {
    if (prog_hdr[i].p_type != PT_LOAD)
      continue;
    memcpy((void *)((uintptr_t)load_base + prog_hdr[i].p_vaddr),
           (void *)((uintptr_t)so_base + prog_hdr[i].p_offset),
           prog_hdr[i].p_filesz);
  }

  text_size = prog_hdr[exec_seg].p_memsz;
  text_base = (void *)((uintptr_t)load_base + prog_hdr[exec_seg].p_vaddr);
  data_base = load_base;
  data_size = load_size;

  for (int i = 0; i < elf_hdr->e_shnum; i++) {
    char *sh_name = shstrtab + sec_hdr[i].sh_name;
    if (strcmp(sh_name, ".dynsym") == 0) {
      syms = (Elf64_Sym *)(sec_hdr[i].sh_addr
      ? ((uintptr_t)load_base + sec_hdr[i].sh_addr)
      : ((uintptr_t)so_base + sec_hdr[i].sh_offset));
      num_syms = sec_hdr[i].sh_size / sizeof(Elf64_Sym);
    } else if (strcmp(sh_name, ".dynstr") == 0) {
      dynstrtab = (char *)(sec_hdr[i].sh_addr
      ? ((uintptr_t)load_base + sec_hdr[i].sh_addr)
      : ((uintptr_t)so_base + sec_hdr[i].sh_offset));
    } else if (strcmp(sh_name, ".eh_frame") == 0) {
      if (sec_hdr[i].sh_addr != 0) {
        void *eh_frame_ptr = (void *)((uintptr_t)load_base + sec_hdr[i].sh_addr);
        __register_frame(eh_frame_ptr);
        printf("[so_util] Registered .eh_frame for %s at %p\n", filename, eh_frame_ptr);
      }
    }
  }

  // Register for dl_iterate_phdr hook
  if (g_num_modules < MAX_MODULES) {
    g_modules[g_num_modules].load_base = load_base;
    g_modules[g_num_modules].phdr = prog_hdr;
    g_modules[g_num_modules].phnum = elf_hdr->e_phnum;
    g_modules[g_num_modules].name = strdup(filename);
    g_num_modules++;
  }

  return 0;
}

int so_relocate(void) {
  int total_relocs = 0;
  for (int i = 0; i < elf_hdr->e_shnum; i++) {
    if (sec_hdr[i].sh_type == SHT_RELA) {
      Elf64_Rela *rels =
      (Elf64_Rela *)(sec_hdr[i].sh_addr
      ? ((uintptr_t)load_base + sec_hdr[i].sh_addr)
      : ((uintptr_t)so_base + sec_hdr[i].sh_offset));

      int count = (int)(sec_hdr[i].sh_size / sizeof(Elf64_Rela));
      for (int j = 0; j < count; j++) {
        uintptr_t *ptr = (uintptr_t *)((uintptr_t)load_base + rels[j].r_offset);
        Elf64_Sym *sym = &syms[ELF64_R_SYM(rels[j].r_info)];
        int type = ELF64_R_TYPE(rels[j].r_info);

        if (type == R_X86_64_RELATIVE) {
          *ptr = (uintptr_t)load_base + rels[j].r_addend;
          total_relocs++;
        } else if (type == R_X86_64_64 || type == R_X86_64_GLOB_DAT ||
          type == R_X86_64_JUMP_SLOT) {
          if (sym->st_shndx != SHN_UNDEF) {
            *ptr = (uintptr_t)load_base + sym->st_value + rels[j].r_addend;
            total_relocs++;
          }
          } else if (type == R_X86_64_TPOFF64) {
            *ptr = sym->st_value + rels[j].r_addend - tls_total_size;
            total_relocs++;
          } else if (type == R_X86_64_TPOFF32) {
            *(uint32_t *)ptr = (uint32_t)(sym->st_value + rels[j].r_addend - tls_total_size);
            total_relocs++;
          } else if (type == R_X86_64_DTPOFF64) {
            *ptr = sym->st_value + rels[j].r_addend;
            total_relocs++;
          } else if (type == R_X86_64_DTPMOD64) {
            *ptr = 1;
            total_relocs++;
          }
      }
    }
  }
  printf("[so_util] Relocated %d internal pointers.\n", total_relocs);
  return 0;
}

int so_resolve(DynLibFunction *funcs, int num_funcs, int taint_missing_imports) {
  int resolved_count = 0;

  if (!g_libc_handle)
    g_libc_handle = dlopen("libc.so.6", RTLD_LAZY | RTLD_GLOBAL);
  if (!g_libpthread_handle)
    g_libpthread_handle = dlopen("libpthread.so.0", RTLD_LAZY | RTLD_GLOBAL);

  for (int i = 0; i < elf_hdr->e_shnum; i++) {
    if (sec_hdr[i].sh_type != SHT_RELA) continue;

    Elf64_Rela *rels =
    (Elf64_Rela *)(sec_hdr[i].sh_addr
    ? ((uintptr_t)load_base + sec_hdr[i].sh_addr)
    : ((uintptr_t)so_base + sec_hdr[i].sh_offset));

    int count = (int)(sec_hdr[i].sh_size / sizeof(Elf64_Rela));
    for (int j = 0; j < count; j++) {
      uintptr_t *ptr = (uintptr_t *)((uintptr_t)load_base + rels[j].r_offset);
      Elf64_Sym *sym = &syms[ELF64_R_SYM(rels[j].r_info)];
      int type = ELF64_R_TYPE(rels[j].r_info);

      if (type == R_X86_64_GLOB_DAT || type == R_X86_64_JUMP_SLOT || type == R_X86_64_64) {
        if (sym->st_shndx == SHN_UNDEF) {
          char *name = dynstrtab + sym->st_name;
          uintptr_t resolved = 0;

          for (int k = 0; k < num_funcs; k++) {
            if (strcmp(name, funcs[k].symbol) == 0) {
              resolved = funcs[k].func;
              break;
            }
          }

          if (!resolved) resolved = (uintptr_t)dlsym(RTLD_DEFAULT, name);
          if (!resolved && g_libc_handle) resolved = (uintptr_t)dlsym(g_libc_handle, name);
          if (!resolved && g_libpthread_handle) resolved = (uintptr_t)dlsym(g_libpthread_handle, name);

          if (resolved) {
            *ptr = resolved + rels[j].r_addend;
            resolved_count++;
          } else {
            if (taint_missing_imports)
              *ptr = rels[j].r_offset;
            else
              *ptr = 0;
            char *error = dlerror();
            fprintf(stderr, "*** UNRESOLVED import: \"%s\" (dlerror: %s) ***\n",
                    name, error ? error : "none");
          }
        }
      } else if (type == R_X86_64_TPOFF64) {
        *ptr = sym->st_value + rels[j].r_addend - tls_total_size;
        resolved_count++;
      } else if (type == R_X86_64_TPOFF32) {
        *(uint32_t *)ptr = (uint32_t)(sym->st_value + rels[j].r_addend - tls_total_size);
        resolved_count++;
      }
    }
  }
  printf("[so_util] Resolved %d import symbols.\n", resolved_count);
  return 0;
}

void so_override_imports(const ImportOverride *ovr, int n) {
  int patched = 0;
  if (n <= 0 || !ovr) return;
  for (int i = 0; i < elf_hdr->e_shnum; i++) {
    if (sec_hdr[i].sh_type != SHT_RELA) continue;

    Elf64_Rela *rels =
      (Elf64_Rela *)(sec_hdr[i].sh_addr
      ? ((uintptr_t)load_base + sec_hdr[i].sh_addr)
      : ((uintptr_t)so_base + sec_hdr[i].sh_offset));
    int count = (int)(sec_hdr[i].sh_size / sizeof(Elf64_Rela));

    for (int j = 0; j < count; j++) {
      int type = ELF64_R_TYPE(rels[j].r_info);
      if (type != R_X86_64_GLOB_DAT && type != R_X86_64_JUMP_SLOT &&
          type != R_X86_64_64)
        continue;
      Elf64_Sym *sym = &syms[ELF64_R_SYM(rels[j].r_info)];
      if (sym->st_shndx != SHN_UNDEF) continue;

      char *name = dynstrtab + sym->st_name;
      for (int k = 0; k < n; k++) {
        if (strcmp(name, ovr[k].name) != 0) continue;
        uintptr_t *g = (uintptr_t *)((uintptr_t)load_base + rels[j].r_offset);
        if (ovr[k].real_out && !*ovr[k].real_out)
          *ovr[k].real_out = *g;
        *g = ovr[k].func + rels[j].r_addend;
        patched++;
        break;
      }
    }
  }
  fprintf(stderr, "[so_util] Overrode %d import entries for %d symbol(s).\n",
          patched, n);
}

void so_execute_init_array(void) {
  for (int i = 0; i < elf_hdr->e_shnum; i++) {
    char *sh_name = shstrtab + sec_hdr[i].sh_name;
    if (strcmp(sh_name, ".init_array") == 0) {
      int n = (int)(sec_hdr[i].sh_size / 8);
      int (**init_array)() =
      (void *)((uintptr_t)load_base + sec_hdr[i].sh_addr);
      printf("so_execute_init_array: Executing %d constructors...\n", n);
      for (int j = 0; j < n; j++) {
        if (init_array[j] != 0) {
          init_array[j]();
        }
      }
    }
  }
}

uintptr_t so_find_addr_safe(const char *symbol) {
  for (int i = 0; i < num_syms; i++) {
    char *name = dynstrtab + syms[i].st_name;
    if (strcmp(name, symbol) == 0)
      return (uintptr_t)load_base + syms[i].st_value;
  }
  return 0;
}

DynLibFunction *so_snapshot_symbols(int *out_count) {
  if (!syms || num_syms <= 0 || !dynstrtab) {
    if (out_count) *out_count = 0;
    return NULL;
  }
  int n = 0;
  for (int i = 0; i < num_syms; i++) {
    if (syms[i].st_shndx == SHN_UNDEF || syms[i].st_value == 0 ||
      syms[i].st_name == 0)
      continue;
    int bind = ELF64_ST_BIND(syms[i].st_info);
    if (bind == STB_GLOBAL || bind == STB_WEAK)
      n++;
  }

  DynLibFunction *tbl = malloc(sizeof(DynLibFunction) * (n > 0 ? n : 1));
  int k = 0;
  for (int i = 0; i < num_syms; i++) {
    if (syms[i].st_shndx == SHN_UNDEF || syms[i].st_value == 0 ||
      syms[i].st_name == 0)
      continue;
    int bind = ELF64_ST_BIND(syms[i].st_info);
    if (bind == STB_GLOBAL || bind == STB_WEAK) {
      tbl[k].symbol = dynstrtab + syms[i].st_name;
      tbl[k].func = (uintptr_t)load_base + syms[i].st_value;
      k++;
    }
  }

  if (out_count)
    *out_count = k;
  return tbl;
}

uintptr_t so_find_addr(const char *symbol) {
  uintptr_t addr = so_find_addr_safe(symbol);
  if (!addr) {
    fprintf(stderr, "FATAL: symbol '%s' not found\n", symbol);
    exit(1);
  }
  return addr;
}

void so_make_text_writable(void) {
  size_t ps = sysconf(_SC_PAGESIZE);
  uintptr_t start = (uintptr_t)text_base & ~(ps - 1);
  size_t len = ((uintptr_t)text_base + text_size - start + ps - 1) & ~(ps - 1);
  if (mprotect((void *)start, len, PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
    perror("so_make_text_writable");
}

void so_make_text_executable(void) {
  size_t ps = sysconf(_SC_PAGESIZE);
  uintptr_t start = (uintptr_t)text_base & ~(ps - 1);
  size_t len = ((uintptr_t)text_base + text_size - start + ps - 1) & ~(ps - 1);
  if (mprotect((void *)start, len, PROT_READ | PROT_EXEC) != 0)
    perror("so_make_text_executable");
}
