#include "canutil.h"

CanSignalState* lookupSignalState(CanSignal* signal, CanSignal* signals,
        int signalCount, char* name) {
    for(int i = 0; i < signal->stateCount; i++) {
        if(!strcmp(signal->states[i].name, name)) {
            return &signal->states[i];
        }
    }
}

CanSignalState* lookupSignalState(CanSignal* signal, CanSignal* signals,
        int signalCount, int value) {
    for(int i = 0; i < signal->stateCount; i++) {
        if(signal->states[i].value == value) {
            return &signal->states[i];
        }
    }
}

uint64_t encodeCanSignal(CanSignal* signal, float value) {
    unsigned long rawValue = (value - signal->offset) / signal->factor;
    uint64_t data = 0;
    setBitField(&data, rawValue, signal->bitPosition, signal->bitSize);
    return data;
}

float decodeCanSignal(CanSignal* signal, uint8_t* data) {
    unsigned long rawValue = getBitField(data, signal->bitPosition,
            signal->bitSize);
    return rawValue * signal->factor + signal->offset;
}

float passthroughHandler(CanSignal* signal, CanSignal* signals, int signalCount,
        float value, bool* send) {
    return value;
}

bool booleanHandler(CanSignal* signal, CanSignal* signals, int signalCount,
        float value, bool* send) {
    return value == 0.0 ? false : true;
}

float ignoreHandler(CanSignal* signal, CanSignal* signals, int signalCount,
        float value, bool* send) {
    *send = false;
    return 0.0;
}

char* stateHandler(CanSignal* signal, CanSignal* signals, int signalCount,
        float value, bool* send) {
    CanSignalState* signalState = lookupSignalState(signal, signals,
            signalCount, value);
    if(signalState != NULL) {
        return signalState->name;
    }
    *send = false;
}

void checkWritePermission(CanSignal* signal, bool* send) {
    if(!signal->writable) {
        *send = false;
    }
}

uint64_t booleanWriter(CanSignal* signal, CanSignal* signals,
        int signalCount, cJSON* value, bool* send) {
    return encodeCanSignal(signal, value->valueint);
}

uint64_t numberWriter(CanSignal* signal, CanSignal* signals,
        int signalCount, cJSON* value, bool* send) {
    checkWritePermission(signal, send);
    return encodeCanSignal(signal, value->valuedouble);
}

uint64_t stateWriter(CanSignal* signal, CanSignal* signals,
        int signalCount, cJSON* value, bool* send) {
    CanSignalState* signalState = lookupSignalState(signal, signals,
            signalCount, value->valuestring);
    if(signalState != NULL) {
        checkWritePermission(signal, send);
        return encodeCanSignal(signal, signalState->value);
    }
    *send = false;
    return 0;
}

CanSignal* lookupSignal(char* name, CanSignal* signals, int signalCount) {
    for(int i = 0; i < signalCount; i++) {
        CanSignal* signal = &signals[i];
        if(!strcmp(name, signal->genericName)) {
            return signal;
        }
    }
    printf("Couldn't find a signal with the genericName \"%s\" "
            "-- probably about to segfault\n", name);
    return NULL;
}
