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
typedef struct _memRequest {
    pendingRequest* head;
    pendingRequest* tail;
    void (*memCallback)(int, int64_t);
    int64_t requestTag;
    struct _memRequest *next;
} memRequest;

typedef struct _requestQueue {
    memRequest* head;
    memRequest* tail;
} requestQueue;

// int64_t globalTag = 0;
int8_t procNum = 0;

// void (*memCallback)(int, int64_t);

cache *self = NULL;
coher *coherComp = NULL;

int processorCount = 1;
int CADSS_VERBOSE = 0;
// memRequest q = {0};

requestQueue memReqQueue = {0};

// pushes a new pending request (PERM/INV) to the end of a single memory request's queue
void enqueueNode(memRequest* req, pendingRequest* node) {
   if (req->head == NULL) {
       req->head = node;
       req->tail = node;
   } 
   else {
       assert(req->tail != NULL);
       req->tail->next = node;
       req->tail = node;
   }
}

// pops the pending request (PERM/INV) from the front of a single memory request's queue
void dequeuePendingRequest(memRequest* req) {
    assert(req->head != NULL && req->tail != NULL);

    DPRINTF("popping from queue\n");
    pendingRequest* temp = req->head;
    if (req->head == req->tail) {
        req->tail = NULL;
    }
    req->head = req->head->next;
    DPRINTF("address of head: %p\n", req->head);
    free(temp);
}

// given pending request (PERM/INV) fields, create the pending request and enqueue it to the
// given memory request's queue
void enqueuePendingRequest(memRequest* req, int64_t addr, bool isLoad,
                    reqType requestType, cacheResult cacheResult) {
    struct _pendingRequest *newReq = malloc(sizeof(struct _pendingRequest));
    // initialize newReq
    newReq->addr = addr;
    newReq->isStarted = false;
    newReq->isLoad = isLoad;
    newReq->requestType = requestType;
    newReq->cacheResult = cacheResult;
    newReq->next = NULL;
    enqueueNode(req, newReq);
}

memRequest *enqueueMemRequest(void (*memCallback)(int, int64_t), int64_t requestTag) {
    memRequest *memReq = malloc(sizeof(struct _memRequest));
    memReq->head = NULL;
    memReq->tail = NULL;
    memReq->memCallback = memCallback;
    memReq->requestTag = requestTag;
    memReq->next = NULL;

    if (memReqQueue.head == NULL) {
        memReqQueue.head = memReq;
       memReqQueue.tail = memReq;
    } else {
        assert(memReqQueue.tail != NULL);
       memReqQueue.tail->next = memReq;
       memReqQueue.tail = memReq;
    }

    assert(memReqQueue.head != NULL && memReqQueue.tail != NULL);
    return memReq;
}

void dequeueMemRequest(memRequest* memReq) {
    assert(memReqQueue.head != NULL && memReqQueue.tail != NULL);
    DPRINTF("popping from queue\n");
    memRequest* temp = memReqQueue.head;
    if (memReqQueue.head == memReqQueue.tail) {
        memReqQueue.tail = NULL;
    }
    memReqQueue.head = memReqQueue.head->next;
    DPRINTF("address of head: %p\n", memReqQueue.head);
    free(temp);
}

void memoryRequest(trace_op *op, int processorNum, int64_t tag,
                   void (*callback)(int, int64_t));
void coherCallback(int type, int procNum, int64_t addr);

unsigned long E, s, b, victim_i, k = 0;
unsigned long S, B, R = 0;
unsigned long iteration = 0; // timestamp used for LRU
bool is_rrip = false;

typedef struct {
    bool valid_bit;
    bool dirty_bit;
    unsigned long tag;
    size_t LRU_counter;
    size_t RRPV;
} cache_line;

cache_line **main_cache = NULL;
// 1d array of cache_lines
cache_line *victim_cache = NULL;

void assert_set_all_valid(cache_line* set, size_t E) {
    for (unsigned long line_index = 0; line_index < E; line_index++) {
        assert(set[line_index].valid_bit);
    }
}

