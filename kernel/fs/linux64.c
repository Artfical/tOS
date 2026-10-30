/* 64-bit Linux programs (the x86-64 ABI): their address space, the ELF64 loader (with the dynamic
 * linker a program names in PT_INTERP) and the syscall instruction's numbers. Most calls go to
 * the same code 32-bit programs use (syscall.c, i386 numbering) when the structures they pass
 * look the same; the rest (stat, iovec, msghdr, timeval, sigaction, clone...) are here. */
#include "linux_abi.h"
#include "syscall.h"
#include "proc.h"
#include "scheduler.h"
#include "paging.h"
#include "memory.h"
#include "string.h"
#include "vfs.h"
#include "fsbridge.h"
#include "errno.h"
#include "serial.h"
#include "auth.h"
#include "csprng.h"
#include "io.h"
#include "debugmon.h"
#include "usermode.h"

void sprintf_u32(char *out, uint32_t v);

#define MSR_EFER   0xC0000080u
#define MSR_STAR   0xC0000081u
#define MSR_LSTAR  0xC0000082u
#define MSR_FMASK  0xC0000084u
#define MSR_FS     0xC0000100u
#define MSR_GS     0xC0000101u

#define PROT_READ  1u
#define PROT_WRITE 2u
#define PROT_EXEC  4u

#define MAP_SHARED    0x01u
#define MAP_PRIVATE   0x02u
#define MAP_FIXED     0x10u
#define MAP_ANONYMOUS 0x20u
#define MAP_FIXED_NOREPLACE 0x100000u

/* where things go in a 64-bit address space */
#define STACK_TOP      0x00007FFFFFFFE000ull
#define STACK_SIZE     (8ull << 20)               /* reserved; pages come when touched */
#define MMAP_TOP       0x00007F0000000000ull       /* mmap without a hint: downwards from here */
#define MMAP_BOTTOM    0x0000000010000000ull
#define PIE_BASE       0x0000555555554000ull
#define INTERP_BASE    0x00007FF000000000ull

static void wrmsr(uint32_t msr, uint64_t v)
{
    asm volatile("wrmsr" : : "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}

static uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

extern void syscall_entry64(void);

void linux64_init(void)
{
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | 1u);                     /* SCE: the syscall instruction */
    /* kernel CS 0x08 (SS 0x10); sysret's base 0x18: CS 0x2B, SS 0x23 */
    wrmsr(MSR_STAR, (0x18ull << 48) | (0x08ull << 32));
    wrmsr(MSR_LSTAR, (uint64_t)(uintptr_t)syscall_entry64);
    wrmsr(MSR_FMASK, 0x47700u);                                 /* IF, TF, DF, AC, NT off on entry */
}

/* ------------------------------------------------------------------ the address space */

typedef struct {
    uint64_t start, end;
    uint32_t prot;
} vma_t;

typedef struct mm64 {
    vma_t *v;
    int n, cap;
    uint64_t brk_base, brk;
} mm64_t;

static mm64_t *mm_of(proc_t *p) { return p ? (mm64_t *)p->mm64 : 0; }

static uint64_t pte_flags(uint32_t prot)
{
    if (!(prot & (PROT_READ | PROT_WRITE | PROT_EXEC))) return 0;      /* PROT_NONE: kept, not present */
    uint64_t f = PTE_PRESENT | PTE_USER;
    if (prot & PROT_WRITE) f |= PTE_WRITABLE;
    if (!(prot & PROT_EXEC)) f |= PTE_NX;
    return f;
}

static int vma_index(mm64_t *mm, uint64_t addr)
{
    int lo = 0, hi = mm->n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (addr < mm->v[mid].start) hi = mid - 1;
        else if (addr >= mm->v[mid].end) lo = mid + 1;
        else return mid;
    }
    return -1;
}

static int vma_grow(mm64_t *mm, int need)
{
    if (mm->n + need <= mm->cap) return 0;
    int cap = mm->cap ? mm->cap * 2 : 64;
    while (cap < mm->n + need) cap *= 2;
    vma_t *nv = malloc(sizeof(vma_t) * (uint32_t)cap);
    if (!nv) return -1;
    if (mm->v) { memcpy(nv, mm->v, sizeof(vma_t) * (uint32_t)mm->n); free(mm->v); }
    mm->v = nv;
    mm->cap = cap;
    return 0;
}

/* frees the pages of [start, end) in the tables, visiting only the parts that exist */
static void unmap_level(uint64_t *t, int level, uint64_t base, uint64_t start, uint64_t end)
{
    uint64_t span = 1ull << (12 + 9 * (level - 1));
    for (int i = 0; i < 512; i++) {
        uint64_t lo = base + (uint64_t)i * span, hi = lo + span;
        if (hi <= start || lo >= end) continue;
        if (!(t[i] & PTE_PRESENT) && !(level == 1 && (t[i] & PTE_SAVED))) continue;
        uint64_t phys = t[i] & 0x000FFFFFFFFFF000ull;
        if (level == 1) {
            free_physical_page((uint32_t)phys);
            t[i] = 0;
            asm volatile("invlpg (%0)" : : "r"(lo) : "memory");
            continue;
        }
        uint64_t *next = (uint64_t *)P2V(phys);
        unmap_level(next, level - 1, lo, start, end);
        if (start <= lo && end >= hi) {                     /* the whole table: it goes too */
            free_physical_page((uint32_t)phys);
            t[i] = 0;
        }
    }
}

static void protect_level(uint64_t *t, int level, uint64_t base, uint64_t start, uint64_t end, uint64_t flags)
{
    uint64_t span = 1ull << (12 + 9 * (level - 1));
    for (int i = 0; i < 512; i++) {
        uint64_t lo = base + (uint64_t)i * span, hi = lo + span;
        if (hi <= start || lo >= end) continue;
        if (!(t[i] & PTE_PRESENT) && !(level == 1 && (t[i] & PTE_SAVED))) continue;
        if (level == 1) {
            /* no access at all: not present, the page kept (PTE_SAVED) for when access comes back */
            uint64_t phys = t[i] & 0x000FFFFFFFFFF000ull;
            t[i] = (flags & PTE_PRESENT) ? phys | paging_cow_flags(phys, flags) : phys | PTE_SAVED;
            asm volatile("invlpg (%0)" : : "r"(lo) : "memory");
            continue;
        }
        protect_level((uint64_t *)P2V(t[i] & 0x000FFFFFFFFFF000ull), level - 1, lo, start, end, flags);
    }
}

/* takes [start, end) out of the map (splitting regions) and frees its pages */
static int vma_remove(proc_t *p, uint64_t start, uint64_t end)
{
    mm64_t *mm = mm_of(p);
    if (vma_grow(mm, 1) != 0) return -1;
    for (int i = 0; i < mm->n; i++) {
        vma_t *r = &mm->v[i];
        if (r->end <= start || r->start >= end) continue;
        if (r->start < start && r->end > end) {             /* the hole is inside: split */
            memmove(&mm->v[i + 2], &mm->v[i + 1], sizeof(vma_t) * (uint32_t)(mm->n - i - 1));
            mm->v[i + 1] = *r;
            mm->v[i + 1].start = end;
            r->end = start;
            mm->n++;
            break;
        }
        if (r->start < start) { r->end = start; continue; }
        if (r->end > end) { r->start = end; continue; }
        memmove(&mm->v[i], &mm->v[i + 1], sizeof(vma_t) * (uint32_t)(mm->n - i - 1));
        mm->n--;
        i--;
    }
    unmap_level(p->pgdir, 4, 0, start, end);
    return 0;
}

/* a region (anything there before is gone); neighbours with the same protection join it */
static int vma_insert(proc_t *p, uint64_t start, uint64_t end, uint32_t prot)
{
    mm64_t *mm = mm_of(p);
    if (vma_remove(p, start, end) != 0 || vma_grow(mm, 1) != 0) return -1;
    int i = 0;
    while (i < mm->n && mm->v[i].start < start) i++;
    memmove(&mm->v[i + 1], &mm->v[i], sizeof(vma_t) * (uint32_t)(mm->n - i));
    mm->v[i].start = start;
    mm->v[i].end = end;
    mm->v[i].prot = prot;
    mm->n++;
    if (i + 1 < mm->n && mm->v[i + 1].start == end && mm->v[i + 1].prot == prot) {
        mm->v[i].end = mm->v[i + 1].end;
        memmove(&mm->v[i + 1], &mm->v[i + 2], sizeof(vma_t) * (uint32_t)(mm->n - i - 2));
        mm->n--;
    }
    if (i > 0 && mm->v[i - 1].end == start && mm->v[i - 1].prot == prot) {
        mm->v[i - 1].end = mm->v[i].end;
        memmove(&mm->v[i], &mm->v[i + 1], sizeof(vma_t) * (uint32_t)(mm->n - i - 1));
        mm->n--;
    }
    return 0;
}

