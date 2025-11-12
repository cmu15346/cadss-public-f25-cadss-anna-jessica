project(coherence-p5)
add_library(coherence-p5 SHARED coherence.c protocol.c stree.c)
target_include_directories(coherence-p5 PRIVATE ../common)
