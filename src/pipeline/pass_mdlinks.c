/*
 * pass_mdlinks.c — Extract REFERENCES edges from markdown links to source code.
 *
 * Scans markdown files for inline links [text](path) that reference
 * source code files within the same project. Creates REFERENCES edges
 * from the markdown Module node to the target code node.
 *
 * REFERENCES is a weak edge — any markdown link produces one. Stronger,
 * authoritative claims (a doc saying "this IS the behaviour of X") are
 * emitted as SPECIFIES / PRESCRIBES edges by pass_behaviourdoc.
 *
 * Resolution:
 *   [text](relative/path.go)        → Module node for that file
 *   [text](relative/path.go#L42)    → node at that line in that file
 *   [text](relative/path.go#Name)   → node named "Name" in that file
 *
 * Skips external URLs (http://, https://, etc.) and fenced code blocks.
 *
 * Depends on: pass_definitions having populated the graph buffer with nodes.
 */
#include "foundation/constants.h"
#include "pipeline/pipeline.h"
#include "pipeline/pipeline_internal.h"
#include "graph_buffer/graph_buffer.h"
#include "foundation/log.h"
#include "discover/discover.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ── Helpers ──────────────────────────────────────────────────────── */

/* Read entire file into heap buffer. Caller frees. */
static char *mdl_read_file(const char *path, int *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 10 * 1024 * 1024) { /* 10 MB limit */
        fclose(f);
        return NULL;
    }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t nr = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[nr] = '\0';
    *out_len = (int)nr;
    return buf;
}

/* Check if a path looks like an external URL. */
static bool is_external_url(const char *path) {
    return strncmp(path, "http://", 7) == 0 || strncmp(path, "https://", 8) == 0 ||
           strncmp(path, "ftp://", 6) == 0 || strncmp(path, "mailto:", 7) == 0 ||
           strncmp(path, "tel:", 4) == 0;
}

/* Resolve a relative link path against the directory of a markdown file.
 * Writes the resolved relative-to-repo-root path into out_buf.
 * Returns true on success. */
