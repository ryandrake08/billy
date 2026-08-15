#include "json_util.h"
#include <stdio.h>
#include <string.h>

bool json_get_string(const char *json, const char *key, char *out, size_t out_len)
{
    char pat[48];
    int patlen = snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return false;
    p += patlen;
    while (*p == ' ' || *p == '\t' || *p == ':') p++;
    if (*p != '"') return false;
    p++;

    size_t o = 0;
    while (*p && *p != '"' && o + 1 < out_len)
    {
        char c = *p;
        if (c == '\\' && p[1])
        {
            p++;
            switch (*p)
            {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                default:  c = *p;   break;   // ", \, / pass through
            }
        }
        out[o++] = c;
        p++;
    }
    out[o] = '\0';
    return true;
}

void json_escape(const char *in, char *out, size_t out_len)
{
    size_t o = 0;
    while (*in && o + 2 < out_len)
    {
        char c = *in++;
        switch (c)
        {
            case '"':  out[o++] = '\\'; out[o++] = '"';  break;
            case '\\': out[o++] = '\\'; out[o++] = '\\'; break;
            case '\n': out[o++] = '\\'; out[o++] = 'n';  break;
            case '\r': out[o++] = '\\'; out[o++] = 'r';  break;
            case '\t': out[o++] = '\\'; out[o++] = 't';  break;
            default:   if ((unsigned char) c >= 0x20) out[o++] = c; break;   // drop other controls
        }
    }
    out[o] = '\0';
}
