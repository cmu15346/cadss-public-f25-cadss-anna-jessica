project(cache-final-1)
add_library(cache-final-1 SHARED cache.c)
target_include_directories(cache-final-1 PRIVATE ../common)
