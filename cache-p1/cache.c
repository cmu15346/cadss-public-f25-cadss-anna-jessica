#include "../common/cache.h"
#include "../common/trace.h"

#include <getopt.h>
#include <stdlib.h>
#include <assert.h>
#include <stdbool.h>

typedef struct _pendingRequest {
    int64_t tag;
    int8_t procNum;
    void (*memCallback)(int, int64_t);
} pendingRequest;

cache* self = NULL;
coher* coherComp = NULL;

int processorCount = 1;
int CADSS_VERBOSE = 0;
pendingRequest pending = {0};
int countDown = 0; // 0 = no pending requests, 1 = ready request, 2 = pending request

void memoryRequest(trace_op* op, int processorNum, int64_t tag,
                   void (*callback)(int, int64_t));
void coherCallback(int type, int procNum, int64_t addr);

unsigned long E, S, B, i, k = 0;
unsigned long L = 0;
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
int load(unsigned long addr) {
    unsigned long addr_set_index, addr_tag;
    if (S == 0) {
        addr_set_index = 0;
        addr_tag = addr >> B;
    } else {
        addr_set_index = (addr << (64UL - (S + B))) >> (64UL - S);
        addr_tag = addr >> (S + B);
    }
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
int store(unsigned long addr) {
    unsigned long addr_set_index, addr_tag;
    if (S == 0) {
        addr_set_index = 0;
        addr_tag = addr >> B;
    } else {
        addr_set_index = (addr << (64UL - (S + B))) >> (64UL - S);
        addr_tag = addr >> (S + B);
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

    // Overwrite the line - YAY miss, evict
    // If that index has dirty bits - we are evicting dirty bits!
    // update tag and LRU_counter and make sure dirty_bit is set to true
    curr_set[LRU_index].dirty_bit = true;
    curr_set[LRU_index].tag = addr_tag;
    curr_set[LRU_index].LRU_counter = iteration;
    return 2; // MISS and EVICT
}

cache* init(cache_sim_args* csa)
{
    int op;

    // get argument list from assignment
    while ((op = getopt(csa->arg_count, csa->arg_list, "E:s:b:i:R:")) != -1)
    {
        switch (op)
        {
            // Lines per set
            case 'E':
                E = strtoul(optarg, NULL, 10);
                break;

            // Sets per cache
            case 's':
                S = strtoul(optarg, NULL, 10);
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
void coherCallback(int type, int procNum, int64_t addr)
{
    switch (type)
    {
        case NO_ACTION:
        case DATA_RECV:
            // TODO: check that the addr is the pending access
            //  This indicates that the cache has received data from memory
            countDown = 1;
            break;

        case INVALIDATE:
            // This is taught later in the semester.
            break;

        default:
            break;
    }  
}

void memoryRequest(trace_op* op, int processorNum, int64_t tag,
                   void (*callback)(int, int64_t))
{
    assert(op != NULL);
    assert(callback != NULL);

    // Simple model to only have one outstanding memory operation
    // if (countDown != 0)
    // {
    //     assert(pending.memCallback != NULL);
    //     pending.memCallback(pending.procNum, pending.tag);
    // }
    assert(countDown == 0);

    pending = (pendingRequest){
        .tag = tag, .procNum = processorNum, .memCallback = callback};

    // In a real cache simulator, the delay is based
    // on whether the request is a hit or miss.
    countDown = 2;

    int res1, res2 = 0;

    switch(op->op) {
        case MEM_LOAD:
            // load first address
            res1 = load(op->memAddress);
            if (res1 == 1) {
                // just miss
                coherComp->permReq(true, op->memAddress, processorNum);
            } else if (res1 == 2) {
                // miss and evict
                coherComp->invlReq(op->memAddress, processorNum);
                coherComp->permReq(true, op->memAddress, processorNum);
            }

            // check if access crosses line boundary
            if (op->memAddress % L + op->size > L) {
                // access spans two lines, load the next address as well
                uint64_t next_addr = (op->memAddress & ~(L - 1)) + L;
                res2 = load(next_addr);
                if (res2 == 1) {
                    // just miss
                    coherComp->permReq(true, op->memAddress, processorNum);
                } else if (res2 == 2) {
                    // miss and evict
                    coherComp->invlReq(op->memAddress, processorNum);
                    coherComp->permReq(true, op->memAddress, processorNum);
                }
            }
            break;
        case MEM_STORE:
            // store first address
            res1 = store(op->memAddress);
            if (res1 == 1) {
                // just miss
                coherComp->permReq(false, op->memAddress, processorNum);
            } else if (res1 == 2) {
                // miss and evict
                coherComp->invlReq(op->memAddress, processorNum);
                coherComp->permReq(false, op->memAddress, processorNum);
            }

            // check if access crosses line boundary
            if (op->memAddress % L + op->size > L) {
                // access spans two lines, store the next address as well
                uint64_t next_addr = (op->memAddress & ~(L - 1)) + L;
                res2 = store(next_addr);
                if (res2 == 1) {
                    // just miss
                    coherComp->permReq(false, op->memAddress, processorNum);
                } else if (res2 == 2) {
                    // miss and evict
                    coherComp->invlReq(op->memAddress, processorNum);
                    coherComp->permReq(false, op->memAddress, processorNum);
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

    if (!res1 && !res2) {
        countDown = 1;
    }
    
    // Tell memory about this request
    // TODO: only do this if this is a miss
    // TODO: evictions will also need a call to memory with
    //  invlReq(addr, procNum) -> true if waiting, false if proceed

    coherComp->permReq(false, op->memAddress, processorNum);
}

int tick()
{
    // Increment iteration count
    iteration++;

    // Advance ticks in the coherence component.
    coherComp->si.tick();
    
    if (countDown == 1)
    {
        assert(pending.memCallback != NULL);
        pending.memCallback(pending.procNum, pending.tag);
    }

    return 1;
}

int finish(int outFd)
{
    return 0;
}

int destroy(void)
{
    // free any internally allocated memory here
    // free main cache
    for (unsigned long i = 0; i < S; i++) {
        free(main_cache[i]);
    }
    free(main_cache);

    return 0;
}
