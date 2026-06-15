/*
 * xml.c — implementation of the minimal SAX XML parser (see xml.h).
 */

#include <stdlib.h>
#include <string.h>

#include "xml.h"

/* ---- growable byte buffer ------------------------------------------ */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} sb;

static int sb_reserve(sb *b, size_t extra)
{
    if (b->len + extra + 1 <= b->cap) return 0;
    size_t cap = b->cap ? b->cap * 2 : 64;
    while (cap < b->len + extra + 1) cap *= 2;
    char *p = realloc(b->data, cap);
    if (!p) return -1;
    b->data = p;
    b->cap = cap;
    return 0;
}

static int sb_putc(sb *b, char c)
{
    if (sb_reserve(b, 1)) return -1;
    b->data[b->len++] = c;
    b->data[b->len] = '\0';
    return 0;
}

static int sb_put(sb *b, const char *s, size_t n)
{
    if (sb_reserve(b, n)) return -1;
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
    return 0;
}

static void sb_reset(sb *b) { b->len = 0; if (b->data) b->data[0] = '\0'; }
static void sb_free(sb *b) { free(b->data); b->data = NULL; b->len = b->cap = 0; }

/* ---- helpers ------------------------------------------------------- */

const char *xml_localname(const char *name)
{
    const char *colon = strrchr(name, ':');
    return colon ? colon + 1 : name;
}

