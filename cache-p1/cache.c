#include "../common/cache.h"
#include "../common/trace.h"

#include <assert.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#define DPRINTF(args...)                                                       \
    if (CADSS_VERBOSE) {                                                       \
        printf(args);                                                          \
    }

typedef enum cacheResult_ { HIT , MISS , MISS_EVICT , NA } cacheResult;
typedef enum reqType_ { PERM , INV } reqType;

typedef struct _pendingRequest {
    int64_t addr;
    bool isStarted;
    bool isLoad;
    reqType requestType;
    cacheResult cacheResult;
    struct _pendingRequest *next;
} pendingRequest;

// nul terminated so tail points to last node
typedef struct _pendingQueue {
    pendingRequest* head;
    pendingRequest* tail;
} pendingQueue;

int64_t globalTag = 0;
int8_t procNum = 0;

void (*memCallback)(int, int64_t);

cache *self = NULL;
coher *coherComp = NULL;

int processorCount = 1;
int CADSS_VERBOSE = 0;
pendingQueue q = {0};

void enqueueNode(pendingRequest* node) {
   if (q.head == NULL) {
       q.head = node;
       q.tail = node;
   } 
   else {
       assert(q.tail != NULL);
       q.tail->next = node;
       q.tail = node;
   }
}

void dequeueRequest() {
    assert(q.head != NULL && q.tail != NULL);
    DPRINTF("popping from queue\n");
    pendingRequest* temp = q.head;
    if (q.head == q.tail) {
        q.tail = NULL;
    }
    q.head = q.head->next;
    DPRINTF("address of head: %p\n", q.head);
    free(temp);
}

void enqueueRequest(int64_t addr, bool isLoad,
                    reqType requestType, cacheResult cacheResult) {
    struct _pendingRequest *newReq = malloc(sizeof(struct _pendingRequest));
    // initialize newReq
    newReq->addr = addr;
    newReq->isStarted = false;
    newReq->isLoad = isLoad;
    newReq->requestType = requestType;
    newReq->cacheResult = cacheResult;
    newReq->next = NULL;
    enqueueNode(newReq);
}

void memoryRequest(trace_op *op, int processorNum, int64_t tag,
                   void (*callback)(int, int64_t));
void coherCallback(int type, int procNum, int64_t addr);