/* changes the protection of [start, end): regions split where needed, mapped pages follow */
static int vma_protect(proc_t *p, uint64_t start, uint64_t end, uint32_t prot)
{
    mm64_t *mm = mm_of(p);
    /* every page of the range has to be in the map */
    uint64_t at = start;
    while (at < end) {
        int i = vma_index(mm, at);
        if (i < 0) return -ENOMEM;
        at = mm->v[i].end;
    }
    /* rebuild the covered part piece by piece with the new protection */
    at = start;
    while (at < end) {
        int i = vma_index(mm, at);
        uint64_t stop = mm->v[i].end < end ? mm->v[i].end : end;
        /* edited in place: vma_insert would free the pages */
        vma_t *r = &mm->v[i];
        if (r->prot != prot) {
            if (vma_grow(mm, 2) != 0) return -ENOMEM;
            r = &mm->v[i];
            vma_t old = *r;
            int parts = 0;
            vma_t piece[3];
            if (old.start < at) piece[parts++] = (vma_t){ old.start, at, old.prot };
            piece[parts++] = (vma_t){ at, stop, prot };
            if (stop < old.end) piece[parts++] = (vma_t){ stop, old.end, old.prot };
            memmove(&mm->v[i + parts], &mm->v[i + 1], sizeof(vma_t) * (uint32_t)(mm->n - i - 1));
            for (int k = 0; k < parts; k++) mm->v[i + k] = piece[k];
            mm->n += parts - 1;
        }
        at = stop;
    }
    protect_level(p->pgdir, 4, 0, start, end, pte_flags(prot));
    return 0;
}

/* a free stretch of `size` bytes: at the hint if it is free, else the highest gap below MMAP_TOP */
static uint64_t vma_find_free(mm64_t *mm, uint64_t hint, uint64_t size)
{
    if (hint && hint >= MMAP_BOTTOM && hint + size > hint && hint + size <= STACK_TOP - STACK_SIZE) {
        int clash = 0;
        for (int i = 0; i < mm->n && !clash; i++)
            if (mm->v[i].start < hint + size && mm->v[i].end > hint) clash = 1;
        if (!clash) return hint;
    }
    uint64_t top = MMAP_TOP;
    for (int i = mm->n - 1; i >= -1; i--) {
        uint64_t below = i >= 0 ? mm->v[i].end : MMAP_BOTTOM;
        if (i >= 0 && mm->v[i].start >= top) continue;
        if (below < MMAP_BOTTOM) below = MMAP_BOTTOM;
        if (top >= below + size) return top - size;
        if (i >= 0) top = mm->v[i].start;
    }
    return 0;
}

/* makes the page at `addr` present if the map allows the access; 1 = it is there now */
static int fault_in(proc_t *p, uint64_t addr, int write)
{
    mm64_t *mm = mm_of(p);
    if (!mm) return 0;
    int i = vma_index(mm, addr);
    if (i < 0) return 0;
    uint32_t prot = mm->v[i].prot;
    if (!(prot & (PROT_READ | PROT_WRITE | PROT_EXEC))) return 0;
    if (write && !(prot & PROT_WRITE)) return 0;
    uint64_t page = addr & ~0xFFFull;
    if (paging_virt_to_phys(p->pgdir, page)) {
        if (write && paging_cow_break(p->pgdir, page) < 0) return 0;     /* shared since a fork */
        return 1;
    }
    if (paging_saved_page(p->pgdir, page)) {          /* kept while it was PROT_NONE */
        protect_level(p->pgdir, 4, 0, page, page + PAGE_SIZE, pte_flags(prot));
        return 1;
    }
    uint32_t phys = alloc_physical_page();
    if (!phys) return 0;
    memset(P2V(phys), 0, PAGE_SIZE);
    if (paging_map_in(p->pgdir, page, phys, pte_flags(prot)) != 0) { free_physical_page(phys); return 0; }
    return 1;
}

int linux64_page_fault(uint64_t addr, uint64_t err)
{
    proc_t *p = proc_current();
    if (!p || p->linux_abi != 2 || (err & 1u)) return 0;           /* only pages not there yet */
    return fault_in(p, addr, (err & 2u) != 0);
}

/* the kernel is about to touch [addr, addr+len) of a 64-bit program: bring its pages in */
void linux64_prefault(uint64_t addr, uint64_t len, int write)
{
    proc_t *p = proc_current();
    if (!p || p->linux_abi != 2 || !len || addr + len < addr) return;
    for (uint64_t pg = addr & ~0xFFFull; pg < addr + len; pg += PAGE_SIZE)
        if (!fault_in(p, pg, write)) return;
}

/* copies `n` bytes of the file into the program's memory at `va` (pages made as needed) */
static int load_file_bytes(proc_t *p, const char *path, uint64_t off, uint64_t va, uint64_t n, uint32_t prot)
{
    uint8_t *buf = malloc(PAGE_SIZE);
    if (!buf) return -1;
    while (n) {
        uint64_t page = va & ~0xFFFull;
        uint32_t phys = (uint32_t)paging_virt_to_phys(p->pgdir, page);
        if (!phys) {
            phys = alloc_physical_page();
            if (!phys) { free(buf); return -1; }
            memset(P2V(phys), 0, PAGE_SIZE);
            if (paging_map_in(p->pgdir, page, phys, pte_flags(prot)) != 0) { free(buf); return -1; }
        } else {
            phys &= ~0xFFFu;
        }
        uint64_t in_page = PAGE_SIZE - (va & 0xFFF);
        uint64_t chunk = n < in_page ? n : in_page;
        int got = fsbridge_read(path, buf, (uint32_t)chunk, (uint32_t)off);
        if (got < 0) { free(buf); return -1; }
        memcpy((uint8_t *)P2V(phys) + (va & 0xFFF), buf, (size_t)got);
        if ((uint64_t)got < chunk) break;                               /* past the end of the file */
        va += chunk; off += chunk; n -= chunk;
    }
    free(buf);
    return 0;
}

static const char *fd_path(proc_t *p, int fd)
{
    if (!p || fd < 3 || fd - 3 >= PROC_MAX_FDS || p->fd[fd - 3] < 0) return 0;
    return p->fdpath[fd - 3];
}

static uint64_t sys_mmap(uint64_t addr, uint64_t len, uint32_t prot, uint32_t flags, int fd, uint64_t off)
{
    proc_t *p = proc_current();
    mm64_t *mm = mm_of(p);
    if (!mm || !len || (off & 0xFFF)) return (uint64_t)-EINVAL;
    uint64_t size = (len + 0xFFF) & ~0xFFFull;
    if (size < len) return (uint64_t)-ENOMEM;
    const char *path = 0;
    if (!(flags & MAP_ANONYMOUS)) {
        path = fd_path(p, fd);
        if (!path) return (uint64_t)-EBADF;
    }
    uint64_t start;
    if (flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) {
        if (addr & 0xFFF) return (uint64_t)-EINVAL;
        if (addr + size > PAGING_USER_END || addr < PAGE_SIZE) return (uint64_t)-ENOMEM;
        if (flags & MAP_FIXED_NOREPLACE)
            for (int i = 0; i < mm->n; i++)
                if (mm->v[i].start < addr + size && mm->v[i].end > addr) return (uint64_t)-EEXIST;
        start = addr;
    } else {
        start = vma_find_free(mm, addr & ~0xFFFull, size);
        if (!start) return (uint64_t)-ENOMEM;
    }
    if (vma_insert(p, start, start + size, prot) != 0) return (uint64_t)-ENOMEM;
    if (path) {
        /* a private copy of the file's bytes (MAP_SHARED files are read the same way: tOS writes
         * nothing back) */
        uint64_t fsize = fsbridge_size(path);
        uint64_t n = off < fsize ? fsize - off : 0;
        if (n > size) n = size;
        if (n && load_file_bytes(p, path, off, start, n, prot | PROT_WRITE) != 0) {
            vma_remove(p, start, start + size);
            return (uint64_t)-ENOMEM;
        }
        if (n) protect_level(p->pgdir, 4, 0, start, start + size, pte_flags(prot));
    }
    return start;
}

static uint64_t sys_munmap(uint64_t addr, uint64_t len)
{
    proc_t *p = proc_current();
    if (!mm_of(p) || (addr & 0xFFF) || !len) return (uint64_t)-EINVAL;
    uint64_t end = addr + ((len + 0xFFF) & ~0xFFFull);
    if (end > PAGING_USER_END || end < addr) return (uint64_t)-EINVAL;
    return vma_remove(p, addr, end) == 0 ? 0 : (uint64_t)-ENOMEM;
}

