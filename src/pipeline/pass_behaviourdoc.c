/*
 * pass_behaviourdoc.c — Promote markdown files with behaviour-claim
 * frontmatter into first-class :BehaviourDoc nodes and emit authoritative
 * SPECIFIES / PRESCRIBES edges to the code they describe.
 *
 * A BehaviourDoc is a markdown file that opens with a YAML frontmatter
 * block of the form:
 *
 *     ---
 *     specifies:
 *       - qn: "internal.business.cases.priority_order.HighestPriorityOrder"
 *       - qn: "internal.business.cases.priority_order.PriorityOrder"
 *     prescribes:
 *       - qn: "internal.business.cases.priority_order_test.Test_HighestPriorityOrder"
 *     ---
 *
 * Qualified names are repo-relative; this pass prepends the current
 * project name before resolving them via the graph buffer.
 *
 * Relationship to pass_mdlinks (REFERENCES edges): pass_mdlinks emits
 * weak "contains a markdown link to" edges from any markdown file. A
 * BehaviourDoc is a stronger claim: the doc authors explicitly say
 * "this is the behaviour of <target>". Both can coexist on the same
 * file — a doc can reference many things and specify a subset.
 *
 * Depends on: pass_definitions having populated the graph buffer with
 * Module, Class, Method, and Function nodes.
 */
#include "foundation/constants.h"
#include "pipeline/pipeline.h"
#include "pipeline/pipeline_internal.h"
#include "graph_buffer/graph_buffer.h"
#include "foundation/log.h"
#include "discover/discover.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Constants ──────────────────────────────────────────────────────── */

enum {
    BD_MAX_QNS_PER_KEY = 64,    /* cap per-key list length */
    BD_MAX_QN_LEN = 512,        /* single QN character cap */
    BD_MAX_FRONTMATTER = 16384, /* cap on frontmatter block we will scan */
    BD_MAX_FILE_BYTES = 10 * 1024 * 1024,
};

/* ── File helpers ──────────────────────────────────────────────────── */

/* Read the first `max_bytes` of a file into a heap buffer. Caller frees. */
static char *bd_read_head(const char *path, int max_bytes, int *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    char *buf = malloc((size_t)max_bytes + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t nr = fread(buf, 1, (size_t)max_bytes, f);
    fclose(f);
    buf[nr] = '\0';
    *out_len = (int)nr;
    return buf;
}

/* ── Frontmatter parser ────────────────────────────────────────────── */

typedef struct {
    char *qns_specifies[BD_MAX_QNS_PER_KEY];
    int n_specifies;
    char *qns_prescribes[BD_MAX_QNS_PER_KEY];
    int n_prescribes;
} bd_frontmatter_t;

static void bd_frontmatter_init(bd_frontmatter_t *fm) {
    fm->n_specifies = 0;
    fm->n_prescribes = 0;
}

static void bd_frontmatter_free(bd_frontmatter_t *fm) {
    for (int i = 0; i < fm->n_specifies; i++) {
        free(fm->qns_specifies[i]);
    }
    for (int i = 0; i < fm->n_prescribes; i++) {
        free(fm->qns_prescribes[i]);
    }
    fm->n_specifies = 0;
    fm->n_prescribes = 0;
}

/* Strip surrounding whitespace in place; return pointer into the same buffer. */
static char *bd_trim(char *s) {
    while (*s && isspace((unsigned char)*s)) {
        s++;
    }
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)*(end - 1))) {
        end--;
    }
    *end = '\0';
    return s;
}

/* Strip surrounding "..." or '...' quotes in place. */
static char *bd_unquote(char *s) {
    size_t n = strlen(s);
    if (n >= 2 && ((s[0] == '"' && s[n - 1] == '"') || (s[0] == '\'' && s[n - 1] == '\''))) {
        s[n - 1] = '\0';
        return s + 1;
    }
    return s;
}

/* Count the number of leading spaces (not tabs) on a line. */
static int bd_leading_spaces(const char *line) {
    int n = 0;
    while (line[n] == ' ') {
        n++;
    }
    return n;
}

/* Locate the frontmatter block bounded by leading `---\n` and a matching
 * `---\n` (or `---\r\n`). Returns pointer to byte after the opening fence
 * and sets *block_len to bytes up to (but not including) the closing
 * fence. Returns NULL if no complete block exists. */
