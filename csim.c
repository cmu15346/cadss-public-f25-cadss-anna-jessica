/**
 * @file csim.c
 * @brief Contains cache simulator
 *
 * Cache simulator which simulates a cache trace file consisting of lines
 * formatted as: Op, Addr, Size, where the operation represents something the
 * CPU would like to do with memory -- loading or storing some amount of bytes
 * at an address. Caching allows the computer to perform these reads and writes
 * at a much higher speed than if the data was being read from a larger, less
 * efficient form of memory. The simulator, given cache parameters such as s, b,
 * E, and a trace file, will simulate the trace's operations on a cache with
 * 2**s sets, E lines per set, and a block size of 2**b. The simulator uses the
 * LRU policy when handling evictions from memory. After the trace is complete,
 * the following statistics will be outputted:
 *      - hits
 *      - misses
 *      - evictions
 *      - dirty bytes in cache after the trace
 *      - dirty bytes evicted in the trace's lifetime
 *
 * The cache simulator was implemented using a 2D array of cache_line structs,
 * implemented as S cache_line* pointers that point to E contiguous cache_line
 * structs in memory. Each cache_line consists of a valid bit which is 1
 * when the cache line represents a valid line, a dirty bit that represents
 * whether data has been written to the address with a store address but hasn't
 * been evicted, and a tag which is a 64-(s+b) bit long identifier for a range
 * of addresses. Finally the LRU_counter acts as a "timestamp" for the last time
 * the cache memory has been read from/written to
 *
 *
 * @author Jessica Li <jgli@andrew.cmu.edu>
 */

#include "cachelab.h"
#include <errno.h>
#include <getopt.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Maximum length of line - 20 characters, then include '\n'
#define LINELEN 21

typedef struct {
    bool valid_bit;
    bool dirty_bit;
    unsigned long tag;
    size_t LRU_counter;
} cache_line;

// Global variables for the input options + statistics of cache simulation
bool verbose = false;
unsigned long s, b, E, B = 0;
csim_stats_t *stats;

// Help string for csim
const char helpstr[] =
    "Usage: ./csim-ref [-v] -s <s> -b <b> -E <E> -t <trace>\n\
    ./csim-ref -h\n\n\
    -h\tPrint this help message and exit\n\
    -v\tVerbose mode: report effects of each memory operation\n\
    -s <s>\tNumber of set index bits (there are 2**s sets)\n\
    -b <b>\tNumber of block bits (there are 2**b blocks)\n\
    -E <E>\tNumber of lines per set (associativity)\n\
    -t <trace>\tFile name of the memory trace to process\n\
\n\
The -s, -b, -E, and -t options must be supplied for all simulations.\n";

// Credit code and description from 15-122!!
/* xcalloc(nobj, size) returns a non-NULL pointer to
 * array of nobj objects, each of size size and
 * exits if the allocation fails.  Like calloc, the
 * array is initialized with zeroes.
 */
static inline void *xcalloc(size_t nobj, size_t size) {
    void *p = calloc(nobj, size);
    if (p == NULL) {
        fprintf(stderr, "allocation failed\n");
        abort();
    }
    return p;
}

/**
 * @brief Checks if s, b, and E are valid input parameters to csim
 *
 * @param[in]     s    Number of sets = 2 ** s
 * @param[in]     b    Number of bytes in a block = 2 ** b
 * @param[in]     E    Number of lines in a set
 *
 * Raises an error if E == 0 or if s and b sum to more than 64
 */
void check_valid(unsigned long s, unsigned long b, unsigned long E) {
    if (E == 0) {
        fprintf(stderr, "E must be positive\n");
        exit(1);
    }
    if (s + b > 64) {
        fprintf(stderr, "s + b must be <= 64\n");
        exit(1);
    }
}

/**
 * @brief Simulates a load into the cache
 * Very similar to store, but the two functions kept separate for clarity
 *
 * @param[in]     cache        2D array of cache lines
 * @param[in]     addr         Address we are reading from
 * @param[in]     iteration    "Timestamp" of load operation
 *
 * Updates cache with result of load operation using a given address
 */