static uint64_t sys_mprotect(uint64_t addr, uint64_t len, uint32_t prot)
{
    proc_t *p = proc_current();
    if (!mm_of(p) || (addr & 0xFFF)) return (uint64_t)-EINVAL;
    if (!len) return 0;
    uint64_t end = addr + ((len + 0xFFF) & ~0xFFFull);
    int r = vma_protect(p, addr, end, prot);
    return r ? (uint64_t)(int64_t)r : 0;
}

static uint64_t sys_brk(uint64_t want)
{
    proc_t *p = proc_current();
    mm64_t *mm = mm_of(p);
    if (!mm) return 0;
    if (want < mm->brk_base || want > mm->brk_base + (1ull << 32)) return mm->brk;
    uint64_t old_end = (mm->brk + 0xFFF) & ~0xFFFull, new_end = (want + 0xFFF) & ~0xFFFull;
    if (new_end > old_end) {
        for (int i = 0; i < mm->n; i++)
            if (mm->v[i].start < new_end && mm->v[i].end > old_end) return mm->brk;
        if (vma_insert(p, old_end, new_end, PROT_READ | PROT_WRITE) != 0) return mm->brk;
    } else if (new_end < old_end) {
        vma_remove(p, new_end, old_end);
    }
    mm->brk = want;
    return want;
}

/* mremap: grows or shrinks in place when it can, else moves (MREMAP_MAYMOVE) by copying */
static uint64_t sys_mremap(uint64_t old, uint64_t old_len, uint64_t new_len, uint32_t flags)
{
    proc_t *p = proc_current();
    mm64_t *mm = mm_of(p);
    if (!mm || (old & 0xFFF) || !new_len) return (uint64_t)-EINVAL;
    old_len = (old_len + 0xFFF) & ~0xFFFull;
    new_len = (new_len + 0xFFF) & ~0xFFFull;
    int i = vma_index(mm, old);
    if (i < 0) return (uint64_t)-EFAULT;
    uint32_t prot = mm->v[i].prot;
    if (new_len <= old_len) {
        if (new_len < old_len) vma_remove(p, old + new_len, old + old_len);
        return old;
    }
    int free_after = 1;
    for (int k = 0; k < mm->n; k++)
        if (mm->v[k].start < old + new_len && mm->v[k].end > old + old_len) free_after = 0;
    if (free_after) {
        vma_insert(p, old + old_len, old + new_len, prot);
        return old;
    }
    if (!(flags & 1u)) return (uint64_t)-ENOMEM;                           /* MREMAP_MAYMOVE */
    uint64_t dst = vma_find_free(mm, 0, new_len);
    if (!dst || vma_insert(p, dst, dst + new_len, prot) != 0) return (uint64_t)-ENOMEM;
    linux64_prefault(old, old_len, 0);
    linux64_prefault(dst, old_len, 1);
    for (uint64_t o = 0; o < old_len; o += PAGE_SIZE) {
        uint64_t s = paging_virt_to_phys(p->pgdir, old + o), d = paging_virt_to_phys(p->pgdir, dst + o);
        if (s && d) memcpy(P2V(d & ~0xFFFull), P2V(s & ~0xFFFull), PAGE_SIZE);
    }
    vma_remove(p, old, old + old_len);
    return dst;
}

int linux64_mm_clone(proc_t *child, proc_t *parent)
{
    mm64_t *src = mm_of(parent);
    child->mm64 = 0;
    if (!src) return 0;
    mm64_t *mm = malloc(sizeof(mm64_t));
    if (!mm) return -1;
    *mm = *src;
    mm->v = 0;
    if (src->cap) {
        mm->v = malloc(sizeof(vma_t) * (uint32_t)src->cap);
        if (!mm->v) { free(mm); return -1; }
        memcpy(mm->v, src->v, sizeof(vma_t) * (uint32_t)src->n);
    }
    child->mm64 = mm;
    return 0;
}

void linux64_mm_free(proc_t *p)
{
    mm64_t *mm = mm_of(p);
    if (!mm) return;
    if (mm->v) free(mm->v);
    free(mm);
    p->mm64 = 0;
}

/* ------------------------------------------------------------------ the ELF64 loader */

typedef struct {
    uint8_t  ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} __attribute__((packed)) elf64_ehdr_t;

typedef struct {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
} __attribute__((packed)) elf64_phdr_t;

#define PT_LOAD   1
#define PT_INTERP 3
#define PT_PHDR   6
#define PF_X 1u
#define PF_W 2u
#define PF_R 4u

int linux64_is_elf64(const char *path)
{
    uint8_t h[20];
    if (fsbridge_read(path, h, sizeof(h), 0) != (int)sizeof(h)) return 0;
    return h[0] == 0x7F && h[1] == 'E' && h[2] == 'L' && h[3] == 'F' && h[4] == 2 &&
           h[18] == 62;                                                          /* EM_X86_64 */
}

/* maps an ELF64's segments at `bias`; *entry, *phdr (in memory), *phnum, *interp (malloc'd) */
static int load_elf64(proc_t *p, const char *path, uint64_t *bias_io, uint64_t *entry,
                      uint64_t *phdr_va, uint16_t *phnum, char **interp, uint64_t *end_out)
{
    elf64_ehdr_t eh;
    if (fsbridge_read(path, &eh, sizeof(eh), 0) != (int)sizeof(eh)) return -ENOEXEC;
    if (eh.ident[4] != 2 || eh.machine != 62 || (eh.type != 2 && eh.type != 3) ||
        eh.phentsize != sizeof(elf64_phdr_t) || !eh.phnum || eh.phnum > 64) return -ENOEXEC;
    elf64_phdr_t ph[64];
    if (fsbridge_read(path, ph, (uint32_t)(eh.phnum * sizeof(elf64_phdr_t)), (uint32_t)eh.phoff) !=
        (int)(eh.phnum * sizeof(elf64_phdr_t))) return -ENOEXEC;
    uint64_t bias = eh.type == 3 ? *bias_io : 0;                                /* ET_DYN */
    uint64_t lo = ~0ull, hi = 0;
    *phdr_va = 0;
    if (interp) *interp = 0;
    for (int i = 0; i < eh.phnum; i++) {
        if (ph[i].type == PT_LOAD) {
            if (ph[i].vaddr < lo) lo = ph[i].vaddr;
            if (ph[i].vaddr + ph[i].memsz > hi) hi = ph[i].vaddr + ph[i].memsz;
        }
        if (ph[i].type == PT_PHDR) *phdr_va = ph[i].vaddr + bias;
        if (ph[i].type == PT_INTERP && interp && ph[i].filesz < 256) {
            char *s = malloc((uint32_t)ph[i].filesz + 1);
            if (!s) return -ENOMEM;
            fsbridge_read(path, s, (uint32_t)ph[i].filesz, (uint32_t)ph[i].offset);
            s[ph[i].filesz] = 0;
            *interp = s;
        }
    }
    if (hi <= lo) return -ENOEXEC;
    for (int i = 0; i < eh.phnum; i++) {
        if (ph[i].type != PT_LOAD || !ph[i].memsz) continue;
        uint32_t prot = ((ph[i].flags & PF_R) ? PROT_READ : 0) | ((ph[i].flags & PF_W) ? PROT_WRITE : 0) |
                        ((ph[i].flags & PF_X) ? PROT_EXEC : 0);
        uint64_t va = ph[i].vaddr + bias;
        uint64_t start = va & ~0xFFFull, end = (va + ph[i].memsz + 0xFFF) & ~0xFFFull;
        if (end > PAGING_USER_END || start < PAGE_SIZE) return -ENOEXEC;
        int shared = vma_index(mm_of(p), start);
        if (shared >= 0) {
            /* the page the previous segment ends in: it keeps its contents and allows both */
            uint32_t both = mm_of(p)->v[shared].prot | prot;
            if (vma_protect(p, start, start + PAGE_SIZE, both) != 0) return -ENOMEM;
            start += PAGE_SIZE;
        }
        if (start < end && vma_insert(p, start, end, prot) != 0) return -ENOMEM;
        if (ph[i].filesz && load_file_bytes(p, path, ph[i].offset, va, ph[i].filesz, prot | PROT_WRITE) != 0)
            return -ENOMEM;
        if (!*phdr_va && eh.phoff >= ph[i].offset && eh.phoff < ph[i].offset + ph[i].filesz)
            *phdr_va = va + (eh.phoff - ph[i].offset);
    }
    /* write access was for loading: every page gets its region's protection */
    {
        mm64_t *mm = mm_of(p);
        uint64_t lo_va = (lo + bias) & ~0xFFFull, hi_va = (hi + bias + 0xFFF) & ~0xFFFull;
        for (int k = 0; k < mm->n; k++) {
            uint64_t s0 = mm->v[k].start > lo_va ? mm->v[k].start : lo_va;
            uint64_t e0 = mm->v[k].end < hi_va ? mm->v[k].end : hi_va;
            if (s0 < e0) protect_level(p->pgdir, 4, 0, s0, e0, pte_flags(mm->v[k].prot));
        }
    }
    *entry = eh.entry + bias;
    *phnum = eh.phnum;
    *bias_io = bias;
    if (end_out) *end_out = (hi + bias + 0xFFF) & ~0xFFFull;
    return 0;
}

