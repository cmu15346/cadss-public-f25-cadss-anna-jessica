#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "branch.h"
#include "cache.h"
#include "processor.h"
#include "trace.h"

#define DPRINTF(args...)                                                       \
    if (CADSS_VERBOSE) {                                                       \
        printf(args);                                                          \
    }

// command-line args
uint64_t F, D, M, J, K, C = 0;

// unique tag counter
uint64_t counter = 0;

typedef struct reg_ {
    bool ready;
    uint32_t tag;
    uint32_t val;
    uint32_t reg_id;
} reg;

typedef struct CDB_ {
    bool busy;
    uint32_t tag;
    uint32_t val;
    uint32_t reg_id;
} CDB;

typedef struct instr_ {
    bool is_long;
    bool fired;
    uint32_t FU;
    uint32_t dest;
    reg *src_arr[2];
    uint32_t tag;
} instr;

instr *init_instr(bool is_long, uint32_t dest, int srcs[]) {
    instr *I = calloc(1, sizeof(instr));
    I->is_long = is_long;
    I->dest = dest;
    for (int i = 0; i < 2; i++) {
        if (srcs[i] != -1) {
            I->src_arr[i] = calloc(1, sizeof(reg));
            I->src_arr[i]->reg_id = srcs[i];
        }
    }
    return I;
}

reg *regs = NULL;
CDB *buses = NULL;
instr **FU_pipeline = NULL;

typedef struct instr_node_ {
    instr *instr;
    struct instr_node_ *next;
} instr_node;

typedef struct instr_queue_ {
    instr_node *head;
    instr_node *tail;
    uint32_t cnt;
    uint32_t cap;
} instr_queue;

instr_queue *init_queue(int cap) {
    instr_queue *Q = malloc(sizeof(instr_queue));
    Q->head = NULL;
    Q->tail = NULL;
    Q->cnt = 0;
    Q->cap = cap;
    return Q;
}

instr_queue *decode_queue = NULL;
instr_queue *dispatch_queue = NULL;
instr_queue *long_schedule_queue = NULL;
instr_queue *fast_schedule_queue = NULL;
instr_queue *state_update_queue = NULL; // priority queue based on tag

trace_reader *tr = NULL;
cache *cs = NULL;
branch *bs = NULL;
processor *self = NULL;

int processorCount = 1;
int CADSS_VERBOSE = 0;

int *pendingMem = NULL;
int *pendingBranch = NULL;
int64_t *memOpTag = NULL;

bool queue_full(instr_queue *q) { return q->cnt == q->cap; }

bool queue_empty(instr_queue *q) { return q->cnt == 0; }

bool priority_push(instr_queue *q, instr *v) {
    if (queue_full(q)) {
        return false;
    }

    instr_node *node = malloc(sizeof(instr_node));
    node->instr = v;
    node->next = NULL;

    if (q->head == NULL) {
        // empty queue
        q->head = node;
        q->cnt++;
        return true;
    }

    instr_node *cur = q->head;
    instr_node *prev = NULL;
    while (cur != NULL && v->tag >= cur->instr->tag) {
        prev = cur;
        cur = cur->next;
    }

    if (prev == NULL) {
        node->next = q->head;
        q->head = node;
    } else {
        node->next = prev->next;
        prev->next = node;
    }

    q->cnt++;
    return true;
}

bool queue_push(instr_queue *q, instr *v) {
    if (queue_full(q)) {
        return false;
    }

    instr_node *node = malloc(sizeof(instr_node));
    node->instr = v;
    node->next = NULL;

    if (q->tail) {
        q->tail->next = node;
    } else {
        q->head = node;
    }

    q->tail = node;
    q->cnt++;
    return true;
}

instr *queue_pop(instr_queue *q) {
    if (queue_empty(q)) {
        return NULL;
    }

    instr_node *node = q->head;
    instr *ret = node->instr;

    q->head = node->next;
    if (!q->head) {
        q->tail = NULL;
    }

    free(node);
    q->cnt--;
    return ret;
}

