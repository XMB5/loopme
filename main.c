#include <stdio.h>
#include <pulse/pulseaudio.h>
#include <string.h>
#include <limits.h>

static char* inputSourceId;
static char* outputSinkId;

static uint32_t realSourceIndex = -1;
static uint32_t realSinkIndex = -1;
static uint32_t sourceIndex = -1;
static uint32_t sinkMonitorIndex = -1;
static uint32_t sinkIndex = -1;

struct moveRequest {
    uint32_t index;
    uint32_t moveTo;
};
typedef struct moveRequest moveRequest;

#define MOVE_REQUESTS_MAX_LEN 16
static moveRequest moveRequests[MOVE_REQUESTS_MAX_LEN];
static int moveRequestsLen = 0;
static int moveRequestsCompleted = 0;

void addMoveRequest(uint32_t index, uint32_t moveTo) {
    if (moveRequestsLen == MOVE_REQUESTS_MAX_LEN) {
        printf("dropping move request index %d, moveTo %d\n", index, moveTo);
    } else {
        moveRequest* moveRequest = moveRequests + moveRequestsLen;
        moveRequest->index = index;
        moveRequest->moveTo = moveTo;
        moveRequestsLen++;
    }
}

void resetMoveRequests() {
    moveRequestsLen = 0;
    moveRequestsCompleted = 0;
}

void stateCb(pa_context* ctx, void* userdata) {
    *((enum pa_context_state*) userdata) = pa_context_get_state(ctx);
}

void sourceListCb(pa_context* ctx, const pa_source_info* info, int eol, void* userdata) {
    if (eol > 0) {
        return;
    }

    const char* driverName = pa_proplist_gets(info->proplist, "alsa.driver_name");
    if (driverName != NULL && strcmp(driverName, "snd_aloop") == 0) {
        const char* class = pa_proplist_gets(info->proplist, "device.class");
        if (class != NULL) {
            if (strcmp(class, "sound") == 0) {
                sourceIndex = info->index;
            } else if (strcmp(class, "monitor") == 0) {
                sinkMonitorIndex = info->index;
            }
        }
    }

    const char* alsaId = pa_proplist_gets(info->proplist, "alsa.id");
    if (alsaId != NULL && strcmp(alsaId, inputSourceId) == 0) {
        realSourceIndex = info->index;
    }
}

void sinkInputListCb(pa_context* ctx, const pa_sink_input_info* info, int eol, void* userdata) {
    if (eol > 0) {
        return;
    }

    const char* applicationName = pa_proplist_gets(info->proplist, "application.name");
    if (applicationName != NULL && strcmp(applicationName, "OBS") == 0) {
        if (info->sink != sinkIndex) {
            addMoveRequest(info->index, sinkIndex);
        }
    } else {
        if (info->sink != realSinkIndex) {
            addMoveRequest(info->index, realSinkIndex);
        }
    }
}

void sinkInputMoveCb(pa_context* ctx, int success, void* userdata) {
    if (!success) {
        printf("failed to move sink input\n");
    }
}

void sinkListCb(pa_context* ctx, const pa_sink_info* info, int eol, void* userdata) {
    if (eol > 0) {
        return;
    }

    const char* driverName = pa_proplist_gets(info->proplist, "alsa.driver_name");
    if (driverName != NULL && strcmp(driverName, "snd_aloop") == 0) {
        sinkIndex = info->index;
    }

    const char* alsaId = pa_proplist_gets(info->proplist, "alsa.id");
    if (alsaId != NULL && strcmp(alsaId, outputSinkId) == 0) {
        realSinkIndex = info->index;
    }
}

void sourceOutputListCb(pa_context* ctx, const pa_source_output_info* info, int eol, void* userdata) {
    if (eol > 0) {
        return;
    }

    const char* applicationName = pa_proplist_gets(info->proplist, "application.name");
    if (applicationName != NULL && strcmp(applicationName, "OBS") == 0) {
        if (info->source != realSourceIndex) {
            addMoveRequest(info->index, realSourceIndex);
        }
    } else {
        if (info->source != sinkMonitorIndex) {
            addMoveRequest(info->index, sinkMonitorIndex);
        }
    }
}

void sourceOutputMoveCb(pa_context* ctx, int success, void* userdata) {
    if (!success) {
        printf("failed to move source output\n");
    }
}