size_t find_evict(cache_line* set, size_t E, bool evict_rrip) {
    // need to evict from MAIN
    unsigned long evict_index = 0;
    if (evict_rrip) {
        assert_set_all_valid(set, E);
        // find first index predicted to be accessed in distant future
        bool found_evict = false;
        while (!found_evict) {
            for (size_t line_index = 0; line_index < E; line_index++) {
                assert(set[line_index].RRPV <= R);
                if (set[line_index].RRPV == R) {
                    found_evict = true;
                    evict_index = line_index;
                    break;
                }
            }
            if (!found_evict) {
                for (size_t line_index = 0; line_index < E; line_index++) {
                    set[line_index].RRPV++;
                }
            }
        }
    } else {
        unsigned long smallest_LRU = set[0].LRU_counter;
        for (unsigned long line_index = 1; line_index < E; line_index++) {
            if (set[line_index].LRU_counter < smallest_LRU) {
                smallest_LRU = set[line_index].LRU_counter;
                evict_index = line_index;
            }
        }
    }
    return evict_index;
}

int cache_access(unsigned long addr, unsigned long *evict_addr, bool is_store) {
    unsigned long addr_set_index, addr_tag;
    addr_tag = addr >> (s + b);
    addr_set_index = (addr << (64UL - (s + b))) >> (64UL - s);

    cache_line *curr_set = main_cache[addr_set_index];
    

    // Look for MAIN hit
    for (unsigned long line_index = 0; line_index < E; line_index++) {
        if ((curr_set[line_index].tag == addr_tag) &&
            curr_set[line_index].valid_bit) {
            // Found tag + valid bit YAY, hit!!
            // update LRU_counter
            curr_set[line_index].LRU_counter = iteration;
            // update dirty bit
            if (is_store) curr_set[line_index].dirty_bit = true;
            // update RRPV
            curr_set[line_index].RRPV = 0;
            return 0; // MAIN HIT
        }
    }

    // can freely bring into MAIN
    for (unsigned long line_index = 0; line_index < E; line_index++) {
        if (!curr_set[line_index].valid_bit) {
            // Found invalid bit? - YAY miss
            // update curr_set[line_index] with values....
            curr_set[line_index].valid_bit = true;
            curr_set[line_index].dirty_bit = is_store;
            curr_set[line_index].tag = addr_tag;
            curr_set[line_index].LRU_counter = iteration;
            curr_set[line_index].RRPV = R - 1;
            return 1; // MAIN MISS, VICTIM MISS, no overall evict
        }
    }

    // need to evict from MAIN
    unsigned long evict_index = find_evict(curr_set, E, is_rrip);

    DPRINTF("set index: %lX\n", addr_set_index);
    *evict_addr = (curr_set[evict_index].tag << (s + b)) + (addr_set_index << b);

    DPRINTF("calculated evict address: 0x%lX\n", *evict_addr);

    // Overwrite the line - YAY miss, evict
    // Update entry
    // If that index has dirty bits - we are evicting dirty bits!
    // since loading, we set dirty bit to false
    curr_set[evict_index].valid_bit = true;
    curr_set[evict_index].dirty_bit = is_store;
    // update tag and evict_counter
    curr_set[evict_index].tag = addr_tag;
    curr_set[evict_index].LRU_counter = iteration;
    curr_set[evict_index].RRPV = R - 1;

    return 2; // MISS and EVICT
}

