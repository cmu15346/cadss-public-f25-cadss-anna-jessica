#include "coher_internal.h"
#include "coherence.h"
#include "interconnect.h"

void sendBusRd(uint64_t addr, int procNum) {
    inter_sim->busReq(BUSRD, addr, procNum);
}

void sendBusWr(uint64_t addr, int procNum) {
    inter_sim->busReq(BUSWR, addr, procNum);
}

void sendData(uint64_t addr, int procNum) {
    inter_sim->busReq(DATA, addr, procNum);
}

void indicateShared(uint64_t addr, int procNum) {
    inter_sim->busReq(SHARED, addr, procNum);
}

// ---------------------------------------
// MI
// ---------------------------------------

coherence_states cacheMI(uint8_t is_read, uint8_t *permAvail,
                         coherence_states currentState, uint64_t addr,
                         int procNum) {
    switch (currentState) {
    case INVALID:
        *permAvail = 0;
        sendBusWr(addr, procNum);
        return INVALID_MODIFIED;
    case MODIFIED:
        *permAvail = 1;
        return MODIFIED;
    case INVALID_MODIFIED:
        fprintf(stderr, "IM state on %lx, but request %d\n", addr, is_read);
        *permAvail = 0;
        return INVALID_MODIFIED;
    default:
        fprintf(stderr, "State %d not supported, found on %lx\n", currentState,
                addr);
        break;
    }

    return INVALID;
}

coherence_states snoopMI(bus_req_type reqType, cache_action *ca,
                         coherence_states currentState, uint64_t addr,
                         int procNum) {
    *ca = NO_ACTION;
    switch (currentState) {
    case INVALID:
        return INVALID;
    case MODIFIED:
        sendData(addr, procNum);
        // indicateShared(addr, procNum); // Needed for E state
        *ca = INVALIDATE;
        return INVALID;
    case INVALID_MODIFIED:
        if (reqType == DATA || reqType == SHARED) {
            *ca = DATA_RECV;
            return MODIFIED;
        }

        return INVALID_MODIFIED;
    default:
        fprintf(stderr, "State %d not supported, found on %lx\n", currentState,
                addr);
        break;
    }

    return INVALID;
}

// ---------------------------------------
// MSI
// ---------------------------------------

coherence_states cacheMSI(uint8_t is_read, uint8_t *permAvail,
                          coherence_states currentState, uint64_t addr,
                          int procNum) {
    switch (currentState) {
    case INVALID:
        *permAvail = 0;
        // I -> S attempt
        if (is_read) {
            sendBusRd(addr, procNum);
            return INVALID_SHARED;
        }
        // I -> M attempt
        else {
            sendBusWr(addr, procNum);
            return INVALID_MODIFIED;
        }
    case SHARED_STATE:
        // Upon read for Shared, do nothing
        if (is_read) {
            *permAvail = 1;
            return SHARED_STATE;
        }
        // S -> M attempt
        else {
            *permAvail = 0;
            sendBusWr(addr, procNum);
            return SHARED_MODIFIED;
        }
    case MODIFIED:
        // always available in M state
        *permAvail = 1;
        return MODIFIED;
    case INVALID_MODIFIED:
        *permAvail = 0;
        return INVALID_MODIFIED;
    case INVALID_SHARED:
        *permAvail = 0;
        return INVALID_SHARED;
    case SHARED_MODIFIED:
        *permAvail = 0;
        return SHARED_MODIFIED;
    default:
        fprintf(stderr, "State %d not supported, found on %lx\n", currentState,
                addr);
        break;
    }

    return INVALID;
}