static bool resolve_link_path(const char *md_rel_path, const char *link_target, char *out_buf,
                              size_t buf_size) {
    /* Get the directory of the markdown file */
    char dir[1024];
    strncpy(dir, md_rel_path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
    char *last_slash = strrchr(dir, '/');
    if (last_slash) {
        *(last_slash + 1) = '\0'; /* keep trailing slash */
    } else {
        dir[0] = '\0'; /* file is at repo root */
    }

    /* Concatenate dir + link_target */
    char combined[2048];
    snprintf(combined, sizeof(combined), "%s%s", dir, link_target);

    /* Normalize: resolve . and .. components */
    char *parts[128];
    int nparts = 0;
    char *tmp = combined;
    char *tok;

    /* Tokenize by / */
    while ((tok = strsep(&tmp, "/")) != NULL) {
        if (strcmp(tok, ".") == 0 || tok[0] == '\0') {
            continue;
        } else if (strcmp(tok, "..") == 0) {
            if (nparts > 0)
                nparts--;
        } else {
            if (nparts < 128)
                parts[nparts++] = tok;
        }
    }

    /* Rebuild normalized path */
    out_buf[0] = '\0';
    for (int i = 0; i < nparts; i++) {
        if (i > 0)
            strncat(out_buf, "/", buf_size - strlen(out_buf) - 1);
        strncat(out_buf, parts[i], buf_size - strlen(out_buf) - 1);
    }

    return out_buf[0] != '\0';
}

/* ── Link scanner ─────────────────────────────────────────────────── */

/* Represents a parsed markdown link. */
typedef struct {
    int line;              /* 1-based line number in the markdown file */
    char path[512];        /* resolved relative path to target file */
    char anchor[256];      /* optional #anchor (without the #), or "" */
} mdl_link_t;

/* Scan markdown source for inline links. Skips fenced code blocks.
 * Returns count of links found, writes up to max_links into out. */
static int scan_md_links(const char *source, int source_len, const char *md_rel_path,
                         mdl_link_t *out, int max_links) {
    int count = 0;
    int line = 1;
    bool in_code_block = false;
    const char *p = source;
    const char *end = source + source_len;

    while (p < end && count < max_links) {
        /* Track line numbers */
        if (*p == '\n') {
            line++;
            p++;
            continue;
        }

        /* Track fenced code blocks (``` or ~~~) */
        if ((*p == '`' || *p == '~') && (p == source || *(p - 1) == '\n')) {
            char fence_char = *p;
            int fence_len = 0;
            const char *fp = p;
            while (fp < end && *fp == fence_char) {
                fence_len++;
                fp++;
            }
            if (fence_len >= 3) {
                in_code_block = !in_code_block;
                p = fp;
                continue;
            }
        }

        if (in_code_block) {
            p++;
            continue;
        }

        /* Look for [text](path) pattern */
        /* Skip images: ![alt](path) — still useful, but skip for now */
        if (*p == '[' && (p == source || *(p - 1) != '!')) {
            /* Find closing ] */
            const char *close_bracket = NULL;
            const char *bp = p + 1;
            int depth = 1;
            while (bp < end && depth > 0) {
                if (*bp == '[')
                    depth++;
                else if (*bp == ']')
                    depth--;
                if (*bp == '\n')
                    break; /* links don't span lines in CommonMark */
                bp++;
            }
            if (depth == 0) {
                close_bracket = bp - 1;
            }

            if (close_bracket && (close_bracket + 1) < end && *(close_bracket + 1) == '(') {
                /* Found [text]( — now find the closing ) */
                const char *paren_start = close_bracket + 2;
                const char *pp = paren_start;
                while (pp < end && *pp != ')' && *pp != '\n' && *pp != ' ' && *pp != '"') {
                    pp++;
                }

                if (pp < end) {
                    /* Extract the raw link target */
                    int link_len = (int)(pp - paren_start);
                    if (link_len > 0 && link_len < 500) {
                        char raw_target[512];
                        memcpy(raw_target, paren_start, (size_t)link_len);
                        raw_target[link_len] = '\0';

                        /* Skip external URLs */
                        if (!is_external_url(raw_target)) {
                            /* Split path and anchor */
                            char *hash = strchr(raw_target, '#');
                            char anchor[256] = "";
                            if (hash) {
                                strncpy(anchor, hash + 1, sizeof(anchor) - 1);
                                anchor[sizeof(anchor) - 1] = '\0';
                                *hash = '\0'; /* truncate path at # */
                            }

                            /* Only process if there's a path (not just #anchor) */
                            if (raw_target[0] != '\0') {
                                char resolved[1024];
                                if (resolve_link_path(md_rel_path, raw_target, resolved,
                                                      sizeof(resolved))) {
                                    out[count].line = line;
                                    strncpy(out[count].path, resolved,
                                            sizeof(out[count].path) - 1);
                                    out[count].path[sizeof(out[count].path) - 1] = '\0';
                                    strncpy(out[count].anchor, anchor,
                                            sizeof(out[count].anchor) - 1);
                                    out[count].anchor[sizeof(out[count].anchor) - 1] = '\0';
                                    count++;
                                }
                            }
                        }
                    }
                    /* Skip past the closing paren */
                    while (pp < end && *pp != ')' && *pp != '\n')
                        pp++;
                    if (pp < end && *pp == ')')
                        pp++;
                    p = pp;
                    continue;
                }
            }
        }
        p++;
    }
    return count;
}

/* ── Target resolution ────────────────────────────────────────────── */

/* Try to find the target node for a markdown link.
 * Returns the node ID or 0 if not found. */
static int64_t resolve_target(cbm_gbuf_t *gbuf, const char *project, const mdl_link_t *link) {
    /* Build module QN for the target file path */
    char *module_qn = cbm_pipeline_fqn_module(project, link->path);
    if (!module_qn)
        return 0;

    const cbm_gbuf_node_t *target = cbm_gbuf_find_by_qn(gbuf, module_qn);
    free(module_qn);

    if (!target)
        return 0;

    /* If no anchor, return the module node */
    if (link->anchor[0] == '\0')
        return target->id;

    /* Try line-number anchor: #L<number> */
    if ((link->anchor[0] == 'L' || link->anchor[0] == 'l') && isdigit(link->anchor[1])) {
        int target_line = atoi(link->anchor + 1);
        if (target_line > 0) {
            /* Search for a node at this line in the target file.
             * Iterate nodes by name to find one in the right file at the right line.
             * This is O(N) over all nodes — acceptable for the scale of markdown links. */
            /* For now, we don't have a file_path index. Use the module node. */
            /* TODO: Add file_path index to graph buffer for line-based resolution */
            return target->id;
        }
    }

    /* Try name anchor: #FunctionName */
    const cbm_gbuf_node_t **matches = NULL;
    int match_count = 0;
    if (cbm_gbuf_find_by_name(gbuf, link->anchor, &matches, &match_count) == 0 &&
        match_count > 0) {
        /* Find the match in the target file */
        for (int i = 0; i < match_count; i++) {
            if (matches[i]->file_path && strcmp(matches[i]->file_path, link->path) == 0) {
                return matches[i]->id;
            }
        }
        /* If no file match, return first match as best effort */
        return matches[0]->id;
    }

    /* Fall back to module node */
    return target->id;
}

/* ── Pass entry point ─────────────────────────────────────────────── */

int cbm_pipeline_pass_mdlinks(cbm_pipeline_ctx_t *ctx, const cbm_file_info_t *files,
                              int file_count) {

    int total_edges = 0;
    enum { MAX_LINKS_PER_FILE = 500 };
    mdl_link_t *links = malloc(MAX_LINKS_PER_FILE * sizeof(mdl_link_t));
    if (!links)
        return -1;

    for (int fi = 0; fi < file_count; fi++) {
        if (cbm_pipeline_check_cancel(ctx))
            break;

        /* Only process markdown files */
        if (files[fi].language != CBM_LANG_MARKDOWN)
            continue;

        /* Read file */
        int src_len = 0;
        char *source = mdl_read_file(files[fi].path, &src_len);
        if (!source)
            continue;

        /* Scan for links */
        int link_count = scan_md_links(source, src_len, files[fi].rel_path, links,
                                       MAX_LINKS_PER_FILE);


        if (link_count > 0) {
            /* Get the source Module node for this markdown file */
            char *src_qn = cbm_pipeline_fqn_module(ctx->project_name, files[fi].rel_path);
            const cbm_gbuf_node_t *src_node = src_qn ? cbm_gbuf_find_by_qn(ctx->gbuf, src_qn) : NULL;
            free(src_qn);

            if (src_node) {
                for (int li = 0; li < link_count; li++) {
                    int64_t target_id = resolve_target(ctx->gbuf, ctx->project_name, &links[li]);
                    if (target_id > 0 && target_id != src_node->id) {
                        char props[256];
                        snprintf(props, sizeof(props), "{\"line\":%d,\"anchor\":\"%s\"}",
                                 links[li].line, links[li].anchor);
                        cbm_gbuf_insert_edge(ctx->gbuf, src_node->id, target_id, "REFERENCES",
                                             props);
                        total_edges++;
                    }
                }
            }
        }

        free(source);
    }

    free(links);

    if (total_edges > 0) {
        char count_str[32];
        snprintf(count_str, sizeof(count_str), "%d", total_edges);
        cbm_log_info("pass.mdlinks", "edges", count_str);
    }

    return 0;
}
