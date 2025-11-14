project(processor-p4)
add_library(processor-p4 SHARED processor.c)
target_include_directories(processor-p4 PRIVATE ../common)