unsigned long E, s, b, i, k = 0;
unsigned long S, B = 0;
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
int load(unsigned long addr, unsigned long *evict_addr, bool evict) {
    unsigned long addr_set_index, addr_tag;
    if (s == 0) {
        addr_set_index = 0;
        addr_tag = addr >> b;
    } else {
        addr_set_index = (addr << (64UL - (s + b))) >> (64UL - s);
        addr_tag = addr >> (s + b);
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

    // Skip logic for eviction and storing in cache when evict is false
    if (!evict) {
        return 1;
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

    DPRINTF("set index: %lX\n", addr_set_index);
    if (s == 0) {
        *evict_addr = curr_set[LRU_index].tag << b;
    } else {
        *evict_addr = (curr_set[LRU_index].tag << (s + b)) + (addr_set_index << b);
    }

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
int store(unsigned long addr, unsigned long *evict_addr, bool evict) {
    unsigned long addr_set_index, addr_tag;
    if (s == 0) {
        addr_set_index = 0;
        addr_tag = addr >> b;
    } else {
        addr_set_index = (addr << (64UL - (s + b))) >> (64UL - s);
        addr_tag = addr >> (s + b);
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
      
    // Skip logic for eviction and storing in cache when evict is false
    if (!evict) {
        return 1;
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
    DPRINTF("set index: %lX\n", addr_set_index);
    // evict address: translate set index and line number to address
    if (s == 0) {
        *evict_addr = curr_set[LRU_index].tag << b;
    } else {
        *evict_addr = (curr_set[LRU_index].tag << (s + b)) + (addr_set_index << b);
    }
    DPRINTF("calculated evict address: %lX\n", *evict_addr);

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
            b = strtoul(optarg, NULL, 10);
            B = 1 << b;
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


// we only call permReq on misses, but what we consider a cache miss might not be a cache miss 
// according to coherence. in those cases we won't get a callback so we just need to advance 
// the queue manually ourselves (aka we can't wait for next tick)
// note this only happens if permReq returns 1
void handlePermReq() {
    q.head->isStarted = true;
    if (coherComp->permReq(q.head->isLoad, q.head->addr, procNum)) {
        dequeueRequest();
    }
}

// invReq's equivalent to permReq's handlePermReq
void handleInvReq() {
    q.head->isStarted = true;
    // invlReq should return 1 for P1 (aka wait)
    assert(coherComp->invlReq(q.head->addr, procNum));
}

// This routine is a linkage to the rest of the memory hierarchy
void coherCallback(int type, int procNum, int64_t addr) {
    switch (type) {
    case NO_ACTION:
        DPRINTF("** received inv callback\n");
        // dq current invreq
        dequeueRequest();
        handlePermReq();
        break;
    case DATA_RECV:
        // This indicates that the cache has received data from memory

        // check that the addr is the pending access
        assert(addr == q.head->addr);
        dequeueRequest();
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
    assert(q.head == NULL && q.tail == NULL);

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
        ;
        uint64_t addr = op->memAddress & ~(B - 1);
        res1 = op->op == MEM_LOAD ? load(addr, &evict_addr, true)
                                  : store(addr, &evict_addr, true);
        if (res1 == 1) {
            // just miss
            DPRINTF("miss, enqueued %lX\n", addr);
            enqueueRequest(addr, op->op == MEM_LOAD, PERM, MISS);
        } else if (res1 == 2) {
            // miss and evict
            DPRINTF("miss, enqueued %lX, evicting %lX\n", addr, evict_addr);

            enqueueRequest(evict_addr, op->op == MEM_LOAD, INV, MISS_EVICT);
            enqueueRequest(addr, op->op == MEM_LOAD, PERM, MISS_EVICT);
        } else if (res1 == 0) {
            DPRINTF("hit, enqueued %lX\n", addr);
            enqueueRequest(addr, op->op == MEM_LOAD, PERM, HIT);
        } else {
            assert(false);
        }

        // check if access crosses line boundary
        if (op->memAddress % B + op->size > B) {
            // access spans two lines, load the next address as well
            uint64_t next_addr = (op->memAddress + B) & ~(B - 1);
            // just send perm request but do absolutely nothing in the actual cache
            DPRINTF("second req, enqueued %lX\n", next_addr);
            enqueueRequest(next_addr, op->op == MEM_LOAD, PERM, NA);
//          res2 = op->op == MEM_LOAD ? load(next_addr, &evict_addr, false)
//                                    : store(next_addr, &evict_addr, false);
//          if (res2 == 1) {
//              // just miss
//              DPRINTF("second miss, enqueued %lX\n", next_addr);
//              enqueueRequest(next_addr, op->op == MEM_LOAD, PERM, MISS);
//          } else if (res2 == 2) {
//              // miss but evict -- shouldn't get here because 2nd cache line access of request does not evict
//              assert(false);
//          } else if (res2 == 0) {
//              DPRINTF("hit, enqueued %lX\n", addr);
//              enqueueRequest(next_addr, op->op == MEM_LOAD, PERM, HIT);
//          } else {
//              assert(false);
//          }
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

void advancePendingQueue() {
    // if queue is empty, see if callback is necessary
    if (q.head == NULL) {
        if (memCallback != NULL) {
            memCallback(procNum, globalTag);
            memCallback = NULL;
            procNum = -1;
            globalTag = -1;
        }
    }
    else if (!q.head->isStarted) {
        q.head->requestType == INV ? handleInvReq() : handlePermReq();
    }
}

int tick() {
    // Increment iteration count
    iteration++;

    // Advance ticks in the coherence component.
    coherComp->si.tick();

    advancePendingQueue();

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