coherence_states snoopMSI(bus_req_type reqType, cache_action *ca,
                          coherence_states currentState, uint64_t addr,
                          int procNum) {
    *ca = NO_ACTION;
    switch (currentState) {
    case INVALID:
        return INVALID;
    case MODIFIED:
        // Modified sees BUSWR (BusRdx)
        if (reqType == BUSWR) {
            sendData(addr, procNum);
            *ca = INVALIDATE; // TODO: is this equiv to flush?
            return INVALID;
        }
        // Modified sees BUSRD (BusRd)
        else if (reqType == BUSRD) {
            sendData(addr, procNum);
            indicateShared(addr, procNum);
            *ca = INVALIDATE; // TODO: is this equiv to flush?
            return INVALID;
        }
    case SHARED_STATE:
        // Shared sees BUSWR (BusRdx)
        if (reqType == BUSWR) {
            return INVALID;
        }
    case INVALID_MODIFIED:
        if (reqType == SHARED || reqType == DATA) // TODO: more granular?
        {
            *ca = DATA_RECV;
            return MODIFIED;
        }

        return INVALID_MODIFIED;
    case INVALID_SHARED:
        if (reqType == SHARED || reqType == DATA) // TODO: more granular?
        {
            *ca = DATA_RECV;
            return SHARED_STATE;
        }

        return INVALID_SHARED;
    case SHARED_MODIFIED:
        if (reqType == SHARED || reqType == DATA) // TODO: more granular?
        {
            *ca = DATA_RECV;
            return MODIFIED;
        }

        return SHARED_MODIFIED;
    default:
        fprintf(stderr, "State %d not supported, found on %lx\n", currentState,
                addr);
        break;
    }

    return INVALID;
}

// ---------------------------------------
// MESI
// ---------------------------------------

coherence_states cacheMESI(uint8_t is_read, uint8_t *permAvail,
                           coherence_states currentState, uint64_t addr,
                           int procNum) {
    switch (currentState) {
    case INVALID:
        *permAvail = 0;
        if (is_read) {
            sendBusRd(addr, procNum);
            return INVALID_READ;
        } else {
            sendBusWr(addr, procNum);
            return INVALID_MODIFIED;
        }
    case MODIFIED:
        *permAvail = 1;
        return MODIFIED;
    case EXCLUSIVE:
        *permAvail = 1;
        if (is_read) {
            return EXCLUSIVE;
        } else {
            return MODIFIED;
        }
    case SHARED_STATE:
        if (is_read) {
            *permAvail = 1;
            return SHARED_STATE;
        } else {
            *permAvail = 0;
            sendBusWr(addr, procNum);
            return SHARED_MODIFIED;
        }
    case INVALID_MODIFIED:
        *permAvail = 0;
        return INVALID_MODIFIED;
    case INVALID_READ:
        *permAvail = 0;
        return INVALID_READ;
    case SHARED_MODIFIED:
        *permAvail = 0;
        return SHARED_MODIFIED;
    default:
        fprintf(stderr, "State %d not supported, found on %lx\n", currentState,
                addr);
        break;
    }

    return INVALID;
}

coherence_states snoopMESI(bus_req_type reqType, cache_action *ca,
                           coherence_states currentState, uint64_t addr,
                           int procNum) {
    *ca = NO_ACTION;
    switch (currentState) {
    case INVALID:
        return INVALID;
    case MODIFIED:
        if (reqType == BUSRD) {
            sendData(addr, procNum);
            indicateShared(addr, procNum);
            *ca = INVALIDATE;
            return SHARED_STATE;
        } else if (reqType == BUSWR) {
            sendData(addr, procNum);
            *ca = INVALIDATE;
            return INVALID;
        }
        return MODIFIED;
    case EXCLUSIVE:
        if (reqType == BUSRD) {
            indicateShared(addr, procNum);
            return SHARED_STATE;
        } else if (reqType == BUSWR) {
            return INVALID;
        }
        return EXCLUSIVE;
    case SHARED_STATE:
        if (reqType == BUSRD) {
            indicateShared(addr, procNum);
            return SHARED_STATE;
        } else if (reqType == BUSWR) {
            return INVALID;
        }
        return SHARED_STATE;
    case INVALID_MODIFIED:
        if (reqType == DATA || reqType == SHARED) {
            *ca = DATA_RECV;
            return MODIFIED;
        }
        return INVALID_MODIFIED;
    case INVALID_READ:
        if (reqType == DATA) {
            *ca = DATA_RECV;
            return EXCLUSIVE;
        } else if (reqType == SHARED) {
            *ca = DATA_RECV;
            return SHARED_STATE;
        }
        return INVALID_READ;
    case SHARED_MODIFIED:
        if (reqType == DATA || reqType == SHARED) {
            *ca = DATA_RECV;
            return MODIFIED;
        }
        return SHARED_MODIFIED;
    default:
        fprintf(stderr, "State %d not supported, found on %lx\n", currentState,
                addr);
        break;
    }

    return INVALID;
}

