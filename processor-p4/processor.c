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
uint64_t counter = 1;

typedef struct reg_ {
    bool ready;
    uint32_t tag;
    uint32_t val;
    int reg_id;
} reg;

typedef struct CDB_ {
    bool busy;
    uint32_t tag;
    uint32_t val;
    int reg_id;
} CDB;

typedef struct instr_ {
    bool is_long;
    int op_typ; // -1 = normal , 0 = memory , 1 = branch
    trace_op *trace_op;
    bool fired;
    uint32_t FU;
    int dest;
    reg *src_arr[2];
    uint32_t tag;
} instr;

instr *init_instr(bool is_long, int op_typ, trace_op *op, uint32_t dest,
                  int srcs[]) {
    instr *I = calloc(1, sizeof(instr));
    I->is_long = is_long;
    I->op_typ = op_typ;
    I->trace_op = calloc(1, sizeof(trace_op));
    memcpy(I->trace_op, op, sizeof(trace_op));
    I->dest = dest;
    for (int i = 0; i < 2; i++) {
        if (srcs[i] != -1) {
            I->src_arr[i] = calloc(1, sizeof(reg));
            I->src_arr[i]->reg_id = srcs[i];
        }
    }
    I->tag = counter;
    counter++;
    return I;
}

// 33 registers
reg *regs = NULL;
// C buses
CDB *buses = NULL;
// 2d array of instr*, first J are fast, next K are long
instr ***FU_pipeline = NULL;

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
instr_queue *long_schedule_queue = NULL; // priority queue based on tag
instr_queue *fast_schedule_queue = NULL; // priority queue based on tag
instr_queue *state_update_queue = NULL;  // priority queue based on tag

trace_reader *tr = NULL;
cache *cs = NULL;
branch *bs = NULL;
processor *self = NULL;

int processorCount = 1;
int CADSS_VERBOSE = 0;

int *pendingBranch = NULL;
int *pendingMem = NULL;
int64_t *memOpTag = NULL;
int64_t globalTag = 1;

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

bool queue_delete(instr_queue *q, instr *v) {
    if (queue_empty(q))
        return false;
    instr_node *prev = NULL;
    instr_node *cur = q->head;
    while (cur != NULL && cur->instr != v) {
        prev = cur;
        cur = cur->next;
    }
    // if not found, return false
    if (cur->instr != v)
        return false;

    // completely middle case
    if (cur != q->head && cur != q->tail) {
        prev->next = cur->next;
    } else {
        if (cur == q->head) {
            q->head = cur->next;
        }
        if (cur == q->tail) {
            q->tail = prev;
            if (prev != NULL)
                prev->next = NULL;
        }
    }
    free(cur);
    q->cnt--;
    return true;
}

instr *queue_peek(instr_queue *q) {
    if (queue_empty(q)) {
        return NULL;
    }
    return q->head->instr;
}

