# Databricks tests
These tests rely on a databricks account preloaded with the right data.
Note that these are currently manually populated, this is to be changed in the future.


## Main env variables
To run these tests, log into 1password and run:

```shell
op read op://testing-rw/databricks_ccv2/_env | op inject
```

and execute the export statements variables