/* writes into the program's (already present) memory */
static int put_user(proc_t *p, uint64_t va, const void *src, uint64_t n)
{
    const uint8_t *s = src;
    while (n) {
        if (!fault_in(p, va, 1)) return -1;
        uint64_t phys = paging_virt_to_phys(p->pgdir, va);
        uint64_t chunk = PAGE_SIZE - (va & 0xFFF);
        if (chunk > n) chunk = n;
        memcpy(P2V(phys), s, (size_t)chunk);
        va += chunk; s += chunk; n -= chunk;
    }
    return 0;
}

/* Builds the address space of a 64-bit program: its segments, the dynamic linker (PT_INTERP),
 * the stack with argv, envp and the auxiliary vector. The process `p` must have its tables
 * (p->pgdir) and no map yet. */
int linux64_load(proc_t *p, const char *path, int argc, const char *const argv[],
                 int envc, const char *const envp[], uint64_t *entry_out, uint64_t *sp_out)
{
    mm64_t *mm = malloc(sizeof(mm64_t));
    if (!mm) return -ENOMEM;
    memset(mm, 0, sizeof(*mm));
    p->mm64 = mm;

    uint64_t bias = PIE_BASE, entry, phdr, end;
    uint16_t phnum;
    char *interp = 0;
    int rc = load_elf64(p, path, &bias, &entry, &phdr, &phnum, &interp, &end);
    if (rc) return rc;
    mm->brk_base = mm->brk = end;
    uint64_t start = entry, interp_base = 0;
    if (interp) {
        uint64_t ib = INTERP_BASE, ientry, iphdr;
        uint16_t iphnum;
        rc = load_elf64(p, interp, &ib, &ientry, &iphdr, &iphnum, 0, 0);
        free(interp);
        if (rc) return rc;
        interp_base = ib;
        start = ientry;
    }

    /* the stack: reserved whole, pages when touched */
    if (vma_insert(p, STACK_TOP - STACK_SIZE, STACK_TOP, PROT_READ | PROT_WRITE) != 0) return -ENOMEM;
    uint64_t sp = STACK_TOP;
    uint64_t *argv_va = malloc(sizeof(uint64_t) * (uint32_t)(argc + envc + 2));
    if (!argv_va) return -ENOMEM;
#define PUSH_STR(s, out) do { uint64_t l_ = strlen(s) + 1; sp -= l_; if (put_user(p, sp, (s), l_)) { free(argv_va); return -E2BIG; } (out) = sp; } while (0)
    uint64_t execfn;
    PUSH_STR(path, execfn);
    for (int i = envc - 1; i >= 0; i--) PUSH_STR(envp[i], argv_va[argc + 1 + i]);
    for (int i = argc - 1; i >= 0; i--) PUSH_STR(argv[i], argv_va[i]);
    uint64_t platform;
    PUSH_STR("x86_64", platform);
#undef PUSH_STR
    uint8_t rnd[16];
    csprng_fill(rnd, sizeof(rnd));
    sp -= 16;
    put_user(p, sp, rnd, 16);
    uint64_t random_va = sp;

    uint32_t a, b, c, d;
    asm volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1));
    uint64_t aux[] = {
        3, phdr, 4, sizeof(elf64_phdr_t), 5, phnum, 6, PAGE_SIZE, 7, interp_base, 8, 0, 9, entry,
        11, auth_uid(), 12, auth_uid(), 13, auth_gid(), 14, auth_gid(), 16, d, 17, 100,
        23, 0, 25, random_va, 31, execfn, 15, platform, 0, 0
    };
    uint64_t words = 1 + (uint64_t)argc + 1 + (uint64_t)envc + 1 + sizeof(aux) / 8;
    sp = (sp - words * 8) & ~15ull;
    uint64_t at = sp;
    uint64_t v = (uint64_t)argc;
    put_user(p, at, &v, 8); at += 8;
    for (int i = 0; i < argc; i++) { put_user(p, at, &argv_va[i], 8); at += 8; }
    v = 0; put_user(p, at, &v, 8); at += 8;
    for (int i = 0; i < envc; i++) { put_user(p, at, &argv_va[argc + 1 + i], 8); at += 8; }
    v = 0; put_user(p, at, &v, 8); at += 8;
    put_user(p, at, aux, sizeof(aux));
    free(argv_va);
    *entry_out = start;
    *sp_out = sp;
    return 0;
}

/* ------------------------------------------------------------------ syscalls */

/* the shared code's 32-bit error values (0xFFFFF001..) become 64-bit ones */
static uint64_t sx(uint64_t r)
{
    if (r >= 0xFFFFF001ull && r <= 0xFFFFFFFFull) return (uint64_t)(int64_t)(int32_t)(uint32_t)r;
    return r;
}

static uint64_t call32(uint32_t nr, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e,
                       uint64_t f, registers_t *r)
{
    return sx(lx_dispatch32(nr, a, b, c, d, e, f, r));
}

/* x86-64 struct stat from the shared i386 stat64 */
static uint64_t put_stat(uint64_t dst, const linux_stat64_t *s)
{
    struct {
        uint64_t dev, ino, nlink;
        uint32_t mode, uid, gid, pad0;
        uint64_t rdev;
        int64_t size, blksize, blocks;
        uint64_t atime, atime_ns, mtime, mtime_ns, ctime, ctime_ns;
        int64_t unused[3];
    } st;
    memset(&st, 0, sizeof(st));
    st.dev = s->st_dev;
    st.ino = s->st_ino;
    st.nlink = s->st_nlink;
    st.mode = s->st_mode;
    st.uid = s->st_uid;
    st.gid = s->st_gid;
    st.rdev = s->st_rdev;
    st.size = s->st_size;
    st.blksize = s->st_blksize;
    st.blocks = s->st_blocks;
    st.atime = (uint32_t)s->atime_sec; st.atime_ns = s->atime_nsec;
    st.mtime = (uint32_t)s->mtime_sec; st.mtime_ns = s->mtime_nsec;
    st.ctime = (uint32_t)s->ctime_sec; st.ctime_ns = s->ctime_nsec;
    if (!lx_user_wr_ok(dst, sizeof(st))) return (uint64_t)-EFAULT;
    memcpy((void *)(uintptr_t)dst, &st, sizeof(st));
    return 0;
}

static uint64_t sys_stat_at(int dirfd, uint64_t path, uint64_t buf, uint32_t flags)
{
    linux_stat64_t s;
    int r = lx_stat_at(dirfd, path, flags, &s);
    if (r < 0) return (uint64_t)(int64_t)r;
    return put_stat(buf, &s);
}

/* readv/writev with 16-byte iovecs: one 32-bit read/write per piece */
static uint64_t sys_rwv(int write, int fd, uint64_t iov, uint64_t cnt, registers_t *r)
{
    if (cnt > 1024 || !lx_user_range_ok(iov, cnt * 16)) return (uint64_t)-EFAULT;
    const uint64_t *v = (const uint64_t *)(uintptr_t)iov;
    uint64_t total = 0;
    for (uint64_t i = 0; i < cnt; i++) {
        if (!v[2 * i + 1]) continue;
        uint64_t n = call32(write ? 4 : 3, (uint64_t)fd, v[2 * i], v[2 * i + 1], 0, 0, 0, r);
        if ((int64_t)n < 0) return total ? total : n;
        total += n;
        if (n < v[2 * i + 1]) break;
    }
    return total;
}

