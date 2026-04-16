// tree.c — Tree object serialization and construction
//
// PROVIDED functions: get_file_mode, tree_parse, tree_serialize
// TODO functions:     tree_from_index
//
// Binary tree format (per entry, concatenated with no separators):
//   "<mode-as-ascii-octal> <name>\0<32-byte-binary-hash>"
//
// Example single entry (conceptual):
//   "100644 hello.txt\0" followed by 32 raw bytes of SHA-256

#include "tree.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include "pes.h"

// Forward declaration
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);
#include "index.h"

// ─── Mode Constants ─────────────────────────────────────────────────────────

#define MODE_FILE      0100644
#define MODE_EXEC      0100755
#define MODE_DIR       0040000

// ─── PROVIDED ───────────────────────────────────────────────────────────────

// Determine the object mode for a filesystem path.
uint32_t get_file_mode(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;

    if (S_ISDIR(st.st_mode))  return MODE_DIR;
    if (st.st_mode & S_IXUSR) return MODE_EXEC;
    return MODE_FILE;
}

// Parse binary tree data into a Tree struct safely.
// Returns 0 on success, -1 on parse error.
int tree_parse(const void *data, size_t len, Tree *tree_out) {
    tree_out->count = 0;
    const uint8_t *ptr = (const uint8_t *)data;
    const uint8_t *end = ptr + len;

    while (ptr < end && tree_out->count < MAX_TREE_ENTRIES) {
        TreeEntry *entry = &tree_out->entries[tree_out->count];

        // 1. Safely find the space character for the mode
        const uint8_t *space = memchr(ptr, ' ', end - ptr);
        if (!space) return -1; // Malformed data

        // Parse mode into an isolated buffer
        char mode_str[16] = {0};
        size_t mode_len = space - ptr;
        if (mode_len >= sizeof(mode_str)) return -1;
        memcpy(mode_str, ptr, mode_len);
        entry->mode = strtol(mode_str, NULL, 8);

        ptr = space + 1; // Skip space

        // 2. Safely find the null terminator for the name
        const uint8_t *null_byte = memchr(ptr, '\0', end - ptr);
        if (!null_byte) return -1; // Malformed data

        size_t name_len = null_byte - ptr;
        if (name_len >= sizeof(entry->name)) return -1;
        memcpy(entry->name, ptr, name_len);
        entry->name[name_len] = '\0';

        ptr = null_byte + 1; // Skip null byte

        // 3. Read the 32-byte binary hash
        if (ptr + HASH_SIZE > end) return -1; 
        memcpy(entry->hash.hash, ptr, HASH_SIZE);
        ptr += HASH_SIZE;

        tree_out->count++;
    }
    return 0;
}

// Helper for qsort to ensure consistent tree hashing
static int compare_tree_entries(const void *a, const void *b) {
    return strcmp(((const TreeEntry *)a)->name, ((const TreeEntry *)b)->name);
}

// Helper for sorting tree pointers
static int compare_tree_ptrs(const void *a, const void *b) {
    return strcmp((*(const TreeEntry **)a)->name, (*(const TreeEntry **)b)->name);
}

// Serialize a Tree struct into binary format for storage.
int tree_serialize(const Tree *tree, void **data_out, size_t *len_out) {
    size_t max_size = tree->count * 600; 
    uint8_t *buffer = malloc(max_size);
    if (!buffer) return -1;

    // 1. Sort using pointers to avoid stack overflow
    const TreeEntry *ptrs[MAX_TREE_ENTRIES];
    for (int i = 0; i < tree->count; i++) {
        ptrs[i] = &tree->entries[i];
    }
    qsort(ptrs, tree->count, sizeof(TreeEntry *), compare_tree_ptrs);

    size_t offset = 0;
    for (int i = 0; i < tree->count; i++) {
        const TreeEntry *entry = ptrs[i];
        int written = sprintf((char *)buffer + offset, "%o %s", entry->mode, entry->name);
        offset += written + 1;
        memcpy(buffer + offset, entry->hash.hash, HASH_SIZE);
        offset += HASH_SIZE;
    }

    *data_out = buffer;
    *len_out = offset;
    return 0;
}

// ─── Recursive helper to build tree from a level of paths ──────────────────
static int write_tree_recursive(IndexEntry **entries, int count, int depth, ObjectID *id_out) {
    Tree tree;
    tree.count = 0;

    for (int i = 0; i < count; ) {
        const char *path = entries[i]->path;
        
        // Find current component at this depth
        const char *current_comp = path;
        for (int d = 0; d < depth; d++) {
            current_comp = strchr(current_comp, '/');
            if (current_comp) current_comp++;
            else break;
        }
        
        if (!current_comp) { i++; continue; }

        const char *next_slash = strchr(current_comp, '/');
        
        if (next_slash == NULL) {
            // It's a file
            if (tree.count >= MAX_TREE_ENTRIES) return -1;
            TreeEntry *te = &tree.entries[tree.count++];
            te->mode = entries[i]->mode;
            te->hash = entries[i]->hash;
            strncpy(te->name, current_comp, sizeof(te->name)-1);
            te->name[sizeof(te->name)-1] = '\0';
            i++;
        } else {
            // It's a directory
            char dir_name[256];
            size_t dir_len = next_slash - current_comp;
            if (dir_len >= sizeof(dir_name)) dir_len = sizeof(dir_name) - 1;
            memcpy(dir_name, current_comp, dir_len);
            dir_name[dir_len] = '\0';
            
            // Group entries under this directory
            int start = i;
            while (i < count) {
                const char *p = entries[i]->path;
                const char *cc = p;
                for (int d = 0; d < depth; d++) {
                    cc = strchr(cc, '/');
                    if (cc) cc++;
                }
                if (cc && strncmp(cc, dir_name, dir_len) == 0 && (cc[dir_len] == '/' || cc[dir_len] == '\0')) {
                    i++;
                } else {
                    break;
                }
            }
            
            ObjectID sub_hash;
            if (write_tree_recursive(&entries[start], i - start, depth + 1, &sub_hash) < 0) {
                return -1;
            }
            
            if (tree.count >= MAX_TREE_ENTRIES) return -1;
            TreeEntry *te = &tree.entries[tree.count++];
            te->mode = MODE_DIR;
            te->hash = sub_hash;
            strncpy(te->name, dir_name, sizeof(te->name)-1);
            te->name[sizeof(te->name)-1] = '\0';
        }
    }

    void *data;
    size_t len;
    if (tree_serialize(&tree, &data, &len) < 0) return -1;
    if (object_write(OBJ_TREE, data, len, id_out) < 0) {
        free(data);
        return -1;
    }
    free(data);
    return 0;
}

// Build a tree hierarchy from the current index and write all tree
// objects to the object store.
int tree_from_index(ObjectID *id_out) {
    Index index;
    if (index_load(&index) < 0) return -1;
    if (index.count == 0) return -1;

    // Index is already sorted by path in index_save
    IndexEntry *ptr_entries[MAX_INDEX_ENTRIES];
    for (int i = 0; i < index.count; i++) {
        ptr_entries[i] = &index.entries[i];
    }
    
    return write_tree_recursive(ptr_entries, index.count, 0, id_out);
}
