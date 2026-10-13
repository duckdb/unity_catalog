# TPC-H / TPC-DS databricks tests — analysis & disposition

> **Applied (2026-07-08):** per the recommendation below, `tpcds.test_slow` and the old
> data-gen (`scripts/databricks_data_gen/`) were **deleted**; `tpch.test_slow` is **kept** as a
> marker for the "slim to q1/q5/q6, make it collected" follow-up. The analysis is retained as the
> decision record — the file paths it cites under `scripts/databricks_data_gen/` no longer exist.

Assessment of `test/databricks/tpch.test_slow` and (the since-removed) `tpcds.test_slow` as we
modernize the databricks suite. **Neither ran** — the pytest driver collects only `.test`, not
`.test_slow`.

## What each test does

**`tpch.test_slow`** — Creates a UC secret, `ATTACH`es `duckdb_testing` at schema
`tpch_sf0_01`, then loops `PRAGMA tpch(i)` and diffs each result against the canonical
shipped answer CSV (`duckdb/extension/tpch/dbgen/answers/sf0.01/qNN.csv`). These are **real
full-result correctness checks** — every row/column of each query is compared to the golden
answer, not a smoke/perf check. Loop bounds `loop i 1 9` + `loop i 10 23` (end-exclusive)
run q1–q8 and q10–q22 → **21 of 22 queries; q9 is silently skipped** (a real gap). Data is
8 TPC-H tables at SF 0.01 (lineitem ~60k rows, etc.).

**`tpcds.test_slow`** — Same structure against `tpcds_sf0_01`, looping `PRAGMA tpcds(i)`
over q1–q8 and q10–q98 vs answer CSVs (misses q9 and q99). But it carries **`mode skip` at
the top**: the header TODO explains the old data-gen path exported via
pandas → `spark.createDataFrame()`, which emits a Databricks decimal encoding that
delta-kernel rejects (`MalformedJsonError ... DataType`). The data is **unparseable and the
test is hard-disabled** — dead even if renamed to `.test`.

Both are generated from one-liners (`call dbgen(sf=0.01)` / `call dsdgen(sf=0.01)`),
exported to parquet, and pushed to Databricks as Delta tables.

## Distinct coverage vs. the rest of the suite

The other databricks tests operate on **tiny, hand-shaped tables** (`simple_table` 5 rows,
evolution tables a few rows, `id_day_*` 50 rows) and target **mechanics**: attach/detach/use
+ the `TYPE UC` alias, `AT (VERSION => n)` time travel, column-mapping/name-mode evolution,
catalog-managed staged commits + `max_catalog_version`, insert round-trip, staged-commit
backfill/watermark logging. None read data at scale or with rich types.

What tpch (and tpcds, if it worked) uniquely stress in the UC/Delta **read** path:

- **Multi-table catalog resolution + larger Delta scans** — 8 tables in one schema, tables
  spanning multiple parquet add-files/row-groups, vs. single-table single-file reads
  everywhere else.
- **Rich type decoding through delta-kernel → DuckDB**: `DECIMAL` (prices) and `DATE`
  (filters/arithmetic). This is exactly the surface tpcds's TODO shows is *broken* today.
- **Filter/projection/predicate pushdown** into the Delta scan from real WHERE/date-range
  predicates, plus join- and aggregation-heavy execution on delta-sourced data.
- **End-to-end result correctness** (full result set vs. golden answers), where the other
  tests mostly assert `count(*)` or a few literal rows.

They add the one thing the targeted suite lacks: proof the read path returns spec-correct
data over many tables, decimals, and dates at non-trivial scale.

## Recommendation: **replace**

- **Drop `tpcds` outright.** It is `mode skip` *and* its data can't be generated in a
  kernel-readable form; a 99-query benchmark that has never run and is blocked on an upstream
  delta-kernel/decimal-encoding fix is pure maintenance weight. Track the decimal-encoding
  issue as a **bug**, not a skipped test.
- **Replace `tpch`** with a small, *collected* `.test` (renamed off `.test_slow` so the
  driver picks it up) running ~3–4 targeted queries that keep the golden-answer comparison —
  e.g. **q1** (decimal aggregation + date filter), **q5** (multi-table join), **q6**
  (date-range predicate pushdown). That preserves the distinct decimal/date/multi-table
  read-path coverage while cutting runtime and the full-account preload burden, and fixes
  the q9 gap by being explicit. Keep the full 22-query version only if a real slow-lane
  collector is added; otherwise it never runs and rots.

## Incidental note

tpcds's failure mode — pandas → Spark decimal → kernel `MalformedJsonError` — is exactly the
`spark.createDataFrame()` DataFrame-push path removed in the `databricks_gen` (all-SQL SDK)
migration. Seeding via `INSERT … VALUES` / `CREATE TABLE … AS SELECT` sidesteps that whole
class of bug.