int cache_access_victim(unsigned long addr, unsigned long *evict_addr, bool is_store) {
    unsigned long addr_set_index, addr_tag, victim_addr_tag;
    addr_tag = addr >> (s + b);
    victim_addr_tag = addr >> b;
    addr_set_index = (addr << (64UL - (s + b))) >> (64UL - s);

    cache_line *curr_set = main_cache[addr_set_index];

    // Look for MAIN hit
    for (unsigned long line_index = 0; line_index < E; line_index++) {
        if ((curr_set[line_index].tag == addr_tag) &&
            curr_set[line_index].valid_bit) {
            // Found tag + valid bit YAY, hit!!
            // update LRU_counter
            curr_set[line_index].LRU_counter = iteration;
            // update dirty bit
            if (is_store) curr_set[line_index].dirty_bit = true;
            // update RRPV
            curr_set[line_index].RRPV = 0;
            return 0; // MAIN HIT
        }
    }

    // MAIN miss, Look for VICTIM HIT
    for (unsigned long vic_line_index = 0; vic_line_index < victim_i; vic_line_index++) {
        if ((victim_cache[vic_line_index].tag == victim_addr_tag) &&
            victim_cache[vic_line_index].valid_bit) {
            // Found tag + valid bit YAY, victim hit!!
            // update LRU_counter
            victim_cache[vic_line_index].LRU_counter = iteration;
            // update dirty bit
            if (is_store) victim_cache[vic_line_index].dirty_bit = true;
            // update RRPV
            victim_cache[vic_line_index].RRPV = 0;

            // swap into main cache (guaranteed corresponding main cache set is full)
            assert_set_all_valid(curr_set, E);

            // Find evict index in main cache set
            unsigned long evict_index = find_evict(curr_set, E, is_rrip); 

            // make new tags as we are switching cache configurations
            unsigned long lru_to_victim_tag = (curr_set[evict_index].tag << s) + addr_set_index;
            unsigned long victim_to_lru_tag = addr_tag;

            // swap
            cache_line temp = {0};

            memcpy(&temp, &victim_cache[vic_line_index], sizeof(cache_line));
            memcpy(&victim_cache[vic_line_index], &curr_set[evict_index], 
                    sizeof(cache_line));
            memcpy(&curr_set[evict_index], &temp, sizeof(cache_line));

            victim_cache[vic_line_index].tag = lru_to_victim_tag;
            curr_set[evict_index].tag = victim_to_lru_tag;

            return 0; // MAIN MISS, VICTIM HIT
        }
    }

    // MAIN MISS, VICTIM MISS

    // can freely bring into MAIN
    for (unsigned long line_index = 0; line_index < E; line_index++) {
        if (!curr_set[line_index].valid_bit) {
            // Found invalid bit? - YAY miss
            // update curr_set[line_index] with values....
            curr_set[line_index].valid_bit = true;
            curr_set[line_index].dirty_bit = is_store;
            curr_set[line_index].tag = addr_tag;
            curr_set[line_index].LRU_counter = iteration;
            curr_set[line_index].RRPV = R - 1;
            return 1; // MAIN MISS, VICTIM MISS, no overall evict
        }
    }

    // now we are handling main miss, victim miss, evict from main cache
    // 2 subcases: victim cache has room OR victim cache needs to evict
    // if victim cache has room, then no address is reported as evict
    // if victim cache needs to evict, then evict address is reported

    // need to evict from MAIN
    unsigned long evict_index = find_evict(curr_set, E, is_rrip); 

    // can freely bring into VICTIM
    for (unsigned long vic_line_index = 0; vic_line_index < victim_i; vic_line_index++) {
        if (!victim_cache[vic_line_index].valid_bit) {
            // make new tags as we are switching cache configurations
            unsigned long lru_to_victim_tag = (curr_set[evict_index].tag << s) + 
                                                (addr_set_index);

            // main LRU evict -> victim free spot
            victim_cache[vic_line_index].valid_bit = true;
            victim_cache[vic_line_index].dirty_bit = curr_set[evict_index].dirty_bit;
            victim_cache[vic_line_index].tag = lru_to_victim_tag;
            victim_cache[vic_line_index].LRU_counter = curr_set[evict_index].LRU_counter;
            victim_cache[vic_line_index].RRPV = curr_set[evict_index].RRPV;

            // new address -> main
            curr_set[evict_index].valid_bit = true;
            curr_set[evict_index].dirty_bit = is_store;
            curr_set[evict_index].tag = addr_tag;
            curr_set[evict_index].LRU_counter = iteration;
            curr_set[evict_index].RRPV = R - 1;
            return 1; // MAIN MISS, VICTIM MISS, EVICT FROM MAIN TO VICTIM CACHE, 
                      // no overall evict, just permreq
        }
    }

    // evict from VICTIM
    unsigned long vic_LRU_index = find_evict(victim_cache, victim_i, false);

    // victim cache doesn't have room, evict victim LRU + replace w/ new address info
    // evict victim LRU -- set evict_addr = victim tag << b
    // main LRU -> victim LRU -- from before
    // new address -> main cache -- from before
    
    *evict_addr = victim_cache[vic_LRU_index].tag << b;

    // make new tags as we are switching cache configurations
    unsigned long lru_to_victim_tag = (curr_set[evict_index].tag << s) + 
                                        addr_set_index;

    // main LRU evict -> victim LRU spot 
    victim_cache[vic_LRU_index].valid_bit = true;
    victim_cache[vic_LRU_index].dirty_bit = curr_set[evict_index].dirty_bit;
    victim_cache[vic_LRU_index].tag = lru_to_victim_tag;
    victim_cache[vic_LRU_index].LRU_counter = curr_set[evict_index].LRU_counter;
    victim_cache[vic_LRU_index].RRPV = curr_set[evict_index].RRPV;

    // new address -> main
    curr_set[evict_index].valid_bit = true;
    curr_set[evict_index].dirty_bit = is_store;
    curr_set[evict_index].tag = addr_tag;
    curr_set[evict_index].LRU_counter = iteration;
    curr_set[evict_index].RRPV = R - 1;

    return 2; // BOTH MISS, EVICT
}

