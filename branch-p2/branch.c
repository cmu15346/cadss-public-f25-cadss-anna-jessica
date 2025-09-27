#include <branch.h>
#include <trace.h>

#include <assert.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#define DPRINTF(args...)                                                       \
    if (CADSS_VERBOSE) {                                                       \
        printf(args);                                                          \
    }

// branch target buffer
typedef struct BTB_entry_ {
    uint64_t tag;
    uint64_t targetAddress;
} BTB_entry;
BTB_entry **BTB = NULL;

// two-bit counters array
uint8_t *counter = NULL;

uint64_t p, s, b, g = 0;
size_t MAX_TAG = 0;
enum BRANCH_MODEL_TYPE branch_model = DEFAULT;

branch *self = NULL;

uint64_t branchRequest(trace_op *op, int processorNum);

branch *init(branch_sim_args *csa) {
    int op;

    // get argument list from assignment
    while ((op = getopt(csa->arg_count, csa->arg_list, "p:s:b:g:")) != -1) {
        switch (op) {
        // Processor count
        case 'p':
            p = strtoul(optarg, NULL, 10);
            break;

        // predictor size
        case 's':
            s = strtoul(optarg, NULL, 10);
            MAX_TAG = 1 << s;
            break;

        // BHR size
        case 'b':
            b = strtoul(optarg, NULL, 10);
            break;

        // predictor model
        case 'g':
            g = strtoul(optarg, NULL, 10);
            if (g == 0) {
                branch_model = DEFAULT;
            } else if (g == 1) {
                branch_model = GSHARE;
            } else if (g == 2) {
                branch_model = GSELECT;
            } else if (g == 3) {
                branch_model = YEH_PATT;
            } else {
                assert(false);
            }
            break;
        }
    }

    // initialize branch target buffer
    BTB = calloc(MAX_TAG, sizeof(BTB_entry *));

    // initialize pattern history table
    counter = calloc(MAX_TAG, sizeof(uint8_t));
    for (size_t i = 0; i < MAX_TAG; i++) {
        counter[i] = 1;
    }

    self = malloc(sizeof(branch));
    self->branchRequest = branchRequest;
    self->si.tick = tick;
    self->si.finish = finish;
    self->si.destroy = destroy;

    return self;
}

// default 2-bit counter prediction model
uint64_t predict_default(uint64_t pcAddress, uint64_t outcomeAddress) {
    uint64_t predAddress = pcAddress + 4;

    uint64_t tag = (pcAddress >> 3) & (MAX_TAG - 1);

    // compute prediction from PHT counter
    if (counter[tag] >= 2) {
        // predictor takes the branch
        // assert(BTB[tag] != NULL);
        predAddress = BTB[tag]->targetAddress;
    }

    DPRINTF("B(0x%lx) has predict state %d, predicting 0x%lx, actual 0x%lx\n", pcAddress, counter[tag], predAddress, outcomeAddress);

    // update PHT and BTB based on actual outcome
    if (outcomeAddress == pcAddress + 4) {
        // did not actually take branch, decrement counter
        if (counter[tag] > 0) {
            counter[tag]--;
        }
    } else {
        // actually took branch, increment counter
        if (counter[tag] < 3) {
            counter[tag]++;
        }

        // store jump address in BTB
        if (BTB[tag] == NULL) {
            BTB[tag] = malloc(sizeof(BTB_entry));
        }
        BTB[tag]->tag = tag;
        BTB[tag]->targetAddress = outcomeAddress;
    }

    return predAddress;
}

// gshare/gselect prediction model
uint64_t predict_gmodel(uint64_t pcAddress, uint64_t outcomeAddress,
                        bool isGshare) {
    uint64_t predAddress = pcAddress + 4;

    // TODO: compute 2-bit prediction based on BTB

    return predAddress;
}

// Given a branch operation, return the predicted PC address
uint64_t branchRequest(trace_op *op, int processorNum) {
    assert(op != NULL);

    uint64_t pcAddress = op->pcAddress;
    uint64_t outcomeAddress = op->nextPCAddress;
    uint64_t predAddress = op->nextPCAddress; // 100% accuracy

    // In student's simulator, either return a predicted address from BTB
    //   or pcAddress + 4 as a simplified "not taken".
    // Predictor has the actual nextPCAddress, so it knows how to update
    //   its state after computing the prediction.

    // compute branch prediction
    switch (branch_model) {
    case DEFAULT:
        predAddress = predict_default(pcAddress, outcomeAddress);
        break;

    case GSHARE:
        predAddress = predict_gmodel(pcAddress, outcomeAddress, true);
        break;

    case GSELECT:
        predAddress = predict_gmodel(pcAddress, outcomeAddress, false);
        break;

    case YEH_PATT:
        DPRINTF("Yeh-Patt model is unimplemented\n");
        break;
    }

    DPRINTF("Branch %lx -> %lx\n", pcAddress, predAddress);

    return predAddress;
}

int tick() { return 1; }

int finish(int outFd) { return 0; }

int destroy(void) {
    // free any internally allocated memory here
    for (size_t i = 0; i < MAX_TAG; i++) {
        free(BTB[i]);
    }
    free(BTB);
    free(counter);
    return 0;
}