/* sendmsg/recvmsg: the x86-64 msghdr, done as sendto/recvfrom of each piece */
static uint64_t sys_msg(int send, int fd, uint64_t msg, uint32_t flags, registers_t *r)
{
    if (!lx_user_range_ok(msg, 56)) return (uint64_t)-EFAULT;
    uint64_t *m = (uint64_t *)(uintptr_t)msg;           /* name, namelen, iov, iovlen, control, controllen, flags */
    uint64_t name = m[0], namelen = (uint32_t)m[1], iov = m[2], iovlen = m[3];
    if (iovlen > 64 || !lx_user_range_ok(iov, iovlen * 16)) return (uint64_t)-EFAULT;
    const uint64_t *v = (const uint64_t *)(uintptr_t)iov;
    uint64_t total = 0;
    uint64_t control = m[4], controllen = m[5];
    if (send && control && controllen >= 16 && lx_user_range_ok(control, controllen)) {      /* SCM_RIGHTS: descriptors travel with the data */
        const uint8_t *c = (const uint8_t *)(uintptr_t)control;
        uint64_t off = 0;
        while (off + 16 <= controllen) {
            uint64_t clen;
            int32_t level, type;
            memcpy(&clen, c + off, 8); memcpy(&level, c + off + 8, 4); memcpy(&type, c + off + 12, 4);
            if (clen < 16 || off + clen > controllen) break;
            if (level == 1 && type == 1) {
                int fds[8], n = (int)((clen - 16) / 4);
                if (n > 8) n = 8;
                memcpy(fds, c + off + 16, (size_t)n * 4);
                int rc = scm_send_fds(fd, fds, n);
                if (rc < 0) return (uint64_t)(int64_t)rc;
            }
            off += (clen + 7) & ~7ull;
        }
    }
    for (uint64_t i = 0; i < iovlen; i++) {
        if (!v[2 * i + 1]) continue;
        uint64_t n;
        if (send) n = call32(369, (uint64_t)fd, v[2 * i], v[2 * i + 1], flags, i ? 0 : name, i ? 0 : namelen, r);
        else n = call32(371, (uint64_t)fd, v[2 * i], v[2 * i + 1], flags | (i ? 0x40u : 0), i ? 0 : name,
                        i ? 0 : (name ? msg + 8 : 0), r);
        if ((int64_t)n < 0) { if (total) break; return n; }
        total += n;
        if (n < v[2 * i + 1]) break;
    }
    if (!send) {
        int got = 0, fds[8];
        if (control && controllen >= 20 && lx_user_range_ok(control, controllen)) {
            int maxfds = (int)((controllen - 16) / 4);
            if (maxfds > 8) maxfds = 8;
            got = scm_recv_fds(fd, fds, maxfds);
            if (got > 0) {
                uint8_t *c = (uint8_t *)(uintptr_t)control;
                uint64_t clen = 16 + (uint64_t)got * 4;
                int32_t level = 1, type = 1;
                memcpy(c, &clen, 8); memcpy(c + 8, &level, 4); memcpy(c + 12, &type, 4);
                memcpy(c + 16, fds, (size_t)got * 4);
                m[5] = (clen + 7) & ~7ull;
            }
        }
        if (got <= 0) m[5] = 0;
        ((uint32_t *)m)[12] = 0;                                              /* no flags */
    }
    return total;
}

static uint64_t sys_clock_gettime(uint32_t clk, uint64_t ts)
{
    return call32(403, clk, ts, 0, 0, 0, 0, 0);                            /* clock_gettime64 */
}

static uint64_t sys_gettimeofday(uint64_t tv)
{
    if (!tv) return 0;
    int64_t ts[2];
    if (!lx_user_wr_ok(tv, 16)) return (uint64_t)-EFAULT;
    uint64_t r = call32(403, 0, (uint64_t)(uintptr_t)tv, 0, 0, 0, 0, 0);
    if (r) return r;
    memcpy(ts, (void *)(uintptr_t)tv, 16);
    ts[1] /= 1000;
    memcpy((void *)(uintptr_t)tv, ts, 16);
    return 0;
}

static uint64_t sys_sysinfo(uint64_t dst)
{
    struct {
        int64_t uptime;
        uint64_t loads[3], totalram, freeram, sharedram, bufferram, totalswap, freeswap;
        uint16_t procs, pad;
        uint32_t pad2;
        uint64_t totalhigh, freehigh;
        uint32_t mem_unit;
        char rest[4];
    } si;
    memset(&si, 0, sizeof(si));
    si.uptime = (int64_t)(debugmon_uptime_ms64() / 1000);
    si.totalram = (uint64_t)get_total_pages() * PAGE_SIZE;
    si.freeram = (uint64_t)(get_total_pages() - get_used_pages()) * PAGE_SIZE;
    si.procs = 1;
    si.mem_unit = 1;
    if (!lx_user_wr_ok(dst, sizeof(si))) return (uint64_t)-EFAULT;
    memcpy((void *)(uintptr_t)dst, &si, sizeof(si));
    return 0;
}

static uint64_t sys_arch_prctl(uint32_t code, uint64_t addr)
{
    task_t *t = task_current();
    switch (code) {
        case 0x1002:                                                       /* ARCH_SET_FS */
            if (addr >= PAGING_USER_END) return (uint64_t)-EPERM;
            t->fs_base = addr;
            wrmsr(MSR_FS, addr);
            return 0;
        case 0x1003:                                                       /* ARCH_GET_FS */
            if (!lx_user_wr_ok(addr, 8)) return (uint64_t)-EFAULT;
            *(uint64_t *)(uintptr_t)addr = t->fs_base;
            return 0;
        case 0x1001:                                                       /* ARCH_SET_GS */
            return (uint64_t)-EINVAL;
        default:
            return (uint64_t)-EINVAL;
    }
}

/* ------------------------------------------------------------------ signals */

#define SA_SIGINFO   0x4u
#define SA_ONSTACK   0x08000000u
#define SA_RESTORER  0x04000000u
#define SA_NODEFER   0x40000000u
#define SA_RESETHAND 0x80000000u
#define SIGBIT(n) (1ull << ((n) - 1))

static uint64_t sys_rt_sigaction(int sig, uint64_t act, uint64_t oact, uint64_t size)
{
    proc_t *p = proc_current();
    if (!p || sig < 1 || sig > 64 || size != 8) return (uint64_t)-EINVAL;
    if (act && (sig == 9 || sig == 19)) return (uint64_t)-EINVAL;
    uint64_t in[4];
    if (act) {
        if (!lx_user_range_ok(act, 32)) return (uint64_t)-EFAULT;
        memcpy(in, (const void *)(uintptr_t)act, 32);           /* handler, flags, restorer, mask */
    }
    if (oact) {
        if (!lx_user_wr_ok(oact, 32)) return (uint64_t)-EFAULT;
        uint64_t out[4] = { p->sig_handler[sig], p->sig_flags[sig], p->sig_restorer[sig], p->sig_mask[sig] };
        memcpy((void *)(uintptr_t)oact, out, 32);
    }
    if (act) {
        p->sig_handler[sig] = in[0];
        p->sig_flags[sig] = in[1];
        p->sig_restorer[sig] = in[2];
        p->sig_mask[sig] = in[3] & ~(SIGBIT(9) | SIGBIT(19));
        if (in[0] <= 1) p->sig_pending &= ~SIGBIT(sig);         /* default or ignored: nothing to run */
    }
    return 0;
}

static uint64_t sys_rt_sigprocmask(int how, uint64_t set, uint64_t oset, uint64_t size)
{
    task_t *t = task_current();
    if (size != 8 || !t) return (uint64_t)-EINVAL;
    uint64_t in = 0;
    if (set) {
        if (!lx_user_range_ok(set, 8)) return (uint64_t)-EFAULT;
        memcpy(&in, (const void *)(uintptr_t)set, 8);
    }
    if (oset) {
        if (!lx_user_wr_ok(oset, 8)) return (uint64_t)-EFAULT;
        memcpy((void *)(uintptr_t)oset, &t->sig_blocked, 8);
    }
    if (set) {
        if (how == 0) t->sig_blocked |= in;                     /* SIG_BLOCK */
        else if (how == 1) t->sig_blocked &= ~in;               /* SIG_UNBLOCK */
        else if (how == 2) t->sig_blocked = in;                 /* SIG_SETMASK */
        else return (uint64_t)-EINVAL;
        t->sig_blocked &= ~(SIGBIT(9) | SIGBIT(19));
    }
    return 0;
}

/* the FPU/SSE state the program had at the system call: syscall_entry64 saved it below the frame */
static uint8_t *saved_fpu(registers_t *r) { return (uint8_t *)r - 520; }

/* the kernel's struct sigcontext (glibc's mcontext_t has the same layout) */
typedef struct {
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15, rdi, rsi, rbp, rbx, rdx, rax, rcx, rsp, rip, eflags;
    uint16_t cs, gs, fs, ss;
    uint64_t err, trapno, oldmask, cr2, fpstate, reserved[8];
} sigcontext64_t;

typedef struct {
    uint64_t flags, link;
    uint64_t ss_sp; int32_t ss_flags, pad; uint64_t ss_size;
    sigcontext64_t mc;
    uint64_t sigmask;
} ucontext64_t;

/* runs the handler of a signal that is waiting, when returning to the program: the frame Linux
 * builds (rt_sigframe: the restorer's address, ucontext, siginfo, FPU state) on its stack */