void subscribeCb(pa_context* ctx, pa_subscription_event_type_t t, uint32_t id, void* userdata) {
    int* alarm = (int*) userdata;

    if ((t & PA_SUBSCRIPTION_EVENT_TYPE_MASK) == PA_SUBSCRIPTION_EVENT_NEW) {
        const pa_subscription_event_type_t eventFacility = t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK;
        if (eventFacility == PA_SUBSCRIPTION_EVENT_SINK_INPUT || eventFacility == PA_SUBSCRIPTION_EVENT_SOURCE_OUTPUT) {
            *alarm = 1;
        }
    }
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        printf("usage: loopme <input_source_alsa_id> <output_sink_alsa_id>");
        return 1;
    }
    inputSourceId = argv[1];
    outputSinkId = argv[2];
    printf("start with input source id %s and output sink id %s\n", inputSourceId, outputSinkId);
    pa_mainloop* mainloop = pa_mainloop_new();
    pa_mainloop_api* api = pa_mainloop_get_api(mainloop);
    pa_context* ctx = pa_context_new(api, "loopme");
    pa_context_connect(ctx, NULL, 0, NULL);
    enum pa_context_state paState = PA_CONTEXT_UNCONNECTED;
    pa_context_set_state_callback(ctx, stateCb, &paState);

    int state = 0;
    int alarm = 0;
    pa_operation* op = NULL;
    int alreadySubscribed = 0;

    while(1) {
        if (paState == PA_CONTEXT_READY) {
            reprocess:
            if (state == 0) {
                printf("list sources\n");
                op = pa_context_get_source_info_list(ctx, sourceListCb, NULL);
                state++;
            }
            if (state == 1) {
                if (pa_operation_get_state(op) == PA_OPERATION_DONE) {
                    printf("source index: %d\nsink monitor index: %d\nreal source index: %d\n", sourceIndex, sinkMonitorIndex, realSourceIndex);
                    pa_operation_unref(op);
                    state++;
                }
            }
            if (state == 2) {
                printf("list sinks\n");
                op = pa_context_get_sink_info_list(ctx, sinkListCb, NULL);
                state++;
            }
            if (state == 3) {
                if (pa_operation_get_state(op) == PA_OPERATION_DONE) {
                    printf("sink index: %d\nreal sink index: %d\n", sinkIndex, realSinkIndex);
                    pa_operation_unref(op);
                    state++;
                }
            }
            if (state == 4) {
                printf("list sink inputs\n");
                resetMoveRequests();
                op = pa_context_get_sink_input_info_list(ctx, sinkInputListCb, NULL);
                state++;
            }
            if (state == 5) {
                if (pa_operation_get_state(op) == PA_OPERATION_DONE) {
                    printf("%d sink inputs to move\n", moveRequestsLen);
                    pa_operation_unref(op);
                    state++;
                }
            }
            if (state == 6) {
                if (moveRequestsCompleted == moveRequestsLen) {
                    state += 2;
                } else {
                    moveRequest* req = moveRequests + moveRequestsCompleted;
                    printf("move sink input %d to sink %d\n", req->index, req->moveTo);
                    op = pa_context_move_sink_input_by_index(ctx, req->index, req->moveTo, sinkInputMoveCb, NULL);
                    state++;
                }
            }
            if (state == 7) {
                if (pa_operation_get_state(op) == PA_OPERATION_DONE) {
                    moveRequestsCompleted++;
                    pa_operation_unref(op);
                    state--;
                    goto reprocess;
                }
            }
            if (state == 8) {
                printf("list source outputs\n");
                resetMoveRequests();
                op = pa_context_get_source_output_info_list(ctx, sourceOutputListCb, NULL);
                state++;
            }
            if (state == 9) {
                if (pa_operation_get_state(op) == PA_OPERATION_DONE) {
                    printf("%d source outputs to move\n", moveRequestsLen);
                    pa_operation_unref(op);
                    state++;
                }
            }
            if (state == 10) {
                if (moveRequestsCompleted == moveRequestsLen) {
                    state += 2;
                } else {
                    moveRequest* req = moveRequests + moveRequestsCompleted;
                    printf("move source output %d to source %d\n", req->index, req->moveTo);
                    op = pa_context_move_source_output_by_index(ctx, req->index, req->moveTo, sourceOutputMoveCb, NULL);
                    state++;
                }
            }
            if (state == 11) {
                if (pa_operation_get_state(op) == PA_OPERATION_DONE) {
                    moveRequestsCompleted++;
                    pa_operation_unref(op);
                    state--;
                    goto reprocess;
                }
            }
            if (state == 12) {
                if (!alreadySubscribed) {
                    printf("subscribe\n");
                    pa_context_set_subscribe_callback(ctx, subscribeCb, &alarm);
                    pa_context_subscribe(ctx, PA_SUBSCRIPTION_MASK_SINK_INPUT | PA_SUBSCRIPTION_MASK_SOURCE_OUTPUT, NULL, NULL);
                    alreadySubscribed = 1;
                }
                state++;
            }
            if (state == 13) {
                if (alarm != 0) {
                    state = 4; //list sink inputs
                    alarm = 0;
                    goto reprocess;
                }
            }
        } else if (paState == PA_CONTEXT_FAILED || paState == PA_CONTEXT_TERMINATED) {
            break;
        }

        if (pa_mainloop_iterate(mainloop, 1, NULL) < 0) {
            break;
        }
    }

    pa_context_disconnect(ctx);
    pa_context_unref(ctx);
    pa_mainloop_free(mainloop);
}
