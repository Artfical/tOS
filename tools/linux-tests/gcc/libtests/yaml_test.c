#include "common.h"
#include <yaml.h>
int main(void)
{
    const char *doc =
        "name: tOS\n"
        "version: 616\n"
        "tags: [os, x86-64, \"quoted: yes\"]\n"
        "nested:\n"
        "  list:\n"
        "    - a: 1\n"
        "      b: two\n"
        "    - c: |\n"
        "        multi\n"
        "        line\n"
        "anchors: &base {x: 1}\n"
        "copy: *base\n";
    yaml_parser_t p; yaml_event_t ev;
    CHECK(yaml_parser_initialize(&p), "parser init");
    yaml_parser_set_input_string(&p, (const unsigned char *)doc, strlen(doc));
    int scalars = 0, maps = 0, seqs = 0, aliases = 0, done = 0, err = 0; int found_multi = 0, found_quoted = 0;
    while (!done) {
        if (!yaml_parser_parse(&p, &ev)) { err = 1; break; }
        switch (ev.type) {
            case YAML_SCALAR_EVENT:
                scalars++;
                if (!strcmp((char *)ev.data.scalar.value, "multi\nline\n")) found_multi = 1;
                if (!strcmp((char *)ev.data.scalar.value, "quoted: yes")) found_quoted = 1;
                break;
            case YAML_MAPPING_START_EVENT: maps++; break;
            case YAML_SEQUENCE_START_EVENT: seqs++; break;
            case YAML_ALIAS_EVENT: aliases++; break;
            case YAML_STREAM_END_EVENT: done = 1; break;
            default: break;
        }
        yaml_event_delete(&ev);
    }
    CHECK(!err, "the document parses");
    CHECK(maps == 5 && seqs == 2 && aliases == 1, "5 mappings, 2 sequences, 1 alias");
    CHECK(scalars == 20, "20 scalars");
    CHECK(found_multi, "a block scalar keeps its lines");
    CHECK(found_quoted, "a quoted scalar keeps its colon");
    yaml_parser_delete(&p);
    /* a broken document is refused */
    yaml_parser_initialize(&p);
    const char *bad = "a: [1, 2\nb: }";
    yaml_parser_set_input_string(&p, (const unsigned char *)bad, strlen(bad));
    int bad_ok = 1;
    for (int i = 0; i < 40 && bad_ok; i++) { if (!yaml_parser_parse(&p, &ev)) bad_ok = 0; else { int end = ev.type == YAML_STREAM_END_EVENT; yaml_event_delete(&ev); if (end) break; } }
    CHECK(!bad_ok, "a broken document is reported");
    yaml_parser_delete(&p);
    /* the emitter writes a document that parses again */
    yaml_emitter_t em; yaml_event_t e; unsigned char buf[2000]; size_t written = 0;
    yaml_emitter_initialize(&em); yaml_emitter_set_output_string(&em, buf, sizeof(buf), &written);
    yaml_stream_start_event_initialize(&e, YAML_UTF8_ENCODING); yaml_emitter_emit(&em, &e);
    yaml_document_start_event_initialize(&e, NULL, NULL, NULL, 1); yaml_emitter_emit(&em, &e);
    yaml_mapping_start_event_initialize(&e, NULL, (yaml_char_t *)YAML_MAP_TAG, 1, YAML_BLOCK_MAPPING_STYLE); yaml_emitter_emit(&em, &e);
    for (int i = 0; i < 3; i++) {
        char k[16], v[16]; sprintf(k, "key%d", i); sprintf(v, "value %d", i * i);
        yaml_scalar_event_initialize(&e, NULL, (yaml_char_t *)YAML_STR_TAG, (yaml_char_t *)k, -1, 1, 1, YAML_PLAIN_SCALAR_STYLE); yaml_emitter_emit(&em, &e);
        yaml_scalar_event_initialize(&e, NULL, (yaml_char_t *)YAML_STR_TAG, (yaml_char_t *)v, -1, 1, 1, YAML_PLAIN_SCALAR_STYLE); yaml_emitter_emit(&em, &e);
    }
    yaml_mapping_end_event_initialize(&e); yaml_emitter_emit(&em, &e);
    yaml_document_end_event_initialize(&e, 1); yaml_emitter_emit(&em, &e);
    yaml_stream_end_event_initialize(&e); yaml_emitter_emit(&em, &e);
    yaml_emitter_delete(&em);
    buf[written] = 0;
    CHECK(strstr((char *)buf, "key2: value 4") != NULL, "the emitter writes key2: value 4");
    yaml_parser_initialize(&p); yaml_parser_set_input_string(&p, buf, written);
    int pairs = 0, perr = 0;
    for (;;) { if (!yaml_parser_parse(&p, &ev)) { perr = 1; break; } int end = ev.type == YAML_STREAM_END_EVENT; if (ev.type == YAML_SCALAR_EVENT) pairs++; yaml_event_delete(&ev); if (end) break; }
    CHECK(!perr && pairs == 6, "and it parses again");
    yaml_parser_delete(&p);
    int ma, mi, pa; yaml_get_version(&ma, &mi, &pa);
    printf("libyaml %d.%d.%d\n", ma, mi, pa);
    DONE("libyaml");
}
