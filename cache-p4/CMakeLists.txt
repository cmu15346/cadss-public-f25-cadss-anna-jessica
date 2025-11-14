project(cache-p4)
add_library(cache-p4 SHARED cache.c)
target_include_directories(cache-p4 PRIVATE ../common)
