#include "json_min.h"

static bool hex4(JsonMin* j, uint32_t* out) {
    if(j->length - j->position < 4) return false;
    *out = 0;
    for(unsigned i = 0; i < 4; ++i) {
        unsigned char c = j->text[j->position++];
        unsigned d;
        if(c >= '0' && c <= '9')
            d = c - '0';
        else if(c >= 'a' && c <= 'f')
            d = c - 'a' + 10;
        else if(c >= 'A' && c <= 'F')
            d = c - 'A' + 10;
        else
            return false;
        *out = *out * 16 + d;
    }
    return true;
}

static void utf8(char* out, size_t* length, uint32_t c) {
    if(c < 0x80)
        out[(*length)++] = c;
    else {
        unsigned n = c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
        out[(*length)++] = (0xff << (8 - n)) | (c >> (6 * (n - 1)));
        while(--n)
            out[(*length)++] = 0x80 | ((c >> (6 * (n - 1))) & 0x3f);
    }
}

bool json_min_string(JsonMin* j, uint16_t token) {
    JsonMinToken* t = &j->tokens[token];
    t->start = ++j->position;
    char* out = j->text + t->start;
    size_t n = 0;
    while(j->position < j->length) {
        unsigned char c = j->text[j->position++];
        if(c == '"') {
            t->length = n;
            return true;
        }
        if(c < 0x20) return false;
        if(c == '\\') {
            if(j->position == j->length) return false;
            c = j->text[j->position++];
            switch(c) {
            case '"':
            case '\\':
            case '/':
                out[n++] = c;
                break;
            case 'b':
                out[n++] = '\b';
                break;
            case 'f':
                out[n++] = '\f';
                break;
            case 'n':
                out[n++] = '\n';
                break;
            case 'r':
                out[n++] = '\r';
                break;
            case 't':
                out[n++] = '\t';
                break;
            case 'u': {
                uint32_t code;
                if(!hex4(j, &code)) return false;
                if(code >= 0xd800 && code <= 0xdbff) {
                    if(j->length - j->position < 6 || j->text[j->position++] != '\\' ||
                       j->text[j->position++] != 'u')
                        return false;
                    uint32_t low;
                    if(!hex4(j, &low) || low < 0xdc00 || low > 0xdfff) return false;
                    code = 0x10000 + ((code - 0xd800) << 10) + low - 0xdc00;
                } else if(code >= 0xdc00 && code <= 0xdfff)
                    return false;
                utf8(out, &n, code);
                break;
            }
            default:
                return false;
            }
        } else if(c >= 0x80) {
            unsigned extra = c >= 0xc2 && c <= 0xdf ? 1 :
                             c >= 0xe0 && c <= 0xef ? 2 :
                             c >= 0xf0 && c <= 0xf4 ? 3 :
                                                      0;
            if(!extra || j->length - j->position < (int)extra) return false;
            uint32_t code = c & ((1u << (6 - extra)) - 1);
            for(unsigned i = 0; i < extra; ++i) {
                unsigned char b = j->text[j->position++];
                if((b & 0xc0) != 0x80) return false;
                code = (code << 6) | (b & 0x3f);
            }
            if(code < (extra == 1 ? 0x80u :
                       extra == 2 ? 0x800u :
                                    0x10000u) ||
               code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff))
                return false;
            utf8(out, &n, code);
        } else
            out[n++] = c;
    }
    return false;
}
