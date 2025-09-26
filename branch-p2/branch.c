#include <branch.h>
#include <trace.h>

#include <assert.h>
#include <getopt.h>
#include <stdlib.h>

unsigned long p, s, b, g = 0;

branch *self = NULL;

uint64_t branchRequest(trace_op *op, int processorNum);

branch *init(branch_sim_args *csa) {
    int op;

    // TODO - get argument list from assignment
    while ((op = getopt(csa->arg_count, csa->arg_list, "p:s:b:g:")) != -1) {
        switch (op) {
        // Processor count
        case 'p':
            p = strtoul(optarg, NULL, 10);
            break;

            // predictor size
        case 's':
            s = strtoul(optarg, NULL, 10);
            break;

            // BHR size
        case 'b':
            b = strtoul(optarg, NULL, 10);
            break;
            // predictor model
        case 'g':
            g = strtoul(optarg, NULL, 10);
            break;
        }
    }

    self = malloc(sizeof(branch));
    self->branchRequest = branchRequest;
    self->si.tick = tick;
    self->si.finish = finish;
    self->si.destroy = destroy;

    return self;
}

// Given a branch operation, return the predicted PC address
uint64_t branchRequest(trace_op *op, int processorNum) {
    assert(op != NULL);

    uint64_t pcAddress = op->pcAddress;
    uint64_t predAddress = op->nextPCAddress; // 100% accuracy

    // In student's simulator, either return a predicted address from BTB
    //   or pcAddress + 4 as a simplified "not taken".
    // Predictor has the actual nextPCAddress, so it knows how to update
    //   its state after computing the prediction.

    return predAddress;
}

int tick() { return 1; }

int finish(int outFd) { return 0; }

int destroy(void) {
    // free any internally allocated memory here
    return 0;
}
