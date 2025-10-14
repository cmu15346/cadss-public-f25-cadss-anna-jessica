project(branch-p2)
add_library(branch-p2 SHARED branch.c)
target_include_directories(branch-p2 PRIVATE ../common)
