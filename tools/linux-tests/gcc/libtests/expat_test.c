#include "common.h"
#include <expat.h>
typedef struct { int elems, attrs, chars, depth, maxdepth, cdata; char last[64]; } St;
static void XMLCALL start(void *u, const XML_Char *name, const XML_Char **atts)
{ St *s = u; s->elems++; s->depth++; if (s->depth > s->maxdepth) s->maxdepth = s->depth; for (; *atts; atts += 2) s->attrs++; strncpy(s->last, name, 63); }
static void XMLCALL end(void *u, const XML_Char *name) { St *s = u; (void)name; s->depth--; }
static void XMLCALL text(void *u, const XML_Char *c, int len) { St *s = u; (void)c; s->chars += len; }
static void XMLCALL cds(void *u) { ((St *)u)->cdata++; }
int main(void)
{
    const char *xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE root [<!ENTITY who \"tOS\">]>\n"
        "<root a=\"1\" b=\"2\"><item id=\"x\">Hello &who; &amp; &#65;</item>"
        "<group><item id=\"y\"><![CDATA[<raw> & data]]></item><item/></group><!-- note --></root>";
    St s; memset(&s, 0, sizeof(s));
    XML_Parser p = XML_ParserCreate("UTF-8");
    XML_SetUserData(p, &s);
    XML_SetElementHandler(p, start, end);
    XML_SetCharacterDataHandler(p, text);
    XML_SetStartCdataSectionHandler(p, cds);
    /* feed it in small pieces: the parser must cope with a tag split anywhere */
    size_t len = strlen(xml), off = 0;
    int ok = 1;
    while (off < len && ok) { size_t k = len - off < 7 ? len - off : 7; ok = XML_Parse(p, xml + off, (int)k, off + k >= len) != XML_STATUS_ERROR; off += k; }
    CHECK(ok, "the document parses");
    CHECK(s.elems == 5 && s.attrs == 4 && s.maxdepth == 3, "5 elements, 4 attributes, depth 3");
    CHECK(s.cdata == 1, "one CDATA section");
    CHECK(s.chars == (int)strlen("Hello tOS & A") + (int)strlen("<raw> & data"), "character data with entities expanded");
    XML_ParserFree(p);
    /* a malformed document is refused, with a line number */
    p = XML_ParserCreate(NULL);
    int r = XML_Parse(p, "<a>\n<b></a>", 11, 1);
    CHECK(r == XML_STATUS_ERROR && XML_GetErrorCode(p) == XML_ERROR_TAG_MISMATCH && XML_GetCurrentLineNumber(p) == 2, "a mismatched tag is reported on line 2");
    XML_ParserFree(p);
    /* namespaces */
    p = XML_ParserCreateNS("UTF-8", '|');
    St s2; memset(&s2, 0, sizeof(s2)); XML_SetUserData(p, &s2); XML_SetElementHandler(p, start, end);
    const char *ns = "<x:r xmlns:x=\"urn:t\"><x:c/></x:r>";
    CHECK(XML_Parse(p, ns, (int)strlen(ns), 1) == XML_STATUS_OK && !strcmp(s2.last, "urn:t|c"), "namespaced names");
    XML_ParserFree(p);
    printf("expat %s\n", XML_ExpatVersion());
    DONE("expat");
}
