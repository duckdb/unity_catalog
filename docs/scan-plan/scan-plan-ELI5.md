# scan-plan, explained simply (ELI5)

Two pieces: **core** (the scan-plan read path, pre-deletes) and **deletes** (applying
delete-files). The technical version is `uc/docs/scan-plan-design.md`; this is the
plain-language companion.

---

## Piece 1 — Core: "ask the server which files to read"

### The problem
Some Unity Catalog tables are **catalog-managed**: you can't read them the normal Delta way
(grab the transaction log, figure out the files yourself). The commits live server-side. The
only way in is to **ask the server to plan the scan for you**.

### The analogy
Instead of browsing the library's whole card catalog and pulling books yourself, you hand the
librarian your request — *"books about X, published after Y"* — and they hand back a specific
stack of books plus a temporary key to the locked room they're in.

### What our code actually does
1. **You run a query with a `WHERE`.** DuckDB hands us the filter. We translate it into the
   server's filter language (JSON): `day = 'Mon'` → `{"type":"eq","term":"day","value":"Mon"}`.
   → `uc_irc_expression.cpp`
2. **We ask the server to plan.** POST the filter to the `/plan` endpoint. The server prunes to
   just the files that could match and hands back: a **list of data files** + **temporary S3
   credentials**. → `uc_api.cpp` (`PlanTableScan`)
3. **We feed those files to DuckDB's normal Parquet reader.** Instead of it globbing a
   directory, we hand it *our* server-provided list. → `uc_multi_file_list.cpp`
   (`UCMultiFileList` / `UCMultiFileReader`)
4. **DuckDB reads the files and re-applies your `WHERE` itself.** So results are always correct,
   even if our server-side filter was approximate.

### The three "gotchas" that make it non-trivial
- **The server filter is only a hint.** We never rely on it for correctness — DuckDB keeps and
  re-applies your real `WHERE`. The server filter just means "read fewer files." (This is why a
  filter that's *too tight* would be a bug — it'd make the server skip a file that actually had
  matching rows. Hence the float-precision fix: round a number wrong and you can silently lose
  rows.)
- **The server can say "still working, ask again."** Async planning — we poll until it's done.
  We pace the polling by the server's "wait this long" hint (or a gently growing backoff), stop
  promptly if you cancel the query, and if we give up we tell the server to throw the plan away.
- **Credentials are handed to us in the plan response** and wired into DuckDB secrets so the S3
  reader (httpfs) can actually fetch the files.

---

## Piece 2 — Deletes: "honor the sticky notes"

### The problem
Modern table formats don't rewrite a whole data file to delete a few rows. They leave the file
alone and write a small **sidecar** saying *"ignore rows 5, 15, 25 of this file."* So some of
the data files the server hands back come with sidecars. If we don't apply them, you'd see rows
that were supposedly deleted.

### The analogy
The librarian hands you the book — with a sticky note: *"pages 5, 15, 25 were retracted, don't
read them."* We honor the sticky note.

### Two kinds of sticky note (both identify rows by **position**)
1. **A little Parquet file** listing `(file_path, row_position)` pairs — "row 5 of file X is
   deleted." The older (Iceberg v2) style. We read the file and collect the positions.
   → `ScanPositionalDeleteFile`
2. **A deletion vector** — a compact **bitmap** packed inside a "puffin" file at a specific byte
   range. Rather than listing positions, it's a dense yes/no-per-row bitmap. The modern
   (Iceberg v3) style, and **what Databricks produces**. Decoding it: read exactly those bytes,
   check a checksum, and unpack a **roaring bitmap** (a standard compressed-bitmap format) into
   a set of deleted positions. → `ScanDeletionVectorFile` + `uc_puffin.cpp` (ported from
   ducklake, uses the CRoaring library)

Both kinds collapse to the same thing: **a set of deleted row positions for this file.** We hand
that to DuckDB's built-in delete-filter hook, and as DuckDB reads each batch of rows it drops the
ones in the set. → `BuildUCDeleteFilter` + `UCMultiFileReader::FinalizeBind`

### The kind we deliberately DON'T do
**Equality deletes** — *"delete any row where email = 'x@y.com'"* (by **value**, not position).
Doing this correctly needs knowing which column the delete refers to via its schema **field-id**,
and our read path doesn't carry that mapping. Guessing by column *name* would break the moment a
column is renamed — so instead we throw a clear "not implemented" error. (Ducklake, a sibling
project, draws the exact same line.) → the `NotImplementedException` in `BuildUCDeleteFilter`

### Why the puffin/roaring part was the scary bit
It's a **binary format** decoded byte-by-byte: magic numbers, a CRC checksum, a specific bitmap
layout. We didn't reinvent it — we ported a proven reader from ducklake and hardened its bounds
checks (e.g. the truncated-blob over-read fix). And it's **confirmed working against live
Databricks**: a real DV-enabled delete came back exactly as the puffin blob we decode.

### Poke at it yourself
There's a SQL function — `uc_read_deletion_vector(path [, content_offset =>, content_size =>])` —
that decodes a deletion vector to its list of deleted row positions, right from a query. Handy for
looking at a real delete file, and it lets the delete decoder be tested without any live server.

---

## One-sentence version
**Core:** ask the server which files to read, then read them with DuckDB's normal Parquet reader
and re-apply the filter locally. **Deletes:** some files come with a "these rows are deleted"
sidecar (a position list or a compressed bitmap) — decode it and tell DuckDB to skip those rows.