/**
 * @brief Simulates a load into the cache
 * Very similar to store, but the two functions kept separate for clarity
 *
 * @param[in]     addr         Address we are reading from
 *
 * Updates cache with result of load operation using a given address
 */
int load(unsigned long addr, unsigned long *evict_addr) {
    return victim_i > 0 ? cache_access_victim(addr, evict_addr, false) : cache_access(addr, evict_addr, false);
}

/**
 * @brief Simulates a store into the cache
 *
 * @param[in]     addr         Address we are writing to
 *
 * Updates cache with result of store operation using a given address
 */
int store(unsigned long addr, unsigned long *evict_addr) {
    return victim_i > 0 ? cache_access_victim(addr, evict_addr, true) : cache_access(addr, evict_addr, true);
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
            victim_i = strtoul(optarg, NULL, 10);
            break;

        // bits in a RRIP-based replacement policy
        case 'R':
            k = strtoul(optarg, NULL, 10);
            if (k != 0) {
                is_rrip = true;
                R = (1UL << k) - 1;
            }
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
    
    // create victim cache -- i lines
    victim_cache = (cache_line *)calloc(victim_i, sizeof(cache_line));

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
    memRequest *q = memReqQueue.head;
    q->head->isStarted = true;
    if (coherComp->permReq(q->head->isLoad, q->head->addr, procNum)) {
        dequeuePendingRequest(q);
    }
}

// invReq's equivalent to permReq's handlePermReq
void handleInvReq() {
    memRequest *q = memReqQueue.head;
    q->head->isStarted = true;
    // invlReq should return 1 for P1 (aka wait)
    assert(coherComp->invlReq(q->head->addr, procNum));
}

