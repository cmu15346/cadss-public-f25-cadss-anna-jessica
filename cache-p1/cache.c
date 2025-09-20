#include "../common/cache.h"
#include "../common/trace.h"

#include <assert.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct _pendingRequest {
    int64_t permAddr;
    bool isLoad;
    bool isStarted;
    bool hasEvict;
    bool isHit;
    int64_t invAddr;
    struct _pendingRequest *next;
} pendingRequest;

int64_t globalTag = 0;
int8_t procNum = 0;

void (*memCallback)(int, int64_t);

cache *self = NULL;
coher *coherComp = NULL;

int processorCount = 1;
int CADSS_VERBOSE = 0;
pendingRequest *pending = NULL;

#define DPRINTF(args...)  if (CADSS_VERBOSE) { printf(args); }


void enqueueRequest(int64_t permAddr, bool isLoad, bool isStarted,
                    bool hasEvict, bool isHit, int64_t invAddr) {
    struct _pendingRequest *newReq = malloc(sizeof(struct _pendingRequest));
    // initialize newReq
    // newReq->tag = tag;
    // newReq->procNum = procNum;
    newReq->permAddr = permAddr;
    newReq->isLoad = isLoad;
    newReq->isStarted = isStarted;
    newReq->hasEvict = hasEvict;
    newReq->isHit = isHit;
    newReq->invAddr = invAddr;
    // push newReq to queue
    if (pending == NULL) {
        pending = newReq;
        pending->next = NULL;
    } else {
        pending->next = newReq;
    }
}

void memoryRequest(trace_op *op, int processorNum, int64_t tag,
                   void (*callback)(int, int64_t));
void coherCallback(int type, int procNum, int64_t addr);

unsigned long E, s, B, i, k = 0;
unsigned long S, L = 0;
unsigned long iteration = 0; // timestamp used for LRU

typedef struct {
    bool valid_bit;
    bool dirty_bit;
    unsigned long tag;
    size_t LRU_counter;
} cache_line;

cache_line **main_cache = NULL;

/**
 * @brief Simulates a load into the cache
 * Very similar to store, but the two functions kept separate for clarity
 *
 * @param[in]     addr         Address we are reading from
 *
 * Updates cache with result of load operation using a given address
 */
