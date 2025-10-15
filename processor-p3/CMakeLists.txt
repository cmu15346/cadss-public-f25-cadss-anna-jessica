project(processor-p3)
add_library(processor-p3 SHARED processor.c)
target_include_directories(processor-p3 PRIVATE ../common)