static const char *bd_locate_frontmatter(const char *src, int src_len, int *block_len) {
    /* Tolerate a UTF-8 BOM. */
    int off = 0;
    if (src_len >= 3 && (unsigned char)src[0] == 0xEF && (unsigned char)src[1] == 0xBB &&
        (unsigned char)src[2] == 0xBF) {
        off = 3;
    }
    /* Opening fence must be at file start and end with a newline. */
    if (src_len - off < 4 || strncmp(src + off, "---", 3) != 0) {
        return NULL;
    }
    const char *p = src + off + 3;
    /* Skip optional \r then require \n. */
    if (*p == '\r') {
        p++;
    }
    if (*p != '\n') {
        return NULL;
    }
    p++;
    const char *body = p;
    /* Scan for a line that contains only "---" followed by end-of-line. */
    const char *end = src + src_len;
    while (p < end) {
        const char *line = p;
        const char *nl = memchr(line, '\n', (size_t)(end - line));
        const char *line_end = nl ? nl : end;
        /* Trim trailing \r. */
        const char *content_end = line_end;
        if (content_end > line && *(content_end - 1) == '\r') {
            content_end--;
        }
        if ((content_end - line) == 3 && strncmp(line, "---", 3) == 0) {
            *block_len = (int)(line - body);
            return body;
        }
        if (!nl) {
            break;
        }
        p = nl + 1;
    }
    return NULL;
}

/* Push a QN (already trimmed, unquoted) into the list if non-empty and
 * within the cap. Returns true if stored. */
static bool bd_push_qn(char **list, int *count, const char *qn) {
    if (!qn || !*qn || *count >= BD_MAX_QNS_PER_KEY) {
        return false;
    }
    size_t n = strlen(qn);
    if (n == 0 || n > BD_MAX_QN_LEN) {
        return false;
    }
    list[*count] = strdup(qn);
    if (!list[*count]) {
        return false;
    }
    (*count)++;
    return true;
}

/* Attempt to extract a QN from a YAML list item line like:
 *   "  - qn: \"a.b.c\""
 *   "  - qn: a.b.c"
 *   "  - a.b.c"        (bare string form)
 * Writes the raw (still-quoted-ok) value into out_buf (size out_sz).
 * Returns true if anything was extracted. */
static bool bd_extract_list_item(const char *line, char *out_buf, size_t out_sz) {
    /* Locate the '-' list marker. */
    const char *dash = strchr(line, '-');
    if (!dash) {
        return false;
    }
    const char *p = dash + 1;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    /* Optional "qn:" key. */
    if (strncmp(p, "qn", 2) == 0) {
        const char *q = p + 2;
        while (*q == ' ' || *q == '\t') {
            q++;
        }
        if (*q == ':') {
            p = q + 1;
            while (*p == ' ' || *p == '\t') {
                p++;
            }
        }
    }
    /* Copy until line end. */
    size_t i = 0;
    while (*p && *p != '\r' && *p != '\n' && i + 1 < out_sz) {
        out_buf[i++] = *p++;
    }
    out_buf[i] = '\0';
    /* Strip trailing inline comment (" # ..."). */
    char *hash = strchr(out_buf, '#');
    if (hash && hash > out_buf && *(hash - 1) == ' ') {
        *hash = '\0';
    }
    return i > 0;
}

/* Parse the frontmatter block into `fm`. The block is the text between
 * opening and closing `---` fences, without either fence.
 * Recognises YAML mapping keys `specifies:` and `prescribes:` at column 0
 * (no indent) followed by indented list items. Unknown keys are skipped. */