void load(cache_line **cache, unsigned long addr, unsigned long iteration) {
    unsigned long addr_set_index, addr_tag;
    if (s == 0) {
        addr_set_index = 0;
        addr_tag = addr >> b;
    } else {
        addr_set_index = (addr << (64UL - (s + b))) >> (64UL - s);
        addr_tag = addr >> (s + b);
    }
    cache_line *curr_set = cache[addr_set_index];

    if (verbose)
        fprintf(stderr, "set index: %lu\n", addr_set_index);
    if (verbose)
        fprintf(stderr, "tag: %lu\n", addr_tag);

    // Loop through lines to get matching tag
    for (unsigned long line_index = 0; line_index < E; line_index++) {
        if ((curr_set[line_index].tag == addr_tag) &&
            curr_set[line_index].valid_bit) {
            // Found tag + valid bit YAY, hit!!
            if (verbose)
                fprintf(stderr, "HIT\n");
            // update LRU_counter
            curr_set[line_index].LRU_counter = iteration;
            // Increment hit count
            stats->hits++;
            return;
        }
    }
    // Didn't find a tag? Ok need to insert:
    // Increment miss count
    stats->misses++;
    // Loop again, look for invalid bits
    for (unsigned long line_index = 0; line_index < E; line_index++) {
        if (!curr_set[line_index].valid_bit) {
            if (verbose)
                fprintf(stderr, "MISS, but no evict\n");
            // Found invalid bit? - YAY miss
            // update curr_set[line_index] with values....
            curr_set[line_index].valid_bit = true;
            curr_set[line_index].dirty_bit = false;
            curr_set[line_index].tag = addr_tag;
            curr_set[line_index].LRU_counter = iteration;
            return;
        }
    }

    // Didn't find invalid bits? - need to evict -- find least recently used
    // index
    unsigned long LRU_index = 0;
    unsigned long smallest_LRU = curr_set[0].LRU_counter;
    for (unsigned long line_index = 1; line_index < E; line_index++) {
        if (curr_set[line_index].LRU_counter < smallest_LRU) {
            smallest_LRU = curr_set[line_index].LRU_counter;
            LRU_index = line_index;
        }
    }

    // Overwrite the line - YAY miss, evict
    if (verbose)
        fprintf(stderr, "MISS, but evict!\n");
    // Update entry
    // If that index has dirty bits - we are evicting dirty bits!
    if (curr_set[LRU_index].dirty_bit) {
        // That means dirty_bytes_in_cache should go down by 2**b
        stats->dirty_bytes -= B;
        // And dirty_bytes_evicted should go up by 2**b
        stats->dirty_evictions += B;
    }
    // since loading, we set dirty bit to false
    curr_set[LRU_index].dirty_bit = false;
    // update tag and LRU_counter
    curr_set[LRU_index].tag = addr_tag;
    curr_set[LRU_index].LRU_counter = iteration;

    // Increment evict count
    stats->evictions++;
}

/**
 * @brief Simulates a store into the cache
 *
 * @param[in]     cache        2D array of cache lines
 * @param[in]     addr         Address we are reading from
 * @param[in]     iteration    "Timestamp" of load operation
 *
 * Updates cache with result of store operation using a given address
 */
void store(cache_line **cache, unsigned long addr, unsigned long iteration) {
    unsigned long addr_set_index, addr_tag;
    if (s == 0) {
        addr_set_index = 0;
        addr_tag = addr >> b;
    } else {
        addr_set_index = (addr << (64UL - (s + b))) >> (64UL - s);
        addr_tag = addr >> (s + b);
    }
    cache_line *curr_set = cache[addr_set_index];

    if (verbose)
        fprintf(stderr, "set index: %lu\n", addr_set_index);
    if (verbose)
        fprintf(stderr, "tag: %lu\n", addr_tag);

    // Loop through lines to get matching tag
    for (unsigned long line_index = 0; line_index < E; line_index++) {
        if ((curr_set[line_index].tag == addr_tag) &&
            curr_set[line_index].valid_bit) {
            // Found tag + valid bit YAY, hit!!
            if (verbose)
                fprintf(stderr, "HIT\n");
            // update LRU_counter
            curr_set[line_index].LRU_counter = iteration;
            // If dirty_bit isn't true - dirty_bytes_in_cache should go up by
            // 2**b
            if (!curr_set[line_index].dirty_bit)
                stats->dirty_bytes += B;
            // update dirty bit
            curr_set[line_index].dirty_bit = true;
            // Increment hit count
            stats->hits++;
            return;
        }
    }
    // Didn't find a tag? Ok need to insert:
    // Increment miss count
    stats->misses++;
    // Loop again, look for invalid bits
    for (unsigned long line_index = 0; line_index < E; line_index++) {
        if (!curr_set[line_index].valid_bit) {
            if (verbose)
                fprintf(stderr, "MISS, but no evict\n");
            // Found invalid bit? - YAY miss
            // dirty_bytes_in_cache should go up by 2**b
            stats->dirty_bytes += B;
            // update curr_set[line_index] with values....
            curr_set[line_index].valid_bit = true;
            curr_set[line_index].dirty_bit = true;
            curr_set[line_index].tag = addr_tag;
            curr_set[line_index].LRU_counter = iteration;
            return;
        }
    }

    // Didn't find invalid bits? - need to evict -- find least recently used
    // index
    unsigned long LRU_index = 0;
    unsigned long smallest_LRU = curr_set[0].LRU_counter;
    for (unsigned long line_index = 1; line_index < E; line_index++) {
        if (curr_set[line_index].LRU_counter < smallest_LRU) {
            smallest_LRU = curr_set[line_index].LRU_counter;
            LRU_index = line_index;
        }
    }

    // Overwrite the line - YAY miss, evict
    if (verbose)
        fprintf(stderr, "MISS, but evict!\n");
    // If that index has dirty bits - we are evicting dirty bits!
    if (curr_set[LRU_index].dirty_bit) {
        // dirty_bytes_in_cache should stay the same...
        stats->dirty_bytes -= B;
        // But dirty_bytes_evicted should go up by 2**b
        stats->dirty_evictions += B;
    }
    stats->dirty_bytes += B;
    // update tag and LRU_counter and make sure dirty_bit is set to true
    curr_set[LRU_index].dirty_bit = true;
    curr_set[LRU_index].tag = addr_tag;
    curr_set[LRU_index].LRU_counter = iteration;

    // Increment evict count
    stats->evictions++;
}