static int is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/* Encode a Unicode code point as UTF-8 into b. */
static int sb_put_utf8(sb *b, unsigned long cp)
{
    if (cp < 0x80) {
        return sb_putc(b, (char)cp);
    } else if (cp < 0x800) {
        if (sb_putc(b, (char)(0xC0 | (cp >> 6)))) return -1;
        return sb_putc(b, (char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        if (sb_putc(b, (char)(0xE0 | (cp >> 12)))) return -1;
        if (sb_putc(b, (char)(0x80 | ((cp >> 6) & 0x3F)))) return -1;
        return sb_putc(b, (char)(0x80 | (cp & 0x3F)));
    } else {
        if (sb_putc(b, (char)(0xF0 | (cp >> 18)))) return -1;
        if (sb_putc(b, (char)(0x80 | ((cp >> 12) & 0x3F)))) return -1;
        if (sb_putc(b, (char)(0x80 | ((cp >> 6) & 0x3F)))) return -1;
        return sb_putc(b, (char)(0x80 | (cp & 0x3F)));
    }
}

/*
 * Append text from [s, s+n) to `out`, decoding XML entity references.
 * Unknown entities are passed through verbatim so we never lose data.
 */
static int decode_entities(sb *out, const char *s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        if (s[i] != '&') {
            if (sb_putc(out, s[i])) return -1;
            i++;
            continue;
        }

        /* Find the terminating ';' within a small window. */
        size_t j = i + 1;
        while (j < n && j < i + 12 && s[j] != ';') j++;
        if (j >= n || s[j] != ';') {
            /* Not a well-formed reference; emit '&' literally. */
            if (sb_putc(out, '&')) return -1;
            i++;
            continue;
        }

        size_t elen = j - (i + 1);
        const char *e = s + i + 1;
        int ok = 1;

        if (elen == 3 && memcmp(e, "amp", 3) == 0) { if (sb_putc(out, '&')) return -1; }
        else if (elen == 2 && memcmp(e, "lt", 2) == 0) { if (sb_putc(out, '<')) return -1; }
        else if (elen == 2 && memcmp(e, "gt", 2) == 0) { if (sb_putc(out, '>')) return -1; }
        else if (elen == 4 && memcmp(e, "quot", 4) == 0) { if (sb_putc(out, '"')) return -1; }
        else if (elen == 4 && memcmp(e, "apos", 4) == 0) { if (sb_putc(out, '\'')) return -1; }
        else if (elen >= 2 && e[0] == '#') {
            unsigned long cp;
            char *endp;
            if (e[1] == 'x' || e[1] == 'X')
                cp = strtoul(e + 2, &endp, 16);
            else
                cp = strtoul(e + 1, &endp, 10);
            if (cp == 0) ok = 0;
            else if (sb_put_utf8(out, cp)) return -1;
        } else {
            ok = 0;
        }

        if (!ok) {
            /* Unknown entity: keep "&...;" as-is. */
            if (sb_put(out, s + i, j - i + 1)) return -1;
        }
        i = j + 1;
    }
    return 0;
}

/* ---- parser -------------------------------------------------------- */

int xml_parse(const char *data, size_t len, const xml_handler *h)
{
    const char *p = data;
    const char *end = data + len;
    sb name = {0};
    sb val = {0};
    sb text = {0};
    int rc = -1;

    while (p < end) {
        if (*p != '<') {
            /* Text run up to the next tag. */
            const char *t = p;
            while (p < end && *p != '<') p++;
            sb_reset(&text);
            if (decode_entities(&text, t, (size_t)(p - t))) goto done;
            if (h->on_text && text.len) h->on_text(h->ud, text.data, text.len);
            continue;
        }

        /* We are at '<'. */
        if (end - p >= 4 && memcmp(p, "<!--", 4) == 0) {
            const char *q = p + 4;
            while (q + 3 <= end && memcmp(q, "-->", 3) != 0) q++;
            if (q + 3 > end) goto done;       /* unterminated comment */
            p = q + 3;
            continue;
        }
        if (end - p >= 9 && memcmp(p, "<![CDATA[", 9) == 0) {
            const char *q = p + 9;
            while (q + 3 <= end && memcmp(q, "]]>", 3) != 0) q++;
            if (q + 3 > end) goto done;
            if (h->on_text && q > p + 9) h->on_text(h->ud, p + 9, (size_t)(q - (p + 9)));
            p = q + 3;
            continue;
        }
        if (end - p >= 2 && p[1] == '?') {                /* <?...?> */
            const char *q = p + 2;
            while (q + 2 <= end && memcmp(q, "?>", 2) != 0) q++;
            if (q + 2 > end) goto done;
            p = q + 2;
            continue;
        }
        if (end - p >= 2 && p[1] == '!') {                /* <!DOCTYPE ...> */
            const char *q = p + 2;
            while (q < end && *q != '>') q++;
            if (q >= end) goto done;
            p = q + 1;
            continue;
        }

        if (end - p >= 2 && p[1] == '/') {                /* end tag </name> */
            const char *q = p + 2;
            while (q < end && is_space(*q)) q++;
            const char *ns = q;
            while (q < end && *q != '>' && !is_space(*q)) q++;
            sb_reset(&name);
            if (sb_put(&name, ns, (size_t)(q - ns))) goto done;
            while (q < end && *q != '>') q++;
            if (q >= end) goto done;
            if (h->on_end) h->on_end(h->ud, name.data);
            p = q + 1;
            continue;
        }

        /* Start tag <name attr="v" ...> or self-closing <name .../>. */
        {
            const char *q = p + 1;
            const char *ns = q;
            while (q < end && *q != '>' && *q != '/' && !is_space(*q)) q++;
            sb_reset(&name);
            if (sb_put(&name, ns, (size_t)(q - ns))) goto done;
            if (name.len == 0) goto done;
            if (h->on_start) h->on_start(h->ud, name.data);

            /* Attributes. */
            int self_close = 0;
            for (;;) {
                while (q < end && is_space(*q)) q++;
                if (q >= end) goto done;
                if (*q == '>') { q++; break; }
                if (*q == '/') {
                    self_close = 1;
                    while (q < end && *q != '>') q++;
                    if (q >= end) goto done;
                    q++;
                    break;
                }

                /* Attribute name. */
                const char *as = q;
                while (q < end && *q != '=' && *q != '>' && !is_space(*q)) q++;
                sb attr = {0};
                if (sb_put(&attr, as, (size_t)(q - as))) { sb_free(&attr); goto done; }

                while (q < end && is_space(*q)) q++;
                if (q < end && *q == '=') {
                    q++;
                    while (q < end && is_space(*q)) q++;
                    char quote = (q < end) ? *q : 0;
                    if (quote == '"' || quote == '\'') {
                        q++;
                        const char *vs = q;
                        while (q < end && *q != quote) q++;
                        if (q >= end) { sb_free(&attr); goto done; }
                        sb_reset(&val);
                        if (decode_entities(&val, vs, (size_t)(q - vs))) { sb_free(&attr); goto done; }
                        q++;
                        if (h->on_attr) h->on_attr(h->ud, attr.data, val.data);
                    }
                }
                sb_free(&attr);
            }

            if (self_close && h->on_end) h->on_end(h->ud, name.data);
            p = q;
            continue;
        }
    }

    rc = 0;
done:
    sb_free(&name);
    sb_free(&val);
    sb_free(&text);
    return rc;
}