static void bd_parse_frontmatter_block(const char *block, int block_len, bd_frontmatter_t *fm) {
    /* Copy into a writable NUL-terminated buffer. */
    if (block_len < 0) {
        block_len = 0;
    }
    if (block_len > BD_MAX_FRONTMATTER) {
        block_len = BD_MAX_FRONTMATTER;
    }
    char *buf = malloc((size_t)block_len + 1);
    if (!buf) {
        return;
    }
    memcpy(buf, block, (size_t)block_len);
    buf[block_len] = '\0';

    /* Active list: 0 = none, 1 = specifies, 2 = prescribes. */
    int active = 0;
    int list_indent = -1; /* required leading-space count for list items */

    char *p = buf;
    while (*p) {
        /* Extract a single line (into [line .. line_end)). */
        char *line = p;
        char *nl = strchr(p, '\n');
        if (nl) {
            *nl = '\0';
            p = nl + 1;
        } else {
            p = line + strlen(line);
        }
        /* Trim trailing \r. */
        size_t ll = strlen(line);
        if (ll > 0 && line[ll - 1] == '\r') {
            line[ll - 1] = '\0';
        }
        /* Skip blank or pure-comment lines. */
        char *trim_probe = line;
        while (*trim_probe == ' ' || *trim_probe == '\t') {
            trim_probe++;
        }
        if (*trim_probe == '\0' || *trim_probe == '#') {
            continue;
        }

        int indent = bd_leading_spaces(line);

        /* A mapping key at column 0 starts a new section. */
        if (indent == 0) {
            active = 0;
            list_indent = -1;
            char *colon = strchr(line, ':');
            if (!colon) {
                continue;
            }
            size_t keylen = (size_t)(colon - line);
            char keybuf[32];
            if (keylen >= sizeof(keybuf)) {
                continue;
            }
            memcpy(keybuf, line, keylen);
            keybuf[keylen] = '\0';
            char *key = bd_trim(keybuf);
            if (strcmp(key, "specifies") == 0) {
                active = 1;
            } else if (strcmp(key, "prescribes") == 0) {
                active = 2;
            }
            /* Inline flow form `specifies: [ qn: "x" ]` is not supported; the
             * pilot uses the indented block form. */
            continue;
        }

        if (active == 0) {
            continue;
        }

        /* List items must start with '-' at a consistent indent > 0. */
        char *content = line + indent;
        if (*content != '-') {
            /* Not a list item — end of the current list. */
            active = 0;
            list_indent = -1;
            continue;
        }
        if (list_indent < 0) {
            list_indent = indent;
        } else if (indent != list_indent) {
            /* Wonky indentation — stop treating this as a list. */
            active = 0;
            list_indent = -1;
            continue;
        }

        char value[BD_MAX_QN_LEN + 32];
        if (!bd_extract_list_item(line, value, sizeof(value))) {
            continue;
        }
        char *qn = bd_unquote(bd_trim(value));
        if (active == 1) {
            bd_push_qn(fm->qns_specifies, &fm->n_specifies, qn);
        } else if (active == 2) {
            bd_push_qn(fm->qns_prescribes, &fm->n_prescribes, qn);
        }
    }
    free(buf);
}

/* ── Target resolution ─────────────────────────────────────────────── */

/* Look up a repo-relative QN against the project graph. Prepends the
 * project name ("project.<qn>") because indexed QNs are project-qualified.
 * Returns the matching node ID, or 0 if not found. */
static int64_t bd_resolve_qn(cbm_gbuf_t *gbuf, const char *project, const char *rel_qn) {
    size_t need = strlen(project) + 1 + strlen(rel_qn) + 1;
    char *full = malloc(need);
    if (!full) {
        return 0;
    }
    snprintf(full, need, "%s.%s", project, rel_qn);
    const cbm_gbuf_node_t *node = cbm_gbuf_find_by_qn(gbuf, full);
    free(full);
    return node ? node->id : 0;
}

/* ── Pass entry point ──────────────────────────────────────────────── */

