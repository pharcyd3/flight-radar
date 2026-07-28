#include "apistatus.h"

static ApiStatus _status;

const ApiStatus& apiStatus() { return _status; }

void setApiStatus(ApiState st, int http, int bytes, const char* detail) {
    _status.state    = st;
    _status.httpCode = http;
    _status.bytes    = bytes;
    _status.lastMs   = millis();
    strncpy(_status.detail, detail, sizeof(_status.detail) - 1);
    _status.detail[sizeof(_status.detail) - 1] = '\0';
}
