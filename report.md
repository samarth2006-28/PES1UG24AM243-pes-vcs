# PES-VCS Lab Report

**Student Name:** Samarth
**SRN:** PES1UG24AM243

---

## Phase 1: Object Storage Foundation
- **Implemented:** `object_write` and `object_read`.
- **Key Concepts:** Content-addressable storage, sharding, and atomic writes via temp files and rename.

## Phase 2: Tree Objects
- **Implemented:** `tree_from_index` and recursive tree building.
- **Key Concepts:** Directory representation as tree objects, nested path handling, and deterministic serialization.

## Phase 3: The Index (Staging Area)
- **Implemented:** `index_load`, `index_save`, and `index_add`.
- **Key Concepts:** Staging area as a text file, metadata-based change detection, and atomic index updates.

## Phase 4: Commits and History
- **Implemented:** `commit_create`.
- **Key Concepts:** Snapshotting the staged state, parent pointers for history, and reference updates.

---

## Phase 5: Branching and Checkout (Analysis)

### Q5.1: Implementing `pes checkout <branch>`
To implement `pes checkout <branch>`, the following actions are required:
1. **Update `HEAD`**: The file `.pes/HEAD` must be updated to contain `ref: refs/heads/<branch>`.
2. **Update Working Directory**:
   - The current staging area (index) and working directory must be synchronized with the tree of the target commit (the one the branch points to).
   - Files present in the target tree but missing in the current directory must be created.
   - Files present in the current directory but missing in the target tree must be removed.
   - Files that exist in both but have different hashes must be overwritten with the content from the target tree.
3. **Complexities**:
   - **Uncommitted Changes**: If the user has modified files that are also different in the target branch, a simple overwrite would lose their work.
   - **Efficiency**: Recursively traversing and updating directories can be slow; Git uses the index to minimize disk I/O.
   - **Atomicity**: If the power fails mid-checkout, the working directory could be in a half-baked state.

### Q5.2: Detecting "Dirty Working Directory" Conflicts
A "dirty" conflict can be detected by comparing the state of the file across three points: the **Working Directory (WD)**, the **Current Index**, and the **Target Tree**.
1. Compare each file in the WD with its entry in the Index (using mtime/size/hash). If they differ, the WD is "dirty" for that file.
2. If a file is dirty, compare the Index's hash for that file with the Target Tree's hash.
3. If the file is dirty AND the Index's hash differs from the Target Tree's hash, checkout should be refused because merging or overwriting would cause data loss of uncommitted work.

### Q5.3: Detached HEAD State
"Detached HEAD" occurs when `.pes/HEAD` contains a raw commit hash instead of a branch reference.
- **Committing**: Making commits in this state works normally: a new commit object is created, and its parent is the current commit in HEAD. HEAD is then updated to point to the new commit's hash.
- **Recovery**: Because no branch reference "follows" these commits, they become "dangling" if the user checks out another branch. A user can recover them by creating a new branch at that specific commit hash: `pes branch <new-branch-name> <commit-hash>`.

---

## Phase 6: Garbage Collection (Analysis)

### Q6.1: Garbage Collection Algorithm
The standard algorithm is **Mark and Sweep**:
1. **Mark**: Start from all reachable references (all files in `.pes/refs/heads/`, the content of `.pes/HEAD`, etc.). Recursively follow every tree and blob hash mentioned in those commits. Store all "seen" hashes in a set (or bitmask).
2. **Sweep**: Scan the `.pes/objects/` directory. For every file found, check if its hash is in the "seen" set. If not, the object is unreachable and can be safely deleted.
- **Visit Estimate**: In a repository with 100,000 commits and 50 branches, you would visit each of the 50 branch tips and work backwards. Since many branches share history, you'd likely visit ~100,000 commit objects, plus the trees and blobs associated with the "latest" state of each branch.

### Q6.2: GC Race Conditions
A race condition occurs if `pes gc` runs while a `pes commit` is in progress:
1. `pes commit` writes a new blob to the object store.
2. `pes gc` starts, scans the current refs, and determines that the new blob is "unreachable" (because the commit object hasn't been written or the branch ref hasn't been updated yet).
3. `pes gc` deletes the blob.
4. `pes commit` finishes writing the commit and updates the branch ref.
5. The repository is now corrupted because a reachable commit points to a missing blob.
- **Git's Solution**: Git uses a **grace period** (objects are only deleted if they haven't been modified/accessed for a certain amount of time, e.g., 2 weeks) and careful ordering of operations to ensure objects being written are not prematurely collected.