int find_CDB_by_tag(uint32_t tag) {
    for (int i = 0; i < C; i++) {
        // busy == broadcasting
        if (buses[i].busy && buses[i].tag == tag) {
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
    // all registers init as ready b/c lmao
    for (int i = 0; i < 33; i++) {
        regs[i].ready = true;
    }
    buses = calloc(C, sizeof(CDB));
    FU_pipeline = calloc(J + K, sizeof(instr **));
    for (int i = 0; i < J + K; i++) {
        instr **instr_arr = calloc(3, sizeof(instr *));
        FU_pipeline[i] = instr_arr;
    }

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

int64_t instrCount = 0;

const int64_t STALL_TIME = 100000;
int64_t tickCount = 0;

int64_t makeTag(int procNum, int64_t baseTag) {
    return ((int64_t)procNum) | (baseTag << 8);
}

void memOpCallback(int procNum, int64_t tag) {
    DPRINTF("received memopcallback with tag %ld\n", tag);

    // Is the completed memop one that is pending?
    if (tag == memOpTag[procNum]) {
        pendingMem[procNum] = 0;
        memOpTag[procNum] = 0;
    } else {
        DPRINTF("memopTag: %ld != tag %ld\n", memOpTag[procNum], tag);
    }
    // DPRINTF("memopTag: %ld matched no FU tags\n", tag);
}

void print_stats() {
    double avg = tickCount == 0 ? 0 : (double)instrCount / (double)tickCount;
    printf("Average number of instructions fired per cycle: %f\n", avg);
    printf("Total number of instructions: %ld\n", instrCount);
    printf("Total simulation run-time in number of cycles: %ld\n", tickCount);
}

int tick(void) {
    // if room in pipeline, request op from trace
    //   for the sample processor, it requests an op
    //   each tick until it reaches a branch or memory op
    //   then it blocks on that op

    DPRINTF("\n\n-- START TICK %ld --\n\n", tickCount);

    trace_op *nextOp = NULL;

    // Pass along to the branch predictor and cache simulator that time ticked
    bs->si.tick();
    cs->si.tick();
    tickCount++;

    int progress = 0;
    for (int i = 0; i < processorCount; i++) {
        // The register file is written via the result bus. (SU g + set CDB.busy
        // to false)
        for (int j = 0; j < C; j++) {
            if (buses[j].busy) {
                if (buses[j].reg_id >= 0) {
                    if (regs[buses[j].reg_id].tag == buses[j].tag) {
                        regs[buses[j].reg_id].ready = true;
                        regs[buses[j].reg_id].val = buses[j].val;
                    }
                }
                buses[j].busy = false;
                DPRINTF("progress = 1 reg <- result bus\n");
                progress = 1;
            }
        }

        instr *completed[4] = {NULL, NULL, NULL, NULL};
        // The state update unit pulls from the state update queue and updates
        // the result bus (SU a-e)
        for (int c = 0; c < C; c++) {
            if (queue_empty(state_update_queue))
                break;
            instr *I = queue_pop(state_update_queue);
            if (I->op_typ != 1) { // ALU or mem instr
                buses[c].busy = true;
                buses[c].tag = I->tag;
                buses[c].val = 0;
                buses[c].reg_id = I->dest;
                // Scoreboard[FU].busy = false already done by setting
                // FU_pipeline[j][0] null
            }

            // mark as completed
            completed[c] = I;
            // DPRINTF("progress = 1 SU a-e, %lx\n", I->op_typ == 0
            //                                           ? I->trace_op->memAddress
            //                                           : I->trace_op->pcAddress);
            progress = 1;
        }

        // — END: STATE UPDATE LATCH —

        // — START: EXECUTE LATCH —

        // advance FU_pipeline[j] - smth should complete for progress
        for (int j = 0; j < J + K; j++) {
            instr *to_queue = NULL;
            // fast
            if (j < J) {
                to_queue = FU_pipeline[j][0];
                if (to_queue != NULL && to_queue->op_typ == 0 &&
                    pendingMem[i] != 0) {
                    DPRINTF(
                        "stalling ALU execution pipeline because pendingMem\n");
                    // stall pipeline if instr is pending memrequest
                    continue;
                }
                FU_pipeline[j][0] = NULL;
            }
            // long
            else {
                to_queue = FU_pipeline[j][2];
                FU_pipeline[j][2] = FU_pipeline[j][1];
                FU_pipeline[j][1] = FU_pipeline[j][0];
                FU_pipeline[j][0] = NULL;
            }
            // put completed instructions in state_update_queue
            //          DPRINTF("to_queue: %p\n", to_queue);
            if (to_queue != NULL) {
                // DPRINTF("priority push %lx into state update quueue\n",
                //         to_queue->op_typ == 0 ? to_queue->trace_op->memAddress
                //                               : to_queue->trace_op->pcAddress);
                // priority_push(state_update_queue, to_queue);
                DPRINTF("progress = 1 state update queue push\n");
                progress = 1;
            }
        }
        for (int j = 0; j < J + K; j++) {
            for (int k = 0; k < 3; k++) {
                // DPRINTF("FU_pipeline[%d][%d] = %p\n", j, k,
                // FU_pipeline[j][k]);
                if (FU_pipeline[j][k] != NULL) {
                    DPRINTF("progress = 1 execute pipeline moved\n");
                    progress = 1;
                }
            }
        }

        // — END: EXECUTE LATCH —

        // — START: SCHEDULE/DISPATCH LATCH —

        // schedule b: mark indep instructions in schedule queue to fire
        instr_queue *qs[2] = {long_schedule_queue, fast_schedule_queue};
        for (int j = 0; j < 2; j++) {
            instr_queue *q = qs[j];
            instr_node *cur_node = q->head;
            while (cur_node != NULL) {
                instr *RS = cur_node->instr;

                if (!RS->fired) {
                    bool ready = true;
                    for (int j = 0; j < 2; j++) {
                        assert(RS->src_arr[j] != NULL);
                        if (RS->src_arr[j] != NULL && !RS->src_arr[j]->ready)
                            ready = false;
                    }
                    if (RS->op_typ == 0 && pendingMem[i] != 0) {
                        ready = false;
                    }
                    if (ready) {
                        // find next possible FU
                        for (int j = 0; j < J + K; j++) {
                            if (j < J && RS->is_long)
                                continue;
                            if (j >= J && !RS->is_long)
                                continue;
                            if (FU_pipeline[j][0] == NULL) {
                                FU_pipeline[j][0] = RS;
                                if (RS->op_typ == 0) {
                                    // call memory request when put into FU
                                    // pipeline
                                    pendingMem[i] = 1;
                                    memOpTag[i] = makeTag(i, globalTag);
                                    cs->memoryRequest(RS->trace_op, i,
                                                      makeTag(i, globalTag),
                                                      memOpCallback);
                                    DPRINTF(
                                        "called memoryRequest with tag %ld\n",
                                        makeTag(i, globalTag));
                                    globalTag++;
                                }
                                RS->FU = j;
                                RS->fired = true;
                                DPRINTF("progress = 1 scheduling b\n");
                                progress = 1;
                                break;
                            }
                        }
                    }
                }
                cur_node = cur_node->next;
            }
        }

        // dispatch unit reserves slots in scheduling queues
        instr_node *cur_node = dispatch_queue->head;
        while (cur_node != NULL) {
            if (queue_full(long_schedule_queue) &&
                queue_full(fast_schedule_queue)) {
                DPRINTF("schedule queues full\n");
                break;
            }
            instr *cur_instr = cur_node->instr;
            instr_node *next = cur_node->next;
            // a: add I to first free slot of schedule queue
            if (cur_instr->is_long) {
                if (queue_full(long_schedule_queue)) {
                    DPRINTF("long schedule queue full\n");
                    cur_node = next;
                    continue;
                }
                DPRINTF("push %p into long schedule quueue\n", cur_instr);
                queue_push(long_schedule_queue, cur_instr);
            } else {
                if (queue_full(fast_schedule_queue)) {
                    DPRINTF("fast schedule queue full\n");
                    cur_node = next;
                    continue;
                }
                queue_push(fast_schedule_queue, cur_instr);
                // dispatch if schedule queue not full
            }
            DPRINTF("progress = 1 dispatch_queue reserve\n");
            progress = 1;
            // b: delete I from dispatch queue
            assert(queue_delete(dispatch_queue, cur_instr));
            cur_node = next;

            // e: for all src registers i of I, do:
            bool cleared =
                cur_instr->src_arr[0] == NULL || cur_instr->src_arr[1] == NULL;

            for (int i = 0; i < 2; i++) {
                if (cur_instr->src_arr[i] == NULL) {
                    cur_instr->src_arr[i] = calloc(1, sizeof(reg));
                    cur_instr->src_arr[i]->reg_id = -1;
                }

                reg *src = cur_instr->src_arr[i];
                assert(src != NULL);

                if (src->reg_id == -1 || regs[src->reg_id].ready) {
                    cur_instr->src_arr[i]->val = regs[src->reg_id].val;
                    cur_instr->src_arr[i]->ready = true;
                } else {
                    cur_instr->src_arr[i]->tag = regs[src->reg_id].tag;
                    cur_instr->src_arr[i]->ready = false;
                }
            }

            // f-g: tag the destination -- if it is a register
            if (cur_instr->dest >= 0) {
                regs[cur_instr->dest].tag = cur_instr->tag;
                // h: set register unready, will be ready once execution
                // completes
                regs[cur_instr->dest].ready = false;
            }
        }

        // schedule a: scheduling queues updated from result buses
        for (int j = 0; j < 2; j++) {
            instr_queue *q = qs[j];
            cur_node = q->head;
            while (cur_node != NULL) {
                instr *RS = cur_node->instr;

                for (int i = 0; i < 2; i++) {
                    reg *src = RS->src_arr[i];

                    // bc -1 means both srcs are 0 .... ;-;
                    assert(src != NULL);

                    if (!src->ready) {
                        int CDB = find_CDB_by_tag(src->tag);
                        if (CDB != -1) {
                            src->ready = true;
                            src->val = buses[CDB].val;
                            DPRINTF("progress = 1 scheduling a\n");
                            progress = 1;
                        }
                    }
                }

                cur_node = cur_node->next;
            }
        }

        // — END: SCHEDULE/DISPATCH LATCH —

        // — START: I F/D LATCH —
        for (uint64_t f = 0; f < F; f++) {
            if (pendingBranch[i] == 1) {
                // if branch was mispredicted, stall
                DPRINTF("branch stall\n");
                break;
            }
            if (queue_full(dispatch_queue)) {
                // if dispatch queue is full, halts
                break;
            }

            // get and manage ops for each processor core
            nextOp = tr->getNextOp(i);

            if (nextOp == NULL)
                continue;

            DPRINTF("progress = 1 instruction\n");
            progress = 1;
            instrCount++;

            instr *new_instr;

            switch (nextOp->op) {
            case MEM_LOAD:
            case MEM_STORE:
                // DPRINTF("fetched memory instruction (0x%lx)\n",
                // nextOp->memAddress);
                new_instr = init_instr(false, 0, nextOp, nextOp->dest_reg,
                                       nextOp->src_reg);
                DPRINTF("push M %lx into dispatch queue\n", nextOp->memAddress);
                assert(queue_push(dispatch_queue, new_instr));
                break;

            case BRANCH:
                // DPRINTF("fetched a branch instruction (0x%lx)\n",
                // nextOp->pcAddress);
                pendingBranch[i] =
                    (bs->branchRequest(nextOp, i) == nextOp->nextPCAddress) ? 0
                                                                            : 1;
                new_instr = init_instr(false, 1, nextOp, nextOp->dest_reg,
                                       nextOp->src_reg);
                // DPRINTF("push B %lx into dispatch queue\n", nextOp->pcAddress);
                assert(queue_push(dispatch_queue, new_instr));
                break;

            case ALU:
            case ALU_LONG:;
                // DPRINTF("fetched an ALU instruction (0x%lx)\n",
                // nextOp->pcAddress);
                new_instr = init_instr(nextOp->op == ALU_LONG, -1, nextOp,
                                       nextOp->dest_reg, nextOp->src_reg);
                // DPRINTF("push A %lx into dispatch queue\n", nextOp->pcAddress);
                assert(queue_push(dispatch_queue, new_instr));
                //              queue_print("dispatch_queue", dispatch_queue);
                break;
            }

            free(nextOp);
        }
        // — END: I F/D LATCH —

        // — START: STATE UPDATE LATCH —

        // The state update unit deletes completed instructions
        // from the scheduling queue. (SU f)
        for (int c = 0; c < C; c++) {
            instr *del_instr = completed[c];
            if (del_instr != NULL) {
                DPRINTF("progress = 1 su f\n");
                progress = 1;

                // unstall on any completed branch instruction
                if (del_instr->op_typ == 1) {
                    pendingBranch[i] = 0;
                }

                instr_queue *q = del_instr->is_long ? long_schedule_queue
                                                    : fast_schedule_queue;
                assert(queue_delete(q, del_instr));
                free(del_instr);
            }
        }
    }

    for (int i = 0; i < processorCount; i++) {
        if (pendingMem[i] != 0) {
            progress = 1;
        }
    }

    if (progress == 0) {
        print_stats();
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

    free(pendingMem);
    free(pendingBranch);
    free(memOpTag);

    int c = cs->si.destroy();
    int b = bs->si.destroy();

    if (b || c)
        return 1;
    return 0;
}
