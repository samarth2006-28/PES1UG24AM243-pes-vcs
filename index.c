// index.c — Staging area implementation
//
// Text format of .pes/index (one entry per line, sorted by path):
//
//   <mode-octal> <64-char-hex-hash> <mtime-seconds> <size> <path>
//
// Example:
//   100644 a1b2c3d4e5f6...  1699900000 42 README.md
//   100644 f7e8d9c0b1a2...  1699900100 128 src/main.c
//
// This is intentionally a simple text format. No magic numbers, no
// binary parsing. The focus is on the staging area CONCEPT (tracking
// what will go into the next commit) and ATOMIC WRITES (temp+rename).
//
// PROVIDED functions: index_find, index_remove, index_status
// TODO functions:     index_load, index_save, index_add

#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <inttypes.h>

// ─── PROVIDED ────────────────────────────────────────────────────────────────

// Find an index entry by path (linear scan).
IndexEntry* index_find(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0)
            return &index->entries[i];
    }
    return NULL;
}

// Remove a file from the index.
// Returns 0 on success, -1 if path not in index.
int index_remove(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0) {
            int remaining = index->count - i - 1;
            if (remaining > 0)
                memmove(&index->entries[i], &index->entries[i + 1],
                        remaining * sizeof(IndexEntry));
            index->count--;
            return index_save(index);
        }
    }
    fprintf(stderr, "error: '%s' is not in the index\n", path);
    return -1;
}

// Print the status of the working directory.
//
// Identifies files that are staged, unstaged (modified/deleted in working dir),
// and untracked (present in working dir but not in index).
// Returns 0.
int index_status(const Index *index) {
    printf("Staged changes:\n");
    int staged_count = 0;
    // Note: A true Git implementation deeply diffs against the HEAD tree here. 
    // For this lab, displaying indexed files represents the staging intent.
    for (int i = 0; i < index->count; i++) {
        printf("  staged:     %s\n", index->entries[i].path);
        staged_count++;
    }
    if (staged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Unstaged changes:\n");
    int unstaged_count = 0;
    for (int i = 0; i < index->count; i++) {
        struct stat st;
        if (stat(index->entries[i].path, &st) != 0) {
            printf("  deleted:    %s\n", index->entries[i].path);
            unstaged_count++;
        } else {
            // Fast diff: check metadata instead of re-hashing file content
            if (st.st_mtime != (time_t)index->entries[i].mtime_sec || st.st_size != (off_t)index->entries[i].size) {
                printf("  modified:   %s\n", index->entries[i].path);
                unstaged_count++;
            }
        }
    }
    if (unstaged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Untracked files:\n");
    int untracked_count = 0;
    DIR *dir = opendir(".");
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            // Skip hidden directories, parent directories, and build artifacts
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            if (strcmp(ent->d_name, ".pes") == 0) continue;
            if (strcmp(ent->d_name, "pes") == 0) continue; // compiled executable
            if (strstr(ent->d_name, ".o") != NULL) continue; // object files

            // Check if file is tracked in the index
            int is_tracked = 0;
            for (int i = 0; i < index->count; i++) {
                if (strcmp(index->entries[i].path, ent->d_name) == 0) {
                    is_tracked = 1; 
                    break;
                }
            }
            
            if (!is_tracked) {
                struct stat st;
                stat(ent->d_name, &st);
                if (S_ISREG(st.st_mode)) { // Only list regular files for simplicity
                    printf("  untracked:  %s\n", ent->d_name);
                    untracked_count++;
                }
            }
        }
        closedir(dir);
    }
    if (untracked_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    return 0;
}

// ─── IMPLEMENTED ───────────────────────────────────────────────────────────

// Load the index from .pes/index.
int index_load(Index *index) {
    index->count = 0;
    FILE *f = fopen(INDEX_FILE, "r");
    if (!f) return 0; // Not an error if file doesn't exist

    char hex[HASH_HEX_SIZE + 1];
    while (index->count < MAX_INDEX_ENTRIES) {
        IndexEntry *e = &index->entries[index->count];
        // Read mode, hex-hash, mtime, size, and finally the path
        if (fscanf(f, "%o %64s %" PRIu64 " %u %[^\n]", &e->mode, hex, &e->mtime_sec, &e->size, e->path) != 5) {
            break;
        }
        hex_to_hash(hex, &e->hash);
        index->count++;
    }
    fclose(f);
    return 0;
}

// Helper for sorting index pointers by path
static int compare_index_ptrs(const void *a, const void *b) {
    return strcmp((*(const IndexEntry **)a)->path, (*(const IndexEntry **)b)->path);
}

// Save the index to .pes/index atomically.
int index_save(const Index *index) {
    // 1. Sort using pointers to avoid stack overflow with 5.6MB struct
    const IndexEntry *ptrs[MAX_INDEX_ENTRIES];
    for (int i = 0; i < index->count; i++) {
        ptrs[i] = &index->entries[i];
    }
    qsort(ptrs, index->count, sizeof(IndexEntry *), compare_index_ptrs);

    // 2. Write to a temporary file
    char temp_path[512];
    snprintf(temp_path, sizeof(temp_path), "%s.tmp", INDEX_FILE);
    FILE *f = fopen(temp_path, "w");
    if (!f) return -1;

    for (int i = 0; i < index->count; i++) {
        const IndexEntry *e = ptrs[i];
        char hex[HASH_HEX_SIZE + 1];
        hash_to_hex(&e->hash, hex);
        fprintf(f, "%o %s %" PRIu64 " %u %s\n", e->mode, hex, e->mtime_sec, e->size, e->path);
    }

    // 3. atomic sync: fflush -> fsync -> fclose -> rename
    fflush(f);
    fsync(fileno(f));
    fclose(f);

    if (rename(temp_path, INDEX_FILE) < 0) {
        unlink(temp_path);
        return -1;
    }

    return 0;
}

// Stage a file for the next commit.
int index_add(Index *index, const char *path) {
    // 1. Get file metadata
    struct stat st;
    if (stat(path, &st) != 0) {
        perror("stat");
        return -1;
    }
    if (!S_ISREG(st.st_mode)) {
        fprintf(stderr, "error: '%s' is not a regular file\n", path);
        return -1;
    }

    // 2. Read file content
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    void *data = malloc(st.st_size);
    if (!data) { fclose(f); return -1; }
    if (fread(data, 1, st.st_size, f) != (size_t)st.st_size && st.st_size > 0) {
        free(data);
        fclose(f);
        return -1;
    }
    fclose(f);

    // 3. Write as OBJ_BLOB
    ObjectID blob_id;
    if (object_write(OBJ_BLOB, data, st.st_size, &blob_id) < 0) {
        free(data);
        return -1;
    }
    free(data);

    // 4. Update or add index entry
    IndexEntry *e = index_find(index, path);
    if (!e) {
        if (index->count >= MAX_INDEX_ENTRIES) return -1;
        e = &index->entries[index->count++];
        strncpy(e->path, path, sizeof(e->path) - 1);
        e->path[sizeof(e->path) - 1] = '\0';
    }

    e->mode = 0100644;
    if (st.st_mode & S_IXUSR) e->mode = 0100755;
    
    e->hash = blob_id;
    e->mtime_sec = (uint64_t)st.st_mtime;
    e->size = (uint32_t)st.st_size;

    // 5. Save the updated index
    return index_save(index);
}
