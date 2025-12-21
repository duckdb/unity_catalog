# Unity Catalog Extension
Warning: this extension is an experimental, proof-of-concept for an extension, feel free to try it out, but no guarantees are given whatsoever.
This extension could be renamed, moved or removed at any point.

This is a proof-of-concept extension demonstrating DuckDB connecting to the Unity Catalog to scan Delta Table using 
the [delta extension](https://duckdb.org/docs/extensions/delta).

**Prerequisites:** Before using this extension, ensure your Unity Catalog metastore has `external_access_enabled` set to true, and that your user has the `EXTERNAL_USE_LOCATION` privilege on external locations (and `EXTERNAL_USE_SCHEMA` privilege on schemas for external tables). See the [Databricks API documentation](https://docs.databricks.com/api/workspace/temporarytablecredentials/generatetemporarytablecredentials) for details.

The credentials are vended for `READ` operations only if the catalog is attached as `READ_ONLY`, otherwise they are for `READ_WRITE`.

You can try it out using DuckDB (>= v1.0.0) on the platforms: `linux_amd64`, `linux_amd64_gcc4`, `osx_amd64` and `osx_arm64` by running:

```SQL
INSTALL unity_catalog;
INSTALL delta;
LOAD delta;
LOAD unity_catalog;
CREATE SECRET (
	TYPE UNITY_CATALOG,
	TOKEN '${UC_TOKEN}',
	ENDPOINT '${UC_ENDPOINT}',
	AWS_REGION '${UC_AWS_REGION}'
)
ATTACH 'test_catalog' AS test_catalog (TYPE UNITY_CATALOG)
SHOW ALL TABLES;
SELECT * FROM test_catalog.test_schema.test_table;
```