// ---------------------------------------
// MOESI
// ---------------------------------------

// ---------------------------------------
// MESIF
// ---------------------------------------

coherence_states cacheMESIF(uint8_t is_read, uint8_t *permAvail,
                            coherence_states currentState, uint64_t addr,
                            int procNum) {
    switch (currentState) {
    case INVALID:
        *permAvail = 0;
        if (is_read) {
            sendBusRd(addr, procNum);
            return INVALID_READ;
        } else {
            sendBusWr(addr, procNum);
            return INVALID_MODIFIED;
        }
    case MODIFIED:
        *permAvail = 1;
        return MODIFIED;
    case EXCLUSIVE:
        *permAvail = 1;
        if (is_read) {
            return EXCLUSIVE;
        } else {
            return MODIFIED;
        }
    case SHARED_STATE:
        if (is_read) {
            *permAvail = 1;
            return SHARED_STATE;
        } else {
            *permAvail = 0;
            sendBusWr(addr, procNum);
            return SHARED_MODIFIED;
        }
    case FORWARD:
        if (is_read) {
            *permAvail = 1;
            return FORWARD;
        } else {
            *permAvail = 0;
            sendBusWr(addr, procNum);
            return FORWARD_MODIFIED;
        }
    case INVALID_MODIFIED:
        *permAvail = 0;
        return INVALID_MODIFIED;
    case INVALID_READ:
        *permAvail = 0;
        return INVALID_READ;
    case SHARED_MODIFIED:
        *permAvail = 0;
        return SHARED_MODIFIED;
    case FORWARD_MODIFIED:
        *permAvail = 0;
        return FORWARD_MODIFIED;
    default:
        fprintf(stderr, "State %d not supported, found on %lx\n", currentState,
                addr);
        break;
    }

    return INVALID;
}

coherence_states snoopMESIF(bus_req_type reqType, cache_action *ca,
                            coherence_states currentState, uint64_t addr,
                            int procNum) {
    *ca = NO_ACTION;
    switch (currentState) {
    case INVALID:
        return INVALID;
    case MODIFIED:
        if (reqType == BUSRD) {
            sendData(addr, procNum);
            indicateShared(addr, procNum);
            *ca = INVALIDATE;
            return procNum == 0 ? FORWARD : SHARED_STATE;
            // return SHARED_STATE;
        } else if (reqType == BUSWR) {
            sendData(addr, procNum);
            *ca = INVALIDATE;
            return INVALID;
        }
        return MODIFIED;
    case EXCLUSIVE:
        if (reqType == BUSRD) {
            if (procNum == 0) {
                indicateShared(addr, procNum);
                return FORWARD;
            } else {
                return SHARED_STATE;
            }
            // indicateShared(addr, procNum);
            return SHARED_STATE;
        } else if (reqType == BUSWR) {
            return INVALID;
        }
        return EXCLUSIVE;
    case SHARED_STATE:
        if (reqType == BUSRD) {
            return SHARED_STATE;
        } else if (reqType == BUSWR) {
            return INVALID;
        }
        return SHARED_STATE;
    case FORWARD:
        if (reqType == BUSRD) {
            indicateShared(addr, procNum);
            return SHARED_STATE;
        } else if (reqType == BUSWR) {
            return INVALID;
        }
        return FORWARD;
    case INVALID_READ:
        if (reqType == DATA) {
            *ca = DATA_RECV;
            return EXCLUSIVE;
        } else if (reqType == SHARED) {
            *ca = DATA_RECV;
            return FORWARD;
        }
        return INVALID_READ;
    case INVALID_MODIFIED:
        if (reqType == DATA || reqType == SHARED) {
            *ca = DATA_RECV;
            return MODIFIED;
        }
        return INVALID_MODIFIED;
    case SHARED_MODIFIED:
        if (reqType == DATA || reqType == SHARED) {
            *ca = DATA_RECV;
            return MODIFIED;
        }
        return SHARED_MODIFIED;
    case FORWARD_MODIFIED:
        if (reqType == DATA || reqType == SHARED) {
            *ca = DATA_RECV;
            return MODIFIED;
        }
        return FORWARD_MODIFIED;
    default:
        fprintf(stderr, "State %d not supported, found on %lx\n", currentState,
                addr);
        break;
    }

    return INVALID;
}