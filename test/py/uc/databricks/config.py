"""Databricks account configuration for the UC test suite.

Env-overridable defaults for the specific Databricks workspace the tests target -- the
databricks counterpart of server.py's `DUCKTEST_UC_*` block. Keep account-specific values HERE
(not scattered through the provisioner and tests) so pointing the suite at a different
workspace is a one-file change, or a set of env overrides.

The connection creds + warehouse are separate (DATABRICKS_ENDPOINT/TOKEN/REGION/WAREHOUSE_ID,
front-loaded by the launching shell; see engine.py `_require_creds` and databricks_gen.sql).
"""

import os

# Premade read-only tables (the id_name__* evolution / catalog-managed variants) live here.
READ_CATALOG = os.environ.get("DATABRICKS_READ_CATALOG", "duckdb_testing")

# Write tests provision per-test cell schemas under this catalog; also the default
# UC_TEST_CATALOG when the launching shell didn't set one.
WRITE_CATALOG = os.environ.get("DATABRICKS_WRITE_CATALOG", "duckdb_write_testing")

# S3 bucket backing `external` (non-managed) tables' LOCATION. The sibling
# ducklabs-uc-testing-ro bucket is not used yet: a def creates and seeds the table it reads,
# so read fixtures need a writable root too, and read/write share this one until the catalogs
# themselves split.
S3_BUCKET = os.environ.get("DATABRICKS_S3_BUCKET", "ducklabs-uc-testing-rw")
