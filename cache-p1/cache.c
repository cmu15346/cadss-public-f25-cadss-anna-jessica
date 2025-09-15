#include <cache.h>
#include <trace.h>

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
int countDown = 0;

void memoryRequest(trace_op* op, int processorNum, int64_t tag,
                   void (*callback)(int, int64_t));
void coherCallback(int type, int procNum, int64_t addr);

typedef struct {
    bool valid_bit;
    bool dirty_bit;
    unsigned long tag;
    size_t LRU_counter;
} cache_line;

cache_line **main_cache = NULL;

cache* init(cache_sim_args* csa)
{
    int op;

    unsigned long E = 1;
    unsigned long S = 1;
    unsigned long B = 1;
    unsigned long i = 1;
    unsigned long k = 1;

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

    // clear the cache
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
    if (countDown != 0)
    {
        assert(pending.memCallback != NULL);
        pending.memCallback(pending.procNum, pending.tag);
    }

    pending = (pendingRequest){
        .tag = tag, .procNum = processorNum, .memCallback = callback};

    // In a real cache simulator, the delay is based
    // on whether the request is a hit or miss.
    countDown = 2;
    
    // Tell memory about this request
    // TODO: only do this if this is a miss
    // TODO: evictions will also need a call to memory with
    //  invlReq(addr, procNum) -> true if waiting, false if proceed
    coherComp->permReq(false, op->memAddress, processorNum);
}

int tick()
{
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
    return 0;
}
