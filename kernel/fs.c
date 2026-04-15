/*
 * fs.c — flat in-memory filesystem.
 *
 * Holds the ramfs inode array and implements the file lookup API.
 * The actual file data for binary inodes comes from objcopy-embedded
 * blobs linked into the kernel; text inodes reference string literals
 * defined below.
 */

#include "fs.h"
#include "defs.h"

/* ── Embedded user binaries (linked by objcopy) ──────────────────── */
extern const unsigned char _binary_user_init_bin_start[];
extern const unsigned char _binary_user_init_bin_end[];
extern const unsigned char _binary_user_hello_bin_start[];
extern const unsigned char _binary_user_hello_bin_end[];
extern const unsigned char _binary_user_cpu_bound_bin_start[];
extern const unsigned char _binary_user_cpu_bound_bin_end[];
extern const unsigned char _binary_user_io_bound_bin_start[];
extern const unsigned char _binary_user_io_bound_bin_end[];

/* ── Text files ──────────────────────────────────────────────────── */
static const char readme_text[] =
    "os-from-scratch\n"
    "---------------\n"
    "A RISC-V kernel built from bare metal, ending in an online-learning\n"
    "process scheduler. Phase 2 adds an interactive shell, a flat ramfs,\n"
    "and an ANSI TUI.\n"
    "\n"
    "commands:\n"
    "  help           list commands\n"
    "  ls             list files in the ramfs\n"
    "  cat <file>     print a text file\n"
    "  ps             list processes\n"
    "  run <f> [n]    spawn n copies of program f\n"
    "  kill <pid>     terminate process\n"
    "  clear          clear shell output\n";

/* ── The ramfs ───────────────────────────────────────────────────── */
inode_t ramfs[] = {
    { "init",      _binary_user_init_bin_start,      0, FT_BINARY },
    { "hello",     _binary_user_hello_bin_start,     0, FT_BINARY },
    { "cpu_bound", _binary_user_cpu_bound_bin_start, 0, FT_BINARY },
    { "io_bound",  _binary_user_io_bound_bin_start,  0, FT_BINARY },
    { "README",    (const unsigned char *)readme_text,
      sizeof(readme_text) - 1, FT_TEXT },
};

const uint64_t NRAMFS = sizeof(ramfs) / sizeof(ramfs[0]);

/* ── Local string equality (no libc) ─────────────────────────────── */
static int str_eq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* ── Public API ──────────────────────────────────────────────────── */
void fs_init(void) {
    ramfs[0].size = (uint64_t)(_binary_user_init_bin_end      - _binary_user_init_bin_start);
    ramfs[1].size = (uint64_t)(_binary_user_hello_bin_end     - _binary_user_hello_bin_start);
    ramfs[2].size = (uint64_t)(_binary_user_cpu_bound_bin_end - _binary_user_cpu_bound_bin_start);
    ramfs[3].size = (uint64_t)(_binary_user_io_bound_bin_end  - _binary_user_io_bound_bin_start);
    /* ramfs[4] is README — size is already set via sizeof at declaration. */
}

const inode_t *fs_lookup(const char *name) {
    for (uint64_t i = 0; i < NRAMFS; i++) {
        if (str_eq(ramfs[i].name, name))
            return &ramfs[i];
    }
    return NULL;
}