/** Process a memory-access trace file.
 *
 * @param trace Name of the trace file to process .
 * @return 0 if successful , 1 if there were errors.
 */
int process_trace_file(const char *trace, cache_line **cache) {
    FILE *tfp = fopen(trace, "rt");
    if (!tfp) {
        fprintf(stderr, "Error opening '%s': %s\n", trace, strerror(errno));
        return 1;
    }
    unsigned long iteration = 0;
    char linebuf[LINELEN]; // How big should LINELEN be?
    while (fgets(linebuf, LINELEN, tfp)) {
        // What do you do if the line is longer than LINELEN-1 chars?
        size_t linebuflen = strlen(linebuf);
        if (linebuf[linebuflen - 1] != '\n') {
            fprintf(stderr, "AHHH 2 long\n");
            return 1; // error, line is too long
        }
        // Parse the line of text in 'linebuf'.
        char op;
        unsigned long addr;
        unsigned int size;
        // What do you do if the line is incorrect?
        if (sscanf(linebuf, "%c %lx,%i", &op, &addr, &size) != 3) {
            fprintf(stderr, "AHHH incorrect\n");
            return 1; // error, line does not parse properly
        }
        if (verbose)
            fprintf(stderr, "%c %lx,%i ", op, addr, size);
        if (op == 'L')
            load(cache, addr, iteration);
        if (op == 'S')
            store(cache, addr, iteration);
        iteration++;
    }
    fclose(tfp);
    return 0;
}

/**
 * @brief Parses command line arguments, initializes variables required
 * for the simulator and frees memory at the end
 *
 * @return 0 if no errors occur, 1 otherwise
 */
int main(int argc, char **argv) {
    int opt;
    bool s_flag, b_flag, E_flag, t_flag = false;
    char *t;
    // parse command line arguments
    while ((opt = getopt(argc, argv, "s:b:E:t:vh")) != -1) {
        switch (opt) {
        case 'h':
            fprintf(stderr, helpstr);
            exit(0);
            break;
        case 's':
            s_flag = true;
            s = strtoul(optarg, NULL, 10);
            break;
        case 'b':
            b_flag = true;
            b = strtoul(optarg, NULL, 10);
            break;
        case 'E':
            E_flag = true;
            E = strtoul(optarg, NULL, 10);
            break;
        case 't':
            t_flag = true;
            t = optarg;
            break;
        case 'v':
            verbose = true;
            break;
        case '?': /* '?', opt not included in options */
            fprintf(stderr, "error while parsing args.\n");
            fprintf(stderr, helpstr);
            exit(1);
            break;
        }
    }
    // missing mandatory arguments
    if (!(s_flag && b_flag && E_flag && t_flag)) {
        fprintf(stderr, "missing mandatory args.\n");
        fprintf(stderr, helpstr);
        exit(1);
    }
    // nonoption arguments
    if (optind < argc) {
        fprintf(stderr, "extra arguments.\n");
        fprintf(stderr, helpstr);
        exit(1);
    }
    if (verbose)
        fprintf(stderr, "here are the args: %lu %lu %lu %s.\n", s, b, E, t);
    check_valid(s, b, E);
    unsigned long S = 1UL << s;
    B = 1UL << b;

    // create cache in memory
    cache_line **cache = (cache_line **)xcalloc(
        S, sizeof(cache_line *)); // equivalent to cache[S][E]

    // clear the cache
    for (unsigned long i = 0; i < S; i++) {
        cache[i] = (cache_line *)xcalloc(E, sizeof(cache_line));
    }

    stats = xcalloc(1, sizeof(csim_stats_t));

    if (t != NULL) {
        if (process_trace_file(t, cache))
            exit(1);
    }

    // free memory
    for (unsigned long i = 0; i < S; i++) {
        free(cache[i]);
    }
    free(cache);

    printSummary(stats);
    free(stats);
    return 0;
}