static void deliver_signal(registers_t *r)
{
    proc_t *p = proc_current();
    task_t *t = task_current();
    if (!p || !t || p->linux_abi != 2) return;
    uint64_t ready = p->sig_pending & ~t->sig_blocked;
    if (!ready) return;
    int sig = 1;
    while (!(ready & SIGBIT(sig))) sig++;
    p->sig_pending &= ~SIGBIT(sig);
    uint64_t handler = p->sig_handler[sig];
    if (handler <= 1) return;                                   /* changed meanwhile */

    uint64_t sp = (r->rsp - 128) & ~63ull;                      /* past the red zone */
    sp -= 512;
    uint64_t fp = sp;                                           /* fxsave image, 64-byte aligned */
    uint64_t frame = ((fp - (8 + sizeof(ucontext64_t) + 128)) & ~15ull) - 8;
    uint64_t uc_va = frame + 8, si_va = uc_va + sizeof(ucontext64_t);

    ucontext64_t uc;
    memset(&uc, 0, sizeof(uc));
    sigcontext64_t *m = &uc.mc;
    m->r8 = r->r8; m->r9 = r->r9; m->r10 = r->r10; m->r11 = r->r11;
    m->r12 = r->r12; m->r13 = r->r13; m->r14 = r->r14; m->r15 = r->r15;
    m->rdi = r->rdi; m->rsi = r->rsi; m->rbp = r->rbp; m->rbx = r->rbx;
    m->rdx = r->rdx; m->rax = r->rax; m->rcx = r->rcx; m->rsp = r->rsp;
    m->rip = r->rip; m->eflags = r->rflags;
    m->cs = 0x2B; m->ss = 0x23;
    m->fpstate = fp;
    uc.sigmask = t->sig_blocked;
    uint8_t info[128];
    memset(info, 0, sizeof(info));
    int32_t code = sig == 17 ? 1 : 0;                           /* CLD_EXITED, SI_USER */
    memcpy(info, &sig, 4);
    memcpy(info + 8, &code, 4);
    uint64_t ret = p->sig_restorer[sig];
    if (put_user(p, fp, saved_fpu(r), 512) || put_user(p, si_va, info, sizeof(info)) ||
        put_user(p, uc_va, &uc, sizeof(uc)) || put_user(p, frame, &ret, 8)) {
        p->term_sig = 11;                                       /* no stack for it: as Linux, SIGSEGV */
        proc_exit(128 + 11);
    }
    t->sig_blocked |= p->sig_mask[sig];
    if (!(p->sig_flags[sig] & SA_NODEFER)) t->sig_blocked |= SIGBIT(sig);
    if (p->sig_flags[sig] & SA_RESETHAND) { p->sig_handler[sig] = 0; p->sig_flags[sig] = 0; }
    t->sig_blocked &= ~(SIGBIT(9) | SIGBIT(19));
    r->rsp = frame;
    r->rip = handler;
    r->rdi = (uint64_t)sig;
    r->rsi = si_va;
    r->rdx = uc_va;
    r->rax = 0;
    r->rflags &= ~(0x400ull | 0x100ull | 0x40000ull);           /* DF, TF, AC */
}

/* back from a handler: the registers, FPU state and mask the frame holds */
static uint64_t sys_rt_sigreturn(registers_t *r)
{
    proc_t *p = proc_current();
    task_t *t = task_current();
    uint64_t uc_va = r->rsp;                                    /* the handler's ret took the restorer */
    ucontext64_t uc;
    if (!lx_user_range_ok(uc_va, sizeof(uc))) { p->term_sig = 11; proc_exit(128 + 11); }
    memcpy(&uc, (const void *)(uintptr_t)uc_va, sizeof(uc));
    sigcontext64_t *m = &uc.mc;
    if (m->rip >= PAGING_USER_END || m->rsp >= PAGING_USER_END) { p->term_sig = 11; proc_exit(128 + 11); }
    r->r8 = m->r8; r->r9 = m->r9; r->r10 = m->r10; r->r11 = m->r11;
    r->r12 = m->r12; r->r13 = m->r13; r->r14 = m->r14; r->r15 = m->r15;
    r->rdi = m->rdi; r->rsi = m->rsi; r->rbp = m->rbp; r->rbx = m->rbx;
    r->rdx = m->rdx; r->rcx = m->rcx; r->rsp = m->rsp; r->rip = m->rip;
    /* the flags a program may change (CF PF AF ZF SF TF DF OF AC), interrupts stay on */
    r->rflags = (r->rflags & ~0x40DD5ull) | (m->eflags & 0x40DD5ull) | 0x200ull;
    if (m->fpstate && lx_user_range_ok(m->fpstate, 512)) {
        uint8_t *dst = saved_fpu(r);
        memcpy(dst, (const void *)(uintptr_t)m->fpstate, 512);
        uint32_t mxcsr;
        memcpy(&mxcsr, dst + 24, 4);
        mxcsr &= 0xFFFFu;                                       /* reserved bits would fault fxrstor */
        memcpy(dst + 24, &mxcsr, 4);
    }
    t->sig_blocked = uc.sigmask & ~(SIGBIT(9) | SIGBIT(19));
    return m->rax;
}

/* tkill/tgkill: a handler if the program has one, else what the shared code does (abort...) */
static uint64_t sys_tgkill(int sig, uint32_t nr, uint64_t a, uint64_t b, uint64_t c, registers_t *r)
{
    proc_t *p = proc_current();
    if (p && sig > 0 && sig <= 64 && p->sig_handler[sig] > 1) {
        p->sig_pending |= SIGBIT(sig);
        return 0;
    }
    return call32(nr, a, b, c, 0, 0, 0, r);
}

int linux64_trace;

