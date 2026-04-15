#ifndef FS_H
#define FS_H

#include "defs.h"

/*
 * fs.h — flat in-memory filesystem (ramfs).
 *
 * Each file is a statically-sized inode with a name, a pointer to the
 * backing bytes (either an embedded binary via objcopy, or a string
 * literal for text files), a byte count, and a type tag.
 *
 * The ramfs is read-only — no create/delete. Binary files come from
 * objcopy-embedded .bin blobs; text files come from string literals
 * defined in fs.c.
 *
 * P2.2 scope:
 *   - fs_init fills in sizes for binary inodes
 *   - fs_lookup(name) returns an inode_t* or NULL
 *   - shell commands ls + cat walk the table directly via exported
 *     ramfs[] + NRAMFS
 */

typedef enum {
    FT_BINARY = 0,   /* objcopy-embedded user program */
    FT_TEXT   = 1,   /* plain text (string literal) */
} file_type_t;

typedef struct {
    const char          *name;   /* NUL-terminated; max ~15 chars */
    const unsigned char *data;   /* pointer into kernel .rodata */
    uint64_t             size;   /* filled by fs_init for binaries */
    file_type_t          type;
} inode_t;

/* Exported ramfs table. The shell iterates this directly for `ls`. */
extern inode_t ramfs[];
extern const uint64_t NRAMFS;

/*
 * fs_init — fill in .size for every binary inode by computing
 * _end - _start of its objcopy symbols. Must be called from kmain
 * before any call to fs_lookup / proc_exec_static.
 */
void fs_init(void);

/*
 * fs_lookup(name) — linear-scan lookup by filename. Returns NULL if
 * no file matches.
 */
const inode_t *fs_lookup(const char *name);

#endif /* FS_H */