// This routine is a linkage to the rest of the memory hierarchy
void coherCallback(int type, int procNum, int64_t addr) {
    memRequest *q = memReqQueue.head;
    switch (type) {
    case NO_ACTION:
        DPRINTF("** received inv callback\n");
        // dq current invreq
        dequeuePendingRequest(q);
        handlePermReq();
        break;
    case DATA_RECV:
        // This indicates that the cache has received data from memory

        // check that the addr is the pending access
        assert(addr == q->head->addr);
        dequeuePendingRequest(q);
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

    DPRINTF("called memoryReq\n");

    // Simple model to only have one outstanding memory operation
    // if (countDown != 0)
    // {
    //     assert(pending.memCallback != NULL);
    //     pending.memCallback(pending.procNum, pending.tag);
    // }
    memRequest *memReq = enqueueMemRequest(callback, tag);
    assert(memReqQueue.head != NULL && memReqQueue.tail != NULL);
    assert(memReq->head == NULL && memReq->tail == NULL);

    // In a real cache simulator, the delay is based
    // on whether the request is a hit or miss.
    // globalTag = tag;
    procNum = processorNum;
    // memCallback = callback;

    int res1, res2 = 0;
    unsigned long evict_addr;

    switch (op->op) {
    case MEM_LOAD:
    case MEM_STORE:
        // load first address
        ;
        uint64_t addr = op->memAddress & ~(B - 1);
        res1 = op->op == MEM_LOAD ? load(addr, &evict_addr)
                                  : store(addr, &evict_addr);
        if (res1 == 1) {
            // just miss
            DPRINTF("miss, enqueued %lX\n", addr);
            enqueuePendingRequest(memReq, addr, op->op == MEM_LOAD, PERM, MISS);
        } else if (res1 == 2) {
            // miss and evict
            DPRINTF("miss, enqueued %lX, evicting %lX\n", addr, evict_addr);

            enqueuePendingRequest(memReq, evict_addr, op->op == MEM_LOAD, INV, MISS_EVICT);
            enqueuePendingRequest(memReq, addr, op->op == MEM_LOAD, PERM, MISS_EVICT);
        } else if (res1 == 0) {
            DPRINTF("hit, enqueued %lX\n", addr);
            enqueuePendingRequest(memReq, addr, op->op == MEM_LOAD, PERM, HIT);
        } else {
            assert(false);
        }

        // check if access crosses line boundary
        if (op->memAddress % B + op->size > B) {
            // access spans two lines, load the next address as well
            uint64_t next_addr = (op->memAddress + B) & ~(B - 1);
            // if s==0, just send perm request, but don't do anything in the cache because yes...
            if (s == 0) {
                DPRINTF("second req, enqueued %lX\n", next_addr);
                enqueuePendingRequest(memReq, next_addr, op->op == MEM_LOAD, PERM, NA);
                break;
            }
            
            res2 = op->op == MEM_LOAD ? load(next_addr, &evict_addr)
                                      : store(next_addr, &evict_addr);
            if (res2 == 1) {
                // just miss
                DPRINTF("second miss, enqueued %lX\n", next_addr);
                enqueuePendingRequest(memReq, next_addr, op->op == MEM_LOAD, PERM, MISS);
            } else if (res2 == 2) {
                // miss and evict
                DPRINTF("second miss, enqueued %lX, evicting %lX\n", next_addr, evict_addr);

                enqueuePendingRequest(memReq, evict_addr, op->op == MEM_LOAD, INV, MISS_EVICT);
                enqueuePendingRequest(memReq, next_addr, op->op == MEM_LOAD, PERM, MISS_EVICT);
            } else if (res2 == 0) {
                DPRINTF("second hit, enqueued %lX\n", next_addr);
                enqueuePendingRequest(memReq, next_addr, op->op == MEM_LOAD, PERM, HIT);
            } else {
                assert(false);
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
}

void advanceQueue() {
    memRequest *q = memReqQueue.head;
    if (q == NULL) {
        return;
    }
    // if queue is empty, see if callback is necessary
    if (q->head == NULL) {
        if (q->memCallback != NULL) {
            // printf("cache called mem callback\n");
            q->memCallback(procNum, q->requestTag);
            memReqQueue.head = q->next; // moves on to the next memory request
            free(q);
        }
    }
    else if (!q->head->isStarted) {
        q->head->requestType == INV ? handleInvReq() : handlePermReq();
    }
}

int tick() {
    // Increment iteration count
    iteration++;

    // Advance ticks in the coherence component.
    coherComp->si.tick();

    advanceQueue();

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
    free(victim_cache);

    return 0;
}
