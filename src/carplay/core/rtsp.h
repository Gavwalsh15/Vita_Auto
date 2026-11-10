#pragma once

#include "rt.h"

namespace carplay::core {

constexpr size_t kRtspMaxHeaderBytes = 16 * 1024;
constexpr size_t kRtspMaxBodyBytes = 1024 * 1024;

struct RtspRequest {
    char method[24] = {};
    char path[256] = {};
    char protocol[48] = {};
    Bytes headers;
    Bytes body;

    bool header(const char *name, char *value, size_t capacity) const;
    bool header_number(const char *name, uint64_t &value) const;
    bool path_ends_with(const char *suffix) const;
};

enum class RtspParse { NeedMore, Ok, Error };

RtspParse rtsp_parse(const uint8_t *data, size_t length, RtspRequest &out, size_t &consumed);

bool rtsp_response(const RtspRequest &request, int status, const char *content_type, const uint8_t *body,
                   size_t body_length, Bytes &out);

bool rtsp_request(const char *method, const char *path, unsigned cseq, const char *content_type,
                  const uint8_t *body, size_t body_length, Bytes &out);

} // namespace carplay::core