void linux64_syscall(registers_t *r)
{
    uint64_t nr = r->rax, a = r->rdi, b = r->rsi, c = r->rdx, d = r->r10, e = r->r8, f = r->r9;
    uint64_t ret;
    switch (nr) {
        case 0:   ret = call32(3, a, b, c, 0, 0, 0, r); break;                 /* read */
        case 1:   ret = call32(4, a, b, c, 0, 0, 0, r); break;                 /* write */
        case 2:   ret = call32(295, (uint64_t)-100, a, b, c, 0, 0, r); break;   /* open */
        case 3:   ret = call32(6, a, 0, 0, 0, 0, 0, r); break;                 /* close */
        case 4:   ret = sys_stat_at(-100, a, b, 0); break;                     /* stat */
        case 5: {                                                              /* fstat */
            linux_stat64_t st;
            int rr = lx_fd_stat((int)a, &st);
            ret = rr < 0 ? (uint64_t)(int64_t)rr : put_stat(b, &st);
            break;
        }
        case 6:   ret = sys_stat_at(-100, a, b, 0x100); break;                 /* lstat */
        case 7:   ret = call32(168, a, b, c, 0, 0, 0, r); break;               /* poll */
        case 8:   ret = call32(19, a, b, c, 0, 0, 0, r); break;                /* lseek */
        case 9:   ret = sys_mmap(a, b, (uint32_t)c, (uint32_t)d, (int)e, f); break;
        case 10:  ret = sys_mprotect(a, b, (uint32_t)c); break;
        case 11:  ret = sys_munmap(a, b); break;
        case 12:  ret = sys_brk(a); break;
        case 13:  ret = sys_rt_sigaction((int)a, b, c, d); break;
        case 14:  ret = sys_rt_sigprocmask((int)a, b, c, d); break;
        case 15:  ret = sys_rt_sigreturn(r); break;
        case 16:  ret = call32(54, a, b, c, 0, 0, 0, r); break;                /* ioctl */
        case 17:  ret = call32(180, a, b, c, d & 0xFFFFFFFFu, d >> 32, 0, r); break;  /* pread64 */
        case 18:  ret = call32(181, a, b, c, d & 0xFFFFFFFFu, d >> 32, 0, r); break;  /* pwrite64 */
        case 19:  ret = sys_rwv(0, (int)a, b, c, r); break;                    /* readv */
        case 20:  ret = sys_rwv(1, (int)a, b, c, r); break;                    /* writev */
        case 21:  ret = call32(33, a, b, 0, 0, 0, 0, r); break;                /* access */
        case 22:  ret = call32(42, a, 0, 0, 0, 0, 0, r); break;                /* pipe */
        case 24:  ret = 0; task_yield(); break;                                /* sched_yield */
        case 25:  ret = sys_mremap(a, b, c, (uint32_t)d); break;
        case 28:  ret = 0; break;                                              /* madvise */
        case 32:  ret = call32(41, a, 0, 0, 0, 0, 0, r); break;                /* dup */
        case 33:  ret = call32(63, a, b, 0, 0, 0, 0, r); break;                /* dup2 */
        case 35:  ret = call32(407, 0, 0, a, b, 0, 0, r); break;               /* nanosleep: clock_nanosleep_time64 */
        case 39:  ret = call32(20, 0, 0, 0, 0, 0, 0, r); break;                /* getpid */
        case 41:  ret = call32(359, a, b, c, 0, 0, 0, r); break;               /* socket */
        case 42:  ret = call32(362, a, b, c, 0, 0, 0, r); break;               /* connect */
        case 44:  ret = call32(369, a, b, c, d, e, f, r); break;               /* sendto */
        case 45:  ret = call32(371, a, b, c, d, e, f, r); break;               /* recvfrom */
        case 46:  ret = sys_msg(1, (int)a, b, (uint32_t)c, r); break;          /* sendmsg */
        case 47:  ret = sys_msg(0, (int)a, b, (uint32_t)c, r); break;          /* recvmsg */
        case 48:  ret = call32(373, a, b, 0, 0, 0, 0, r); break;               /* shutdown */
        case 49:  ret = call32(361, a, b, c, 0, 0, 0, r); break;               /* bind */
        case 50:  ret = call32(363, a, b, 0, 0, 0, 0, r); break;               /* listen */
        case 43:  ret = call32(364, a, b, c, 0, 0, 0, r); break;               /* accept */
        case 288: ret = call32(364, a, b, c, d, 0, 0, r); break;               /* accept4 */
        case 51:  ret = call32(367, a, b, c, 0, 0, 0, r); break;               /* getsockname */
        case 52:  ret = call32(368, a, b, c, 0, 0, 0, r); break;               /* getpeername */
        case 53:  ret = call32(360, a, b, c, d, 0, 0, r); break;               /* socketpair */
        case 36: case 37: case 38: ret = 0; break;                             /* getitimer, alarm, setitimer: no timers yet (git's progress asks) */
        case 34: {                                                             /* pause */
            proc_t *pp = proc_current();
            while (!(pp->sig_pending & ~task_current()->sig_blocked)) task_sleep(20);
            ret = (uint64_t)-EINTR;
            break;
        }
        case 54:  ret = call32(366, a, b, c, d, e, 0, r); break;               /* setsockopt */
        case 55:  ret = call32(365, a, b, c, d, e, 0, r); break;               /* getsockopt */
        case 56:  ret = sx(lx_clone(r, (uint32_t)a, b, c, e, d)); break;       /* clone: flags, stack, ptid, ctid, tls */
        case 57:  ret = call32(2, 0, 0, 0, 0, 0, 0, r); break;                 /* fork */
        case 58:  ret = call32(190, 0, 0, 0, 0, 0, 0, r); break;               /* vfork */
        case 59:  ret = (uint64_t)(int64_t)lx_execve(a, b, c); break;          /* execve: only on failure */
        case 61:  ret = sx(lx_wait4((int)a, b, (uint32_t)c, d)); break;        /* wait4 */
        case 60:  ret = call32(1, a, 0, 0, 0, 0, 0, r); break;                 /* exit */
        case 62:  ret = call32(37, a, b, 0, 0, 0, 0, r); break;                /* kill */
        case 63:  ret = call32(122, a, 0, 0, 0, 0, 0, r); break;               /* uname */
        case 72:  ret = call32(221, a, b, c, 0, 0, 0, r); break;               /* fcntl */
        case 74:  case 75: ret = 0; break;                                     /* fsync, fdatasync */
        case 77:  ret = call32(93, a, b, 0, 0, 0, 0, r); break;                /* ftruncate */
        case 76:  ret = call32(92, a, b, 0, 0, 0, 0, r); break;                /* truncate */
        case 79:  ret = call32(183, a, b, 0, 0, 0, 0, r); break;               /* getcwd */
        case 80:  ret = call32(12, a, 0, 0, 0, 0, 0, r); break;                /* chdir */
        case 82:  ret = call32(38, a, b, 0, 0, 0, 0, r); break;                /* rename */
        case 83:  ret = call32(39, a, b, 0, 0, 0, 0, r); break;                /* mkdir */
        case 84:  ret = call32(40, a, 0, 0, 0, 0, 0, r); break;                /* rmdir */
        case 86:  ret = call32(9, a, b, 0, 0, 0, 0, r); break;                 /* link */
        case 87:  ret = call32(10, a, 0, 0, 0, 0, 0, r); break;                /* unlink */
        case 88:  ret = call32(83, a, b, 0, 0, 0, 0, r); break;                /* symlink */
        case 89:  ret = call32(85, a, b, c, 0, 0, 0, r); break;                /* readlink */
        case 90:  ret = call32(15, a, b, 0, 0, 0, 0, r); break;                /* chmod */
        case 91:  case 92: case 93: case 94: ret = 0; break;                   /* fchmod, chown, fchown, lchown */
        case 95:  ret = call32(60, a, 0, 0, 0, 0, 0, r); break;                /* umask */
        case 96:  ret = sys_gettimeofday(a); break;
        case 97:  ret = call32(340, 0, a, 0, b, 0, 0, r); break;               /* getrlimit: prlimit64 */
        case 99:  ret = sys_sysinfo(a); break;
        case 102: ret = call32(199, 0, 0, 0, 0, 0, 0, r); break;               /* getuid */
        case 104: ret = call32(200, 0, 0, 0, 0, 0, 0, r); break;               /* getgid */
        case 107: ret = call32(201, 0, 0, 0, 0, 0, 0, r); break;               /* geteuid */
        case 108: ret = call32(202, 0, 0, 0, 0, 0, 0, r); break;               /* getegid */
        case 110: ret = call32(64, 0, 0, 0, 0, 0, 0, r); break;                /* getppid */
        case 111: case 112: ret = call32(20, 0, 0, 0, 0, 0, 0, r); break;      /* getpgrp, setsid */
        case 109: ret = 0; break;                                              /* setpgid */
        case 121: case 124: ret = call32(20, 0, 0, 0, 0, 0, 0, r); break;      /* getpgid, getsid: one group per program */
        case 115: ret = 0; break;                                              /* getgroups: none */
        case 1000: {                                                           /* tOS: is this the account's password? (root only: sshd's monitor) */
            uint32_t u, g;
            task_get_cred(&u, &g);
            char user[AUTH_NAME_MAX], pw[128];
            int ok = u == 0;
            for (int k = 0; ok && k < 2; k++) {
                uint64_t ptr = k == 0 ? a : b;
                char *dst = k == 0 ? user : pw;
                int cap = k == 0 ? (int)sizeof(user) : (int)sizeof(pw), i = 0;
                for (; i < cap; i++) {
                    if (!lx_user_range_ok(ptr + (uint64_t)i, 1)) { ok = 0; break; }
                    dst[i] = ((const char *)(uintptr_t)ptr)[i];
                    if (!dst[i]) break;
                }
                if (i == cap) ok = 0;
            }
            ret = !ok ? (uint64_t)-EPERM : auth_check_password(user, pw) == 0 ? 0 : (uint64_t)-EACCES;
            memset(pw, 0, sizeof(pw));
            break;
        }
        case 105: case 106: case 113: case 114: case 117: case 119: {          /* setuid, setgid, setreuid, setregid, setresuid, setresgid */
            uint32_t uid, gid;
            task_get_cred(&uid, &gid);
            int is_gid = nr == 106 || nr == 114 || nr == 119;
            uint64_t want = (nr == 113 || nr == 114) ? b : (nr == 117 || nr == 119) ? b : a;   /* the effective one */
            if (want == (uint64_t)-1) want = a;
            if (want == (uint64_t)-1 || want > 0xFFFFFFFEull) { ret = 0; break; }
            uint32_t cur = is_gid ? gid : uid;
            if (uid != 0 && (uint32_t)want != cur) { ret = (uint64_t)-EPERM; break; }   /* only root becomes someone else */
            if (is_gid) task_set_cred(uid, (uint32_t)want); else task_set_cred((uint32_t)want, gid);
            ret = 0;
            break;
        }
        case 116: { uint32_t u, g; task_get_cred(&u, &g); ret = u == 0 ? 0 : (uint64_t)-EPERM; break; }   /* setgroups */
        case 118: case 120: {                                                  /* getresuid, getresgid */
            uint32_t u, g;
            task_get_cred(&u, &g);
            uint32_t v = nr == 118 ? u : g;
            ret = 0;
            for (int k = 0; k < 3; k++) {
                uint64_t ptr = k == 0 ? a : k == 1 ? b : c;
                if (!lx_user_range_ok(ptr, 4)) { ret = (uint64_t)-EFAULT; break; }
                memcpy((void *)(uintptr_t)ptr, &v, 4);
            }
            break;
        }
        case 122: case 123: { uint32_t u, g; task_get_cred(&u, &g); ret = nr == 122 ? u : g; break; }   /* setfsuid, setfsgid: the old value */
        case 161: { uint32_t u, g; task_get_cred(&u, &g); ret = u == 0 ? 0 : (uint64_t)-EPERM; break; }  /* chroot: tOS has one root */
        case 125: ret = (uint64_t)-EINVAL; break;                              /* capget */
        case 131: ret = call32(186, a, b, 0, 0, 0, 0, r); break;               /* sigaltstack */
        case 137: case 138: ret = (uint64_t)-ENOSYS; break;                    /* statfs, fstatfs */
        case 143: case 145: ret = 0; break;                                    /* sched_getparam, sched_getscheduler */
        case 157: ret = call32(172, a, b, c, d, e, 0, r); break;               /* prctl */
        case 158: ret = sys_arch_prctl((uint32_t)a, b); break;
        case 160: ret = 0; break;                                              /* setrlimit */
        case 186: ret = call32(224, 0, 0, 0, 0, 0, 0, r); break;               /* gettid */
        case 200: ret = sx(sys_tgkill((int)b, 238, a, b, 0, r)); break;        /* tkill */
        case 201: {                                                            /* time */
            int64_t sec = 0;
            int32_t nsec = 0;
            lx_clock_now(0, &sec, &nsec);
            uint64_t k = (uint64_t)sec;
            if (a && lx_user_wr_ok(a, 8)) *(uint64_t *)(uintptr_t)a = k;
            ret = k;
            break;
        }
        case 202: ret = call32(422, a, b, c, d, e, f, r); break;               /* futex (64-bit timespec) */
        case 203: ret = 0; break;                                              /* sched_setaffinity */
        case 204: ret = call32(242, a, b, c, 0, 0, 0, r); break;               /* sched_getaffinity */
        case 217: ret = call32(220, a, b, c, 0, 0, 0, r); break;               /* getdents64 */
        case 218: ret = call32(258, a, 0, 0, 0, 0, 0, r); break;               /* set_tid_address */
        case 228: ret = sys_clock_gettime((uint32_t)a, b); break;
        case 229: ret = call32(406, a, b, 0, 0, 0, 0, r); break;               /* clock_getres (64-bit) */
        case 230: ret = call32(407, a, b, c, d, 0, 0, r); break;               /* clock_nanosleep */
        case 231: ret = call32(252, a, 0, 0, 0, 0, 0, r); break;               /* exit_group */
        case 232: ret = call32(256, a, b, c, d, 0, 0, r); break;               /* epoll_wait */
        case 233: ret = call32(255, a, b, c, d, 0, 0, r); break;               /* epoll_ctl */
        case 234: ret = sx(sys_tgkill((int)c, 270, a, b, c, r)); break;        /* tgkill */
        case 257: ret = call32(295, a, b, c, d, 0, 0, r); break;               /* openat */
        case 258: ret = call32(39, b, c, 0, 0, 0, 0, r); break;                /* mkdirat (AT_FDCWD paths) */
        case 262: ret = sys_stat_at((int)a, b, c, (uint32_t)d); break;         /* newfstatat */
        case 263: ret = call32((d & 0x200) ? 40 : 10, b, 0, 0, 0, 0, 0, r); break;  /* unlinkat */
        case 264: case 316: ret = call32(38, b, d, 0, 0, 0, 0, r); break;      /* renameat, renameat2 */
        case 267: ret = call32(305, a, b, c, d, 0, 0, r); break;               /* readlinkat */
        case 269: case 439: ret = call32(33, b, c, 0, 0, 0, 0, r); break;      /* faccessat, faccessat2 */
        case 23: case 270: {                                                   /* select, pselect6 */
            int64_t ms = -1;
            if (e) {
                int64_t tv[2];
                if (!lx_user_range_ok(e, 16)) { ret = (uint64_t)-EFAULT; break; }
                memcpy(tv, (const void *)(uintptr_t)e, 16);
                if (tv[0] < 0 || tv[1] < 0) { ret = (uint64_t)-EINVAL; break; }
                int64_t frac = nr == 23 ? (tv[1] + 999) / 1000 : (tv[1] + 999999) / 1000000;   /* microseconds / nanoseconds -> ms */
                ms = tv[0] > 2000000 ? 2000000000 : tv[0] * 1000 + frac;
            }
            ret = sx(call32(142, a, b, c, d, (uint64_t)(int64_t)(ms > 2000000000 ? 2000000000 : ms), 0, r));
            break;
        }
        case 271: {                                                            /* ppoll */
            int64_t ms = -1;
            if (c) {
                int64_t ts[2];
                if (!lx_user_range_ok(c, 16)) { ret = (uint64_t)-EFAULT; break; }
                memcpy(ts, (const void *)(uintptr_t)c, 16);
                if (ts[0] < 0 || ts[1] < 0) { ret = (uint64_t)-EINVAL; break; }
                ms = ts[0] > 2000000 ? 2000000000 : ts[0] * 1000 + (ts[1] + 999999) / 1000000;
            }
            ret = call32(168, a, b, (uint64_t)(int64_t)(ms > 2000000000 ? 2000000000 : ms), 0, 0, 0, r);
            break;
        }
        case 273: ret = 0; break;                                              /* set_robust_list */
        case 280: ret = call32(412, a, b, c, d, 0, 0, r); break;               /* utimensat */
        case 281: ret = call32(319, a, b, c, d, e, f, r); break;               /* epoll_pwait */
        case 290: ret = call32(328, a, b, 0, 0, 0, 0, r); break;               /* eventfd2 */
        case 291: ret = call32(329, a, 0, 0, 0, 0, 0, r); break;               /* epoll_create1 */
        case 292: ret = call32(330, a, b, c, 0, 0, 0, r); break;               /* dup3 */
        case 293: ret = call32(331, a, b, 0, 0, 0, 0, r); break;               /* pipe2 */
        case 295: case 296: {                                                  /* preadv, pwritev */
            if (c > 1024 || !lx_user_range_ok(b, c * 16)) { ret = (uint64_t)-EFAULT; break; }
            const uint64_t *v = (const uint64_t *)(uintptr_t)b;
            uint64_t total = 0, off = d;
            ret = 0;
            for (uint64_t i = 0; i < c; i++) {
                if (!v[2 * i + 1]) continue;
                uint64_t n = call32(nr == 295 ? 180 : 181, a, v[2 * i], v[2 * i + 1], off & 0xFFFFFFFFu, off >> 32, 0, r);
                if ((int64_t)n < 0) { ret = total ? total : n; goto done; }
                total += n; off += n;
                if (n < v[2 * i + 1]) break;
            }
            ret = total;
            break;
        }
        case 302: ret = call32(340, a, b, c, d, 0, 0, r); break;               /* prlimit64 */
        case 318: ret = call32(355, a, b, c, 0, 0, 0, r); break;               /* getrandom */
        case 334: ret = (uint64_t)-ENOSYS; break;                              /* rseq */
        case 332: ret = (uint64_t)-ENOSYS; break;                              /* statx: glibc and libuv fall back */
        case 425: case 426: case 427: ret = (uint64_t)-ENOSYS; break;          /* io_uring */
        case 435: ret = (uint64_t)-ENOSYS; break;                              /* clone3: glibc uses clone */
        case 434: ret = (uint64_t)-ENOSYS; break;                              /* pidfd_open: wait4 instead */
        default: {
            static uint8_t seen[512];
            if (nr < 512 && seen[nr]++ < 3) {
                char line[64] = "linux64: no syscall ", h[12];
                sprintf_u32(h, (uint32_t)nr);
                strcat(line, h);
                strcat(line, "\n");
                serial_write(line);
            }
            ret = (uint64_t)-ENOSYS;
        }
    }
done:
    if (linux64_trace) {
        char line[160], h[24];
        static const char hx[] = "0123456789abcdef";
        strcpy(line, "S64 ");
        sprintf_u32(h, (uint32_t)nr); strcat(line, h);
        uint64_t vals[4] = { a, b, c, ret };
        for (int k = 0; k < 4; k++) {
            strcat(line, k == 3 ? " = " : " ");
            int o = 0; h[o++] = '0'; h[o++] = 'x';
            for (int sh = 60; sh >= 0; sh -= 4) if ((vals[k] >> sh) || sh == 0) { for (int t = sh; t >= 0; t -= 4) h[o++] = hx[(vals[k] >> t) & 15]; break; }
            h[o] = 0;
            strcat(line, h);
        }
        strcat(line, "\n");
        serial_write(line);
    }
    r->rax = ret;
    deliver_signal(r);
}
