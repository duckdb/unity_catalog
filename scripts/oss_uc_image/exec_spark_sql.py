#!/usr/bin/env python3

from pyspark.sql import SparkSession
import sys

spark = (
    SparkSession.builder.appName("create-id_name_4")
    .config(
        "spark.jars.packages",
    )
    .config("spark.sql.extensions", "io.delta.sql.DeltaSparkSessionExtension")
    .config(
        "spark.sql.catalog.spark_catalog",
        "org.apache.spark.sql.delta.catalog.DeltaCatalog",
    )
    .config("spark.sql.catalog.demo", "io.unitycatalog.spark.UCSingleCatalog")
    .config("spark.sql.catalog.demo.uri", "https://uc.openlakehousedemos.dev")
    .config("spark.hadoop.fs.s3.impl", "org.apache.hadoop.fs.s3a.S3AFileSystem")
    .config("spark.hadoop.fs.s3a.impl", "org.apache.hadoop.fs.s3a.S3AFileSystem")
    # .config( "spark.sql.catalog.demo.token", "TOKEN")
    .getOrCreate()
)


if __name__ == "__main__":
    inputs = []
    is_file = False
    for arg in sys.argv[1:]:
        if arg in ["-f", "--file"]:
            is_file = True
        else:
            is_file = False
            inputs.append(arg if not is_file else open(arg).read())

    for input in inputs:
        spark.sql(input)