int load(unsigned long addr, unsigned long *evict_addr) {
    unsigned long addr_set_index, addr_tag;
    if (s == 0) {
        addr_set_index = 0;
        addr_tag = addr >> B;
    } else {
        addr_set_index = (addr << (64UL - (s + B))) >> (64UL - s);
        addr_tag = addr >> (s + B);
    }
    // DPRINTF("addr as hex: %lX\n", addr);
    // DPRINTF("set index: %ld, S: %ld\n", addr_set_index, S);
    cache_line *curr_set = main_cache[addr_set_index];

    // Loop through lines to get matching tag
    for (unsigned long line_index = 0; line_index < E; line_index++) {
        if ((curr_set[line_index].tag == addr_tag) &&
            curr_set[line_index].valid_bit) {
            // Found tag + valid bit YAY, hit!!
            // update LRU_counter
            curr_set[line_index].LRU_counter = iteration;
            return 0; // HIT
        }
    }
    // Didn't find a tag? Ok need to insert:
    // Loop again, look for invalid bits
    for (unsigned long line_index = 0; line_index < E; line_index++) {
        if (!curr_set[line_index].valid_bit) {
            // Found invalid bit? - YAY miss
            // update curr_set[line_index] with values....
            curr_set[line_index].valid_bit = true;
            curr_set[line_index].dirty_bit = false;
            curr_set[line_index].tag = addr_tag;
            curr_set[line_index].LRU_counter = iteration;
            return 1; // MISS, no evict
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
    // evict address: translate set index and line number to address
    // if (s == 0) {
    //     addr_set_index = 0;
    //     addr_tag = addr >> B;
    // } else {
    //     addr_set_index = (addr << (64UL - (s + B))) >> (64UL - s);
    //     addr_tag = addr >> (s + B);
    // }

    if (s == 0) {
        *evict_addr = curr_set[LRU_index].tag << B;
    } else {
        *evict_addr = (curr_set[LRU_index].tag << (s + B)) + (LRU_index << B);
    }
    
    // *evict_addr = 0x20;
    DPRINTF("calculated evict address: 0x%lX\n", *evict_addr);

    // Overwrite the line - YAY miss, evict
    // Update entry
    // If that index has dirty bits - we are evicting dirty bits!
    // since loading, we set dirty bit to false
    curr_set[LRU_index].dirty_bit = false;
    // update tag and LRU_counter
    curr_set[LRU_index].tag = addr_tag;
    curr_set[LRU_index].LRU_counter = iteration;

    return 2; // MISS and EVICT
}

/**
 * @brief Simulates a store into the cache
 *
 * @param[in]     addr         Address we are writing to
 *
 * Updates cache with result of store operation using a given address
 */
int store(unsigned long addr, unsigned long *evict_addr) {
    unsigned long addr_set_index, addr_tag;
    if (s == 0) {
        addr_set_index = 0;
        addr_tag = addr >> B;
    } else {
        addr_set_index = (addr << (64UL - (s + B))) >> (64UL - s);
        addr_tag = addr >> (s + B);
    }
    cache_line *curr_set = main_cache[addr_set_index];

    // Loop through lines to get matching tag
    for (unsigned long line_index = 0; line_index < E; line_index++) {
        if ((curr_set[line_index].tag == addr_tag) &&
            curr_set[line_index].valid_bit) {
            // Found tag + valid bit YAY, hit!!
            // update LRU_counter
            curr_set[line_index].LRU_counter = iteration;
            // update dirty bit
            curr_set[line_index].dirty_bit = true;
            return 0; // HIT
        }
    }
    // Didn't find a tag? Ok need to insert:
    // Loop again, look for invalid bits
    for (unsigned long line_index = 0; line_index < E; line_index++) {
        if (!curr_set[line_index].valid_bit) {
            // Found invalid bit? - YAY miss
            // update curr_set[line_index] with values....
            curr_set[line_index].valid_bit = true;
            curr_set[line_index].dirty_bit = true;
            curr_set[line_index].tag = addr_tag;
            curr_set[line_index].LRU_counter = iteration;
            return 1; // MISS, no evict
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
    // evict address: translate set index and line number to address
    if (s == 0) {
        *evict_addr = curr_set[LRU_index].tag << B;
    } else {
        *evict_addr = (curr_set[LRU_index].tag << (s + B)) + (LRU_index << B);
    }
    DPRINTF("calculated evict address: %lX", *evict_addr);

    // Overwrite the line - YAY miss, evict
    // If that index has dirty bits - we are evicting dirty bits!
    // update tag and LRU_counter and make sure dirty_bit is set to true
    curr_set[LRU_index].dirty_bit = true;
    curr_set[LRU_index].tag = addr_tag;
    curr_set[LRU_index].LRU_counter = iteration;
    return 2; // MISS and EVICT
}

cache *init(cache_sim_args *csa) {
    int op;

    // get argument list from assignment
    while ((op = getopt(csa->arg_count, csa->arg_list, "E:s:b:i:R:")) != -1) {
        switch (op) {
        // Lines per set
        case 'E':
            E = strtoul(optarg, NULL, 10);
            break;

        // Sets per cache
        case 's':
            s = strtoul(optarg, NULL, 10);
            S = 1UL << s;
            break;

        // block size in bits
        case 'b':
            B = strtoul(optarg, NULL, 10);
            L = 1 << B;
            break;

        // entries in victim cache
        case 'i':
            i = strtoul(optarg, NULL, 10);
            break;

        // bits in a RRIP-based replacement policy
        case 'R':
            k = strtoul(optarg, NULL, 10);
            break;
        }
    }

    // create cache in memory
    main_cache = (cache_line **)calloc(
        S, sizeof(cache_line *)); // equivalent to cache[S][E]

    // initialize/clear the cache
    for (unsigned long i = 0; i < S; i++) {
        main_cache[i] = (cache_line *)calloc(E, sizeof(cache_line));
    }

    self = malloc(sizeof(cache));
    self->memoryRequest = memoryRequest;
    self->si.tick = tick;
    self->si.finish = finish;
    self->si.destroy = destroy;

    coherComp = csa->coherComp;
    coherComp->registerCacheInterface(coherCallback);

    return self;
}

// This routine is a linkage to the rest of the memory hierarchy
void coherCallback(int type, int procNum, int64_t addr) {
    switch (type) {
    case NO_ACTION:
        DPRINTF("** received inv callback\n");
        coherComp->permReq(pending->isLoad, pending->permAddr, procNum);
        break;
    case DATA_RECV:
        // check that the addr is the pending access
        assert(addr == pending->permAddr);
        // This indicates that the cache has received data from memory
        pendingRequest *temp = pending;
        DPRINTF("popping from queue\n");
        pending = pending->next;
        DPRINTF("address of pending: %p\n", pending);
        free(temp);
        break;

    case INVALIDATE:
        // This is taught later in the semester.
        break;

    default:
        break;
    }
}

void memoryRequest(trace_op *op, int processorNum, int64_t tag,
                   void (*callback)(int, int64_t)) {
    assert(op != NULL);
    assert(callback != NULL);

    // Simple model to only have one outstanding memory operation
    // if (countDown != 0)
    // {
    //     assert(pending.memCallback != NULL);
    //     pending.memCallback(pending.procNum, pending.tag);
    // }
    assert(pending == NULL);

    // In a real cache simulator, the delay is based
    // on whether the request is a hit or miss.
    globalTag = tag;
    procNum = processorNum;
    memCallback = callback;

    int res1, res2 = 0;
    unsigned long evict_addr;

    switch (op->op) {
    case MEM_LOAD:
    case MEM_STORE:
        // load first address
        res1 = op->op == MEM_LOAD ? load(op->memAddress, &evict_addr)
                                  : store(op->memAddress, &evict_addr);
        uint64_t addr = op->memAddress & ~(L - 1);
        if (res1 == 1) {
            // just miss
            DPRINTF("miss\n");

            enqueueRequest(addr, op->op == MEM_LOAD, false, false, false,0);
            // coherComp->permReq(true, addr, processorNum);
        } else if (res1 == 2) {
            // miss and evict
            DPRINTF("miss\n");
            enqueueRequest(addr, op->op == MEM_LOAD, false, true, false, evict_addr);
            // coherComp->invlReq(addr, processorNum);
            // coherComp->permReq(true, addr, processorNum);
        } else {
            enqueueRequest(addr, false, false, false, true, 0);
        }

        // check if access crosses line boundary
        if (op->memAddress % L + op->size > L) {
            // access spans two lines, load the next address as well
            uint64_t next_addr = (op->memAddress & ~(L - 1)) + L;
            res2 = op->op == MEM_LOAD ? load(next_addr, &evict_addr)
                                      : store(next_addr, &evict_addr);
            if (res2 == 1) {
                // just miss
                enqueueRequest(next_addr, op->op == MEM_LOAD, false, false, false, 0);
                // coherComp->permReq(true, next_addr, processorNum);
            } else if (res2 == 2) {
                // miss and evict
                // coherComp->invlReq(next_addr, processorNum);
                // coherComp->permReq(true, next_addr, processorNum);
                enqueueRequest(next_addr, op->op == MEM_LOAD, false, true, false,
                               evict_addr);
            } else {
            enqueueRequest(next_addr, false, false, false, true, 0);
            }
        }
        break;
    case NONE:
    case BRANCH:
    case ALU:
    case ALU_LONG:
    case END:
        assert(false);
        break;
    }

    // if no need to access memory
    if (!res1 && !res2) {
        DPRINTF("HIT\n");
    }
}

int tick() {
    // Increment iteration count
    iteration++;

    // Advance ticks in the coherence component.
    coherComp->si.tick();

    if (pending == NULL) {
        DPRINTF("pending is null\n");
        if (memCallback != NULL) {
            memCallback(procNum, globalTag);
            memCallback = NULL;
            procNum = -1;
            globalTag = -1;
        }
    } else {
        if (pending->isHit) {
            pendingRequest *temp = pending;
            pending = pending->next;
            free(temp);
        } else if (!pending->isStarted) {
            if (pending->hasEvict) {
                coherComp->invlReq(pending->invAddr, procNum);
            } else {
                coherComp->permReq(pending->isLoad, pending->permAddr, procNum);
            }
            pending->isStarted = true;
        }
    }

    return 1;
}

int finish(int outFd) { return 0; }

int destroy(void) {
    // free any internally allocated memory here
    // free main cache
    for (unsigned long i = 0; i < S; i++) {
        free(main_cache[i]);
    }
    free(main_cache);

    return 0;
}
