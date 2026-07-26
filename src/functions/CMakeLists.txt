add_library(
  uc_ext_functions OBJECT
  uc_checkpoint.cpp uc_delta_ccv2_commit.cpp uc_table_data_path.cpp
  uc_deletion_vector.cpp uc_plan_table_scan.cpp)
set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:uc_ext_functions>
    PARENT_SCOPE)
