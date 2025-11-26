project(processor-final-1)
add_library(processor-final-1 SHARED processor.c)
target_include_directories(processor-final-1 PRIVATE ../common)