instr *queue_peek(instr_queue *q) {
    if (queue_empty(q)) {
        return NULL;
    }
    return q->head->instr;
}

int find_CDB_by_tag(uint32_t tag) {
    for (int i = 0; i < C; i++) {
        if (buses[i].tag == tag) {
            return i;
        }
    }
    return -1;
}

//
// init
//
//   Parse arguments and initialize the processor simulator components
//
processor *init(processor_sim_args *psa) {
    int op;

    tr = psa->tr;
    cs = psa->cache_sim;
    bs = psa->branch_sim;

    while ((op = getopt(psa->arg_count, psa->arg_list, "f:d:m:j:k:c:")) != -1) {
        switch (op) {
        // fetch rate
        case 'f':
            F = strtoul(optarg, NULL, 10);
            break;

        // dispatch queue multiplier
        case 'd':
            D = strtoul(optarg, NULL, 10);
            break;

        // Schedule queue multiplier
        case 'm':
            M = strtoul(optarg, NULL, 10);
            break;

        // Number of fast ALUs
        case 'j':
            J = strtoul(optarg, NULL, 10);
            break;

        // Number of long ALUs
        case 'k':
            K = strtoul(optarg, NULL, 10);
            break;

        // Number of CDBs
        case 'c':
            C = strtoul(optarg, NULL, 10);
            break;
        }
    }

    regs = calloc(33, sizeof(reg));
    buses = calloc(C, sizeof(CDB));
    FU_pipeline = calloc(3 * (J + K), sizeof(instr *));

    decode_queue = init_queue(UINT32_MAX);
    dispatch_queue = init_queue(D * (M * J + M * K));
    long_schedule_queue = init_queue(M * K);
    fast_schedule_queue = init_queue(M * J);
    state_update_queue = init_queue(UINT32_MAX);

    pendingBranch = calloc(processorCount, sizeof(int));
    pendingMem = calloc(processorCount, sizeof(int));
    memOpTag = calloc(processorCount, sizeof(int64_t));

    self = calloc(1, sizeof(processor));
    return self;
}

const int64_t STALL_TIME = 100000;
int64_t tickCount = 0;
int64_t stallCount = -1;

int64_t makeTag(int procNum, int64_t baseTag) {
    return ((int64_t)procNum) | (baseTag << 8);
}

void memOpCallback(int procNum, int64_t tag) {
    int64_t baseTag = (tag >> 8);

    // Is the completed memop one that is pending?
    if (baseTag == memOpTag[procNum]) {
        memOpTag[procNum]++;
        pendingMem[procNum] = 0;
        stallCount = tickCount + STALL_TIME;
    } else {
        printf("memopTag: %ld != tag %ld\n", memOpTag[procNum], tag);
    }
}