int cbm_pipeline_pass_behaviourdoc(cbm_pipeline_ctx_t *ctx, const cbm_file_info_t *files,
                                   int file_count) {
    if (!ctx || !files) {
        return 0;
    }

    int promoted = 0;
    int specifies_edges = 0;
    int prescribes_edges = 0;
    int unresolved = 0;

    for (int fi = 0; fi < file_count; fi++) {
        if (cbm_pipeline_check_cancel(ctx)) {
            break;
        }
        if (files[fi].language != CBM_LANG_MARKDOWN) {
            continue;
        }

        int src_len = 0;
        char *head = bd_read_head(files[fi].path, BD_MAX_FRONTMATTER, &src_len);
        if (!head) {
            continue;
        }
        int block_len = 0;
        const char *block = bd_locate_frontmatter(head, src_len, &block_len);
        if (!block) {
            free(head);
            continue;
        }

        bd_frontmatter_t fm;
        bd_frontmatter_init(&fm);
        bd_parse_frontmatter_block(block, block_len, &fm);
        free(head);

        if (fm.n_specifies == 0 && fm.n_prescribes == 0) {
            bd_frontmatter_free(&fm);
            continue;
        }

        /* Look up the source Module node for this markdown file. */
        char *src_qn = cbm_pipeline_fqn_module(ctx->project_name, files[fi].rel_path);
        const cbm_gbuf_node_t *src_node = src_qn ? cbm_gbuf_find_by_qn(ctx->gbuf, src_qn) : NULL;
        if (!src_node) {
            free(src_qn);
            bd_frontmatter_free(&fm);
            continue;
        }

        /* Promote the module to a BehaviourDoc. */
        if (cbm_gbuf_relabel_node(ctx->gbuf, src_qn, "BehaviourDoc") == 0) {
            promoted++;
        }
        int64_t src_id = src_node->id;
        free(src_qn);

        /* Emit SPECIFIES edges. */
        for (int i = 0; i < fm.n_specifies; i++) {
            int64_t tgt = bd_resolve_qn(ctx->gbuf, ctx->project_name, fm.qns_specifies[i]);
            if (tgt > 0) {
                cbm_gbuf_insert_edge(ctx->gbuf, src_id, tgt, "SPECIFIES", "{}");
                specifies_edges++;
            } else {
                unresolved++;
                cbm_log_warn("pass.behaviourdoc", "event", "unresolved_specifies", "doc",
                             files[fi].rel_path, "qn", fm.qns_specifies[i]);
            }
        }

        /* Emit PRESCRIBES edges. Targets are expected to be test Functions;
         * log a warning if the resolved node isn't labelled Function/Method. */
        for (int i = 0; i < fm.n_prescribes; i++) {
            int64_t tgt = bd_resolve_qn(ctx->gbuf, ctx->project_name, fm.qns_prescribes[i]);
            if (tgt > 0) {
                const cbm_gbuf_node_t *tn = cbm_gbuf_find_by_id(ctx->gbuf, tgt);
                if (tn && tn->label &&
                    strcmp(tn->label, "Function") != 0 && strcmp(tn->label, "Method") != 0) {
                    cbm_log_warn("pass.behaviourdoc", "event", "prescribes_non_function",
                                 "doc", files[fi].rel_path, "qn", fm.qns_prescribes[i],
                                 "label", tn->label);
                }
                cbm_gbuf_insert_edge(ctx->gbuf, src_id, tgt, "PRESCRIBES", "{}");
                prescribes_edges++;
            } else {
                unresolved++;
                cbm_log_warn("pass.behaviourdoc", "event", "unresolved_prescribes", "doc",
                             files[fi].rel_path, "qn", fm.qns_prescribes[i]);
            }
        }

        bd_frontmatter_free(&fm);
    }

    if (promoted > 0 || specifies_edges > 0 || prescribes_edges > 0 || unresolved > 0) {
        char pbuf[16], sbuf[16], rbuf[16], ubuf[16];
        snprintf(pbuf, sizeof(pbuf), "%d", promoted);
        snprintf(sbuf, sizeof(sbuf), "%d", specifies_edges);
        snprintf(rbuf, sizeof(rbuf), "%d", prescribes_edges);
        snprintf(ubuf, sizeof(ubuf), "%d", unresolved);
        cbm_log_info("pass.behaviourdoc", "promoted", pbuf, "specifies", sbuf, "prescribes", rbuf,
                     "unresolved", ubuf);
    }
    return 0;
}

/* ── Testable hooks ────────────────────────────────────────────────── */

/* Thin re-exports of the internal frontmatter helpers, intended only for
 * test_pipeline.c. The tokens are stored into caller-provided string
 * arrays so the unit tests don't need visibility into the internal
 * struct. */

int cbm_pipeline_behaviourdoc_parse_for_test(const char *src, int src_len,
                                             char **out_specifies, int *n_specifies,
                                             char **out_prescribes, int *n_prescribes,
                                             int max_per_key) {
    *n_specifies = 0;
    *n_prescribes = 0;
    int block_len = 0;
    const char *block = bd_locate_frontmatter(src, src_len, &block_len);
    if (!block) {
        return -1;
    }
    bd_frontmatter_t fm;
    bd_frontmatter_init(&fm);
    bd_parse_frontmatter_block(block, block_len, &fm);
    for (int i = 0; i < fm.n_specifies && *n_specifies < max_per_key; i++) {
        out_specifies[(*n_specifies)++] = strdup(fm.qns_specifies[i]);
    }
    for (int i = 0; i < fm.n_prescribes && *n_prescribes < max_per_key; i++) {
        out_prescribes[(*n_prescribes)++] = strdup(fm.qns_prescribes[i]);
    }
    bd_frontmatter_free(&fm);
    return 0;
}
