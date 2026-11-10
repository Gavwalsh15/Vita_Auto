#include "rtsp.h"

namespace carplay::core {

namespace {

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool append_text(Bytes &out, const char *text) {
    return out.append(reinterpret_cast<const uint8_t *>(text), strlen(text));
}

bool append_decimal(Bytes &out, uint64_t value) {
    char digits[24];
    int count = 0;
    // Repeated subtraction of powers of ten: no 64-bit division helper needed.
    static const uint64_t kPowers[] = {10000000000000000000ull, 1000000000000000000ull, 100000000000000000ull,
                                       10000000000000000ull, 1000000000000000ull, 100000000000000ull,
                                       10000000000000ull, 1000000000000ull, 100000000000ull, 10000000000ull,
                                       1000000000ull, 100000000ull, 10000000ull, 1000000ull, 100000ull, 10000ull,
                                       1000ull, 100ull, 10ull, 1ull};
    for (uint64_t power : kPowers) {
        char digit = '0';
        while (value >= power) {
            value -= power;
            ++digit;
        }
        if (digit != '0' || count || power == 1) digits[count++] = digit;
    }
    return out.append(reinterpret_cast<const uint8_t *>(digits), static_cast<size_t>(count));
}

bool copy_field(const uint8_t *begin, const uint8_t *end, char *field, size_t capacity) {
    size_t length = static_cast<size_t>(end - begin);
    if (!length || length >= capacity) return false;
    memcpy(field, begin, length);
    field[length] = 0;
    return true;
}

const char *status_text(int status) {
    switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 470: return "Connection Authorization Required";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    default: return "OK";
    }
}

} // namespace

bool RtspRequest::header(const char *name, char *value, size_t capacity) const {
    if (!capacity) return false;
    value[0] = 0;
    const char *text = reinterpret_cast<const char *>(headers.data());
    size_t name_length = strlen(name);
    for (size_t at = 0; text && at < headers.size() && text[at];) {
        size_t end = at;
        while (text[end] && text[end] != '\r' && text[end] != '\n') ++end;
        size_t k = 0;
        while (k < name_length && at + k < end && lower(text[at + k]) == lower(name[k])) ++k;
        size_t colon = at + k;
        while (k == name_length && colon < end && (text[colon] == ' ' || text[colon] == '\t')) ++colon;
        if (k == name_length && colon < end && text[colon] == ':') {
            size_t begin = colon + 1, stop = end;
            while (begin < stop && (text[begin] == ' ' || text[begin] == '\t')) ++begin;
            while (stop > begin && (text[stop - 1] == ' ' || text[stop - 1] == '\t')) --stop;
            size_t length = std::min(stop - begin, capacity - 1);
            memcpy(value, text + begin, length);
            value[length] = 0;
            return true;
        }
        at = end;
        while (text[at] == '\r' || text[at] == '\n') ++at;
    }
    return false;
}

bool RtspRequest::header_number(const char *name, uint64_t &value) const {
    char text[24];
    if (!header(name, text, sizeof text) || !text[0]) return false;
    value = 0;
    for (const char *c = text; *c; ++c) {
        if (*c < '0' || *c > '9' || value > 0x0fffffffffffffffull) return false;
        value = value * 10 + static_cast<uint64_t>(*c - '0');
    }
    return true;
}

bool RtspRequest::path_ends_with(const char *suffix) const {
    size_t length = strlen(path), suffix_length = strlen(suffix);
    if (suffix_length > length) return false;
    for (size_t i = 0; i < suffix_length; ++i)
        if (lower(path[length - suffix_length + i]) != lower(suffix[i])) return false;
    return true;
}

RtspParse rtsp_parse(const uint8_t *data, size_t length, RtspRequest &out, size_t &consumed) {
    consumed = 0;
    size_t header_end = 0;
    bool found = false;
    for (size_t i = 0; i + 4 <= length && i <= kRtspMaxHeaderBytes; ++i)
        if (data[i] == '\r' && data[i + 1] == '\n' && data[i + 2] == '\r' && data[i + 3] == '\n') {
            header_end = i;
            found = true;
            break;
        }
    if (!found) return length > kRtspMaxHeaderBytes ? RtspParse::Error : RtspParse::NeedMore;

    size_t line_end = 0;
    while (line_end < header_end && data[line_end] != '\r') ++line_end;
    const uint8_t *line = data, *stop = data + line_end;
    const uint8_t *space1 = line;
    while (space1 < stop && *space1 != ' ') ++space1;
    const uint8_t *space2 = space1 < stop ? space1 + 1 : stop;
    while (space2 < stop && *space2 != ' ') ++space2;
    out = RtspRequest();
    if (!copy_field(line, space1, out.method, sizeof out.method)) return RtspParse::Error;
    if (space1 < stop && !copy_field(space1 + 1, space2, out.path, sizeof out.path)) return RtspParse::Error;
    if (space2 < stop) {
        if (!copy_field(space2 + 1, stop, out.protocol, sizeof out.protocol)) return RtspParse::Error;
    } else {
        memcpy(out.protocol, "RTSP/1.0", 9);
    }
    size_t headers_begin = line_end + 2 <= header_end ? line_end + 2 : header_end;
    if (!out.headers.append(data + headers_begin, header_end - headers_begin) || !out.headers.push(0))
        return RtspParse::Error;

    uint64_t content_length = 0;
    out.header_number("Content-Length", content_length);
    if (content_length > kRtspMaxBodyBytes) return RtspParse::Error;
    size_t body_begin = header_end + 4;
    if (length - body_begin < content_length) return RtspParse::NeedMore;
    if (!out.body.assign(data + body_begin, static_cast<size_t>(content_length))) return RtspParse::Error;
    consumed = body_begin + static_cast<size_t>(content_length);
    return RtspParse::Ok;
}

bool rtsp_response(const RtspRequest &request, int status, const char *content_type, const uint8_t *body,
                   size_t body_length, Bytes &out) {
    char cseq[24];
    bool ok = append_text(out, request.protocol[0] ? request.protocol : "RTSP/1.0") && append_text(out, " ") &&
              append_decimal(out, static_cast<uint64_t>(status)) && append_text(out, " ") &&
              append_text(out, status_text(status)) && append_text(out, "\r\n");
    if (ok && content_type) ok = append_text(out, "Content-Type: ") && append_text(out, content_type) && append_text(out, "\r\n");
    if (ok && request.header("CSeq", cseq, sizeof cseq)) ok = append_text(out, "CSeq: ") && append_text(out, cseq) && append_text(out, "\r\n");
    ok = ok && append_text(out, "Content-Length: ") &&
         append_decimal(out, body_length) && append_text(out, "\r\n\r\n");
    return ok && out.append(body, body_length);
}

bool rtsp_request(const char *method, const char *path, unsigned cseq, const char *content_type,
                  const uint8_t *body, size_t body_length, Bytes &out) {
    bool ok = append_text(out, method) && append_text(out, " ") && append_text(out, path) &&
              append_text(out, " RTSP/1.0\r\nCSeq: ") && append_decimal(out, cseq) && append_text(out, "\r\n");
    if (ok && content_type) ok = append_text(out, "Content-Type: ") && append_text(out, content_type) && append_text(out, "\r\n");
    ok = ok && append_text(out, "Content-Length: ") && append_decimal(out, body_length) && append_text(out, "\r\n\r\n");
    return ok && out.append(body, body_length);
}

} // namespace carplay::core