int tick(void) {
    // if room in pipeline, request op from trace
    //   for the sample processor, it requests an op
    //   each tick until it reaches a branch or memory op
    //   then it blocks on that op

    trace_op *nextOp = NULL;

    // Pass along to the branch predictor and cache simulator that time ticked
    bs->si.tick();
    cs->si.tick();
    tickCount++;

    if (tickCount == stallCount) {
        printf("Processor may be stalled.  Now at tick - %ld, last op at %ld\n",
               tickCount, tickCount - STALL_TIME);
        for (int i = 0; i < processorCount; i++) {
            if (pendingMem[i] == 1) {
                printf("Processor %d is waiting on memory\n", i);
            }
        }
    }

    int progress = 0;
    for (int i = 0; i < processorCount; i++) {
        if (pendingMem[i] == 1) {
            progress = 1;
            continue;
        }

        // In the full processor simulator, the branch is pending until
        //   it has executed.
        if (pendingBranch[i] > 0) {
            pendingBranch[i]--;
            progress = 1;
            continue;
        }

        // schedule b: mark indep instructions in schedule queue to fire
        instr_node *cur_node = long_schedule_queue->head;
        while (cur_node != NULL) {
            instr *RS = cur_node->instr;

            // TODO: wakeup schedule b

            cur_node = cur_node->next;
        }

        // instruction move (decode_queue -> dispatch_queue)
        int fetch_cnt = 0;
        while (!queue_empty(decode_queue) && !queue_full(dispatch_queue) &&
               fetch_cnt < F) {
            instr *decoded = queue_pop(decode_queue);
            queue_push(dispatch_queue, decoded);
            fetch_cnt++;
            progress = 1;
        }

        // dispatch unit reserves slots in scheduling queues
        while (!queue_empty(dispatch_queue)) {
            instr *cur_instr = queue_peek(dispatch_queue);
            // a: add I to first free slot of schedule queue
            if (cur_instr->is_long) {
                if (queue_full(long_schedule_queue))
                    break;
                // dispatch if schedule queue not full
                queue_push(long_schedule_queue, cur_instr);
            } else {
                if (queue_full(fast_schedule_queue))
                    break;
                // dispatch if schedule queue not full
                queue_push(fast_schedule_queue, cur_instr);
            }
            // b: delete I from dispatch queue
            queue_pop(dispatch_queue);

            // TODO: do we need to move e-f into its own loop?
            // e: for all src registers i of I, do:
            for (int i = 0; i < 2; i++) {
                reg *src = cur_instr->src_arr[i];

                if (src == NULL)
                    continue;

                if (regs[src->reg_id].ready) {
                    cur_instr->src_arr[i]->val = regs[src->reg_id].val;
                    cur_instr->src_arr[i]->ready = true;
                } else {
                    cur_instr->src_arr[i]->tag = regs[src->reg_id].tag;
                    cur_instr->src_arr[i]->ready = false;
                }
            }

            // f-g: tag the destination
            regs[cur_instr->dest].tag = counter;
            cur_instr->tag = regs[cur_instr->dest].tag;
            counter++;
            // h: set register unready, will be ready once execution completes
            regs[cur_instr->dest].ready = false;

            progress = 1;
        }

        // instruction fetch (read into decode queue)
        for (uint64_t f = 0; f < F; f++) {

            // get and manage ops for each processor core
            nextOp = tr->getNextOp(i);

            if (nextOp == NULL)
                continue;

            progress = 1;

            switch (nextOp->op) {
            case MEM_LOAD:
            case MEM_STORE:
                pendingMem[i] = 1;
                cs->memoryRequest(nextOp, i, makeTag(i, memOpTag[i]),
                                  memOpCallback);
                break;

            case BRANCH:
                pendingBranch[i] =
                    (bs->branchRequest(nextOp, i) == nextOp->nextPCAddress) ? 0
                                                                            : 1;
                break;

            case ALU:
            case ALU_LONG:;
                instr *new_instr = init_instr(
                    nextOp->op == ALU_LONG, nextOp->dest_reg, nextOp->src_reg);
                queue_push(decode_queue, new_instr);

                break;
            }

            free(nextOp);
        }

        // schedule a: scheduling queues updated from result buses
        cur_node = long_schedule_queue->head;
        while (cur_node != NULL) {
            instr *RS = cur_node->instr;

            for (int i = 0; i < 2; i++) {
                reg *src = RS->src_arr[i];

                if (src == NULL)
                    continue;

                int CDB = find_CDB_by_tag(src->tag);
                if (CDB != -1) {
                    src->ready = true;
                    src->val = buses[CDB].val;
                }
            }

            cur_node = cur_node->next;
        }
    }

    return progress;
}

int finish(int outFd) {
    int c = cs->si.finish(outFd);
    int b = bs->si.finish(outFd);

    char buf[32];
    size_t charCount = snprintf(buf, 32, "Ticks - %ld\n", tickCount);

    (void)!write(outFd, buf, charCount + 1);

    if (b || c)
        return 1;
    return 0;
}

int destroy(void) {
    free(regs);
    free(buses);
    free(FU_pipeline);

    free(decode_queue);
    free(dispatch_queue);
    free(long_schedule_queue);
    free(fast_schedule_queue);
    free(state_update_queue);

    int c = cs->si.destroy();
    int b = bs->si.destroy();

    if (b || c)
        return 1;
    return 0;
}
