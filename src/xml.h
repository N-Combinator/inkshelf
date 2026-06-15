/*
 * xml.h — minimal SAX-style XML pull parser.
 *
 * inkshelf only needs to read OPDS Atom feeds, which are small, well-formed
 * XML documents. Rather than drag libxml2/tinyxml2 out of an SDK sysroot we
 * can't inspect ahead of time (and keep the whole app dependency-free pure
 * C), we parse with this tiny self-contained tokenizer. It is deliberately a
 * subset: elements, attributes, text, CDATA, comments, processing
 * instructions and the standard entity references. That covers OPDS.
 *
 * The parser is callback driven. Element/attribute names are reported with
 * their namespace prefix intact (e.g. "dc:creator"); callers match on the
 * local name via xml_localname().
 */

#ifndef INKSHELF_XML_H
#define INKSHELF_XML_H

#include <stddef.h>

typedef struct {
    void *ud;                                              /* user data */
    void (*on_start)(void *ud, const char *name);          /* <name ...> */
    void (*on_attr)(void *ud, const char *name, const char *value);
    void (*on_text)(void *ud, const char *text, size_t len);
    void (*on_end)(void *ud, const char *name);            /* </name> or /> */
} xml_handler;

/* Parse the whole document. Returns 0 on success, -1 on malformed input. */
int xml_parse(const char *data, size_t len, const xml_handler *h);

/* Return a pointer to the local name within `name` (the part after the last
 * ':'), or `name` itself if there is no prefix. */
const char *xml_localname(const char *name);

#endif /* INKSHELF_XML_H */
