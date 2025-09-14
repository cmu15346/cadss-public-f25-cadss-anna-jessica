project(cache-p1)
add_library(cache-p1 SHARED cache.c)
target_include_directories(cache-p1 PRIVATE ../common)
