/*
 * layout3d.c — Anchor-based 3D graph layout with local optimization.
 *
 * Strategy: structured first, then refined.
 *   1. Place nodes on a ring by cluster key:
 *      - DIR mode: first 3 directory components (organizational structure)
 *      - LOUVAIN mode: community detection on call edges (functional modules)
 *   2. Assign z from call depth (entry points at top, callees below)
 *   3. Run GENTLE local optimization: ForceAtlas2 with strong anchor springs
 *      that keep nodes near their initial positions while untangling overlaps
 *
 * The result: clean separated clusters (from the ring) with locally
 * optimized intra-cluster positions (from the force simulation).
 */
#include "foundation/constants.h"
#include "ui/layout3d.h"
#include "foundation/log.h"

#include <yyjson/yyjson.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ── Constants ────────────────────────────────────────────────── */

#define DEFAULT_MAX_NODES 50000
#define BH_THETA 1.2f

/* Local optimization: gentle, preserves structure */
#define LOCAL_REPULSION 8.0f
#define LOCAL_ATTRACTION 1.0f
#define LOCAL_ANCHOR_K 0.25f /* how strongly nodes stick to their anchor */
#define LOCAL_ITERATIONS 40
#define Z_DEPTH_SPACING 50.0f /* gentle z-layering per call depth */

/* ── Node colors/sizes ────────────────────────────────────────── */

/* Stellar spectral type colors — maps node degree to star color.
 * Follows real Hertzsprung-Russell distribution:
 *   M (red dwarf, 76% of stars) → low-degree leaf nodes
 *   K (orange)                  → slightly connected
 *   G (yellow, like our Sun)    → moderately connected
 *   F (yellow-white)            → well-connected
 *   A (white)                   → highly connected
 *   B (blue-white)              → hub nodes
 *   O (blue giant, 0.00003%)    → mega-hubs
 */
static uint32_t stellar_color(int degree) {
    if (degree <= 1)
        return 0xff6050; /* M — red dwarf */
    if (degree <= 3)
        return 0xff8855; /* late K — orange-red */
    if (degree <= 5)
        return 0xffa060; /* K — orange */
    if (degree <= 8)
        return 0xffc070; /* early K — warm orange */
    if (degree <= 12)
        return 0xffe080; /* G — yellow (Sun-like) */
    if (degree <= 18)
        return 0xfff0c0; /* F — yellow-white */
    if (degree <= 25)
        return 0xfff8e8; /* late A — warm white */
    if (degree <= 35)
        return 0xe8e8ff; /* A — white-blue */
    if (degree <= 50)
        return 0xc0d0ff; /* B — blue-white */
    return 0x80a0ff;     /* O — blue giant */
}

/* label-based colors removed — using stellar_color(degree) for graph rendering.
 * Label colors are handled in the frontend (lib/colors.ts) for sidebar/tooltips. */

/* Community color — maps a Louvain community id to a distinct hex color.
 *
 * Uses golden-ratio hue spacing so that sequentially-numbered communities
 * are maximally separated in hue space. Fixed saturation and lightness keep
 * the palette visually uniform so no community overpowers its neighbours.
 * Used when cluster_mode == CBM_CLUSTER_LOUVAIN so that the 3D graph
 * visually matches the community structure the user sees in narratives.
 */
static uint32_t community_color(int community_id) {
    if (community_id < 0)
        return 0x808080; /* unassigned → neutral gray */

    /* 137.507764° is the golden-ratio complement — each step maximally
     * separates from the previous one around the hue circle. */
    float hue = fmodf((float)community_id * 137.507764f, 360.0f);
    const float s = 0.70f;
    const float l = 0.58f;

    float c = (1.0f - fabsf(2.0f * l - 1.0f)) * s;
    float h1 = hue / 60.0f;
    float x = c * (1.0f - fabsf(fmodf(h1, 2.0f) - 1.0f));
    float r1, g1, b1;
    if (h1 < 1.0f) {
        r1 = c; g1 = x; b1 = 0;
    } else if (h1 < 2.0f) {
        r1 = x; g1 = c; b1 = 0;
    } else if (h1 < 3.0f) {
        r1 = 0; g1 = c; b1 = x;
    } else if (h1 < 4.0f) {
        r1 = 0; g1 = x; b1 = c;
    } else if (h1 < 5.0f) {
        r1 = x; g1 = 0; b1 = c;
    } else {
        r1 = c; g1 = 0; b1 = x;
    }
    float m = l - c / 2.0f;
    int r = (int)((r1 + m) * 255.0f + 0.5f);
    int g = (int)((g1 + m) * 255.0f + 0.5f);
    int b = (int)((b1 + m) * 255.0f + 0.5f);
    if (r < 0) { r = 0; }
    if (r > 255) { r = 255; }
    if (g < 0) { g = 0; }
    if (g > 255) { g = 255; }
    if (b < 0) { b = 0; }
    if (b > 255) { b = 255; }
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static float size_for_label(const char *label) {
    if (!label)
        return 4.0f;
    if (strcmp(label, "Project") == 0)
        return 20.0f;
    if (strcmp(label, "Package") == 0)
        return 15.0f;
    if (strcmp(label, "Module") == 0)
        return 15.0f;
    if (strcmp(label, "Folder") == 0)
        return 12.0f;
    if (strcmp(label, "File") == 0)
        return 8.0f;
    if (strcmp(label, "Class") == 0)
        return 6.0f;
    if (strcmp(label, "Interface") == 0)
        return 6.0f;
    if (strcmp(label, "Function") == 0)
        return 4.0f;
    if (strcmp(label, "Method") == 0)
        return 4.0f;
    return 4.0f;
}

static uint32_t fnv1a(const char *s) {
    uint32_t h = 2166136261u;
    if (!s)
        return h;
    while (*s) {
        h ^= (uint8_t)*s++;
        h *= 16777619u;
    }
    return h;
}

static float rand_float(uint32_t *seed) {
    *seed = (*seed) * 1103515245u + 12345u;
    return (float)((*seed >> 16) & 0x7FFF) / 32768.0f - 0.5f;
}

/* ── Barnes-Hut Octree ────────────────────────────────────────── */

typedef struct octree_node {
    float cx, cy, cz, total_mass, half_size, ox, oy, oz;
    int body_index;
    float body_mass;
    struct octree_node *children[8];
} octree_node_t;

static octree_node_t *octree_new(float ox, float oy, float oz, float half) {
    octree_node_t *n = calloc(CBM_ALLOC_ONE, sizeof(*n));
    if (!n)
        return NULL;
    n->ox = ox;
    n->oy = oy;
    n->oz = oz;
    n->half_size = half;
    n->body_index = -1;
    return n;
}
static void octree_free(octree_node_t *n) {
    if (!n)
        return;
    for (int i = 0; i < 8; i++)
        octree_free(n->children[i]);
    free(n);
}
static int octant(octree_node_t *n, float x, float y, float z) {
    return ((x >= n->ox) ? 1 : 0) | ((y >= n->oy) ? 2 : 0) | ((z >= n->oz) ? 4 : 0);
}
static void child_center(octree_node_t *n, int o, float *cx, float *cy, float *cz) {
    float q = n->half_size * 0.5f;
    *cx = n->ox + ((o & 1) ? q : -q);
    *cy = n->oy + ((o & 2) ? q : -q);
    *cz = n->oz + ((o & 4) ? q : -q);
}
static void octree_insert(octree_node_t *n, int idx, float x, float y, float z, float mass) {
    if (n->total_mass == 0.0f && n->body_index == -1) {
        n->body_index = idx;
        n->body_mass = mass;
        n->cx = x;
        n->cy = y;
        n->cz = z;
        n->total_mass = mass;
        return;
    }
    if (n->body_index >= 0) {
        int oi = n->body_index;
        float ox = n->cx, oy = n->cy, oz = n->cz, om = n->body_mass;
        n->body_index = -1;
        int o = octant(n, ox, oy, oz);
        if (!n->children[o]) {
            float a, b, c;
            child_center(n, o, &a, &b, &c);
            n->children[o] = octree_new(a, b, c, n->half_size * 0.5f);
        }
        if (n->children[o])
            octree_insert(n->children[o], oi, ox, oy, oz, om);
    }
    float nm = n->total_mass + mass;
    n->cx = (n->cx * n->total_mass + x * mass) / nm;
    n->cy = (n->cy * n->total_mass + y * mass) / nm;
    n->cz = (n->cz * n->total_mass + z * mass) / nm;
    n->total_mass = nm;
    int o = octant(n, x, y, z);
    if (!n->children[o]) {
        float a, b, c;
        child_center(n, o, &a, &b, &c);
        n->children[o] = octree_new(a, b, c, n->half_size * 0.5f);
    }
    if (n->children[o])
        octree_insert(n->children[o], idx, x, y, z, mass);
}
static void octree_repulse(octree_node_t *n, float px, float py, float pz, float mm, int si,
                           float kr, float *fx, float *fy, float *fz) {
    if (!n || n->total_mass == 0.0f || n->body_index == si)
        return;
    float dx = px - n->cx, dy = py - n->cy, dz = pz - n->cz;
    float d = sqrtf(dx * dx + dy * dy + dz * dz);
    if (n->body_index >= 0 || (n->half_size * 2.0f / (d + 0.001f)) < BH_THETA) {
        if (d < 0.01f)
            d = 0.01f;
        float f = kr * mm * n->total_mass / d;
        *fx += f * dx / d;
        *fy += f * dy / d;
        *fz += f * dz / d;
        return;
    }
    for (int i = 0; i < 8; i++)
        octree_repulse(n->children[i], px, py, pz, mm, si, kr, fx, fy, fz);
}

/* ── Body with anchor ─────────────────────────────────────────── */

typedef struct {
    float x, y, z;
    float ax, ay, az; /* anchor position (from ring layout) */
    float fx, fy, fz;
    float mass;
} body_t;

/* ── Local optimization (gentle, anchor-preserving) ───────────── */

static void local_optimize(body_t *b, int n, const int *es, const int *ed, int ne) {
    for (int iter = 0; iter < LOCAL_ITERATIONS; iter++) {
        for (int i = 0; i < n; i++) {
            b[i].fx = 0;
            b[i].fy = 0;
            b[i].fz = 0;
        }

        /* Bounding box */
        float mnx = 1e9f, mny = 1e9f, mnz = 1e9f, mxx = -1e9f, mxy = -1e9f, mxz = -1e9f;
        for (int i = 0; i < n; i++) {
            if (b[i].x < mnx)
                mnx = b[i].x;
            if (b[i].y < mny)
                mny = b[i].y;
            if (b[i].z < mnz)
                mnz = b[i].z;
            if (b[i].x > mxx)
                mxx = b[i].x;
            if (b[i].y > mxy)
                mxy = b[i].y;
            if (b[i].z > mxz)
                mxz = b[i].z;
        }
        float half = fmaxf(fmaxf(mxx - mnx, mxy - mny), mxz - mnz) * 0.5f + 1.0f;

        /* Repulsion (Barnes-Hut) */
        octree_node_t *root =
            octree_new((mnx + mxx) * 0.5f, (mny + mxy) * 0.5f, (mnz + mxz) * 0.5f, half);
        if (!root)
            break;
        for (int i = 0; i < n; i++)
            octree_insert(root, i, b[i].x, b[i].y, b[i].z, b[i].mass);
        for (int i = 0; i < n; i++)
            octree_repulse(root, b[i].x, b[i].y, b[i].z, b[i].mass, i, LOCAL_REPULSION, &b[i].fx,
                           &b[i].fy, &b[i].fz);
        octree_free(root);

        /* Attraction (edges) */
        for (int e = 0; e < ne; e++) {
            int s = es[e], t = ed[e];
            if (s < 0 || s >= n || t < 0 || t >= n)
                continue;
            float dx = b[t].x - b[s].x, dy = b[t].y - b[s].y, dz = b[t].z - b[s].z;
            b[s].fx += dx * LOCAL_ATTRACTION;
            b[s].fy += dy * LOCAL_ATTRACTION;
            b[s].fz += dz * LOCAL_ATTRACTION;
            b[t].fx -= dx * LOCAL_ATTRACTION;
            b[t].fy -= dy * LOCAL_ATTRACTION;
            b[t].fz -= dz * LOCAL_ATTRACTION;
        }

        /* Anchor spring: pull back toward initial ring position */
        for (int i = 0; i < n; i++) {
            b[i].fx += (b[i].ax - b[i].x) * LOCAL_ANCHOR_K * b[i].mass;
            b[i].fy += (b[i].ay - b[i].y) * LOCAL_ANCHOR_K * b[i].mass;
            b[i].fz += (b[i].az - b[i].z) * LOCAL_ANCHOR_K * b[i].mass;
        }

        /* Apply with capped displacement */
        for (int i = 0; i < n; i++) {
            float fm = sqrtf(b[i].fx * b[i].fx + b[i].fy * b[i].fy + b[i].fz * b[i].fz);
            float speed = 1.0f;
            if (speed * fm > 8.0f)
                speed = 8.0f / (fm + 0.001f);
            b[i].x += b[i].fx * speed;
            b[i].y += b[i].fy * speed;
            b[i].z += b[i].fz * speed;
        }
    }
}

/* ── Call depth via BFS ───────────────────────────────────────── */

static void compute_call_depth(int n, const int *es, const int *ed, int ne, const char **labels,
                               int *depth) {
    for (int i = 0; i < n; i++)
        depth[i] = -1;
    int *q = malloc((size_t)n * sizeof(int));
    int head = 0, tail = 0;
    if (!q)
        return;

    /* Entry points at depth 0 */
    for (int i = 0; i < n; i++) {
        if (labels[i] && (strcmp(labels[i], "Route") == 0 || strcmp(labels[i], "File") == 0 ||
                          strcmp(labels[i], "Module") == 0 || strcmp(labels[i], "Package") == 0)) {
            depth[i] = 0;
            q[tail++] = i;
        }
    }
    if (tail == 0) {
        int *in_d = calloc((size_t)n, sizeof(int));
        if (in_d) {
            for (int e = 0; e < ne; e++) {
                int t = ed[e];
                if (t >= 0 && t < n)
                    in_d[t]++;
            }
            for (int i = 0; i < n; i++)
                if (in_d[i] == 0) {
                    depth[i] = 0;
                    q[tail++] = i;
                }
            free(in_d);
        }
    }
    while (head < tail) {
        int c = q[head++], cd = depth[c];
        for (int e = 0; e < ne; e++)
            if (es[e] == c) {
                int t = ed[e];
                if (t >= 0 && t < n && depth[t] == -1) {
                    depth[t] = cd + SKIP_ONE;
                    q[tail++] = t;
                }
            }
    }
    for (int i = 0; i < n; i++)
        if (depth[i] == -1)
            depth[i] = 0;
    free(q);
}

/* ── Helpers ──────────────────────────────────────────────────── */

static void free_edge_array(cbm_edge_t *edges, int count) {
    if (!edges)
        return;
    for (int i = 0; i < count; i++) {
        free((void *)edges[i].project);
        free((void *)edges[i].type);
        free((void *)edges[i].properties_json);
    }
    free(edges);
}

/* ── Node ID → index map (for O(log n) edge filtering) ───────── */

typedef struct {
    int64_t id;
    int idx;
} node_id_entry_t;

static int cmp_node_id_entry(const void *a, const void *b) {
    int64_t da = ((const node_id_entry_t *)a)->id;
    int64_t db = ((const node_id_entry_t *)b)->id;
    return (da > db) - (da < db);
}

static int find_node_index(const node_id_entry_t *map, int count, int64_t id) {
    int lo = 0;
    int hi = count - SKIP_ONE;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / PAIR_LEN;
        if (map[mid].id == id) {
            return map[mid].idx;
        }
        if (map[mid].id < id) {
            lo = mid + SKIP_ONE;
        } else {
            hi = mid - SKIP_ONE;
        }
    }
    return CBM_NOT_FOUND;
}

/* ── Public API ───────────────────────────────────────────────── */

cbm_layout_result_t *cbm_layout_compute(cbm_store_t *store, const char *project,
                                        cbm_layout_level_t level, const char *center_node,
                                        int radius, int max_nodes,
                                        cbm_cluster_mode_t cluster_mode,
                                        cbm_color_mode_t color_mode,
                                        bool force_optimize) {
    if (!store || !project)
        return NULL;
    if (max_nodes <= 0)
        max_nodes = DEFAULT_MAX_NODES;
    (void)center_node;
    (void)radius;
    (void)level;

    /* 1. Query nodes */
    cbm_search_params_t params;
    memset(&params, 0, sizeof(params));
    params.project = project;
    params.limit = max_nodes;
    params.min_degree = -1;
    params.max_degree = -1;

    cbm_search_output_t search_out;
    memset(&search_out, 0, sizeof(search_out));
    if (cbm_store_search(store, &params, &search_out) != CBM_STORE_OK)
        return calloc(CBM_ALLOC_ONE, sizeof(cbm_layout_result_t));

    int n = search_out.count, total_count = search_out.total;
    if (n == 0) {
        cbm_store_search_free(&search_out);
        cbm_layout_result_t *r = calloc(CBM_ALLOC_ONE, sizeof(*r));
        if (r)
            r->total_nodes = total_count;
        return r;
    }

    /* 2. Build sorted node-ID → index map for O(log n) edge filtering */
    node_id_entry_t *id_map = malloc((size_t)n * sizeof(node_id_entry_t));
    if (!id_map) {
        cbm_store_search_free(&search_out);
        cbm_layout_result_t *r = calloc(CBM_ALLOC_ONE, sizeof(*r));
        if (r) {
            r->total_nodes = total_count;
        }
        return r;
    }
    for (int i = 0; i < n; i++) {
        id_map[i].id = search_out.results[i].node.id;
        id_map[i].idx = i;
    }
    qsort(id_map, (size_t)n, sizeof(node_id_entry_t), cmp_node_id_entry);

    /* 3. Query edges — filter during fetch via binary search (O(e log n)) */
    int *deg = calloc((size_t)n, sizeof(int));
    int mapped = 0;
    int edge_cap = CBM_SZ_256;
    cbm_edge_t *all_edges = malloc((size_t)edge_cap * sizeof(cbm_edge_t));
    int *es = malloc((size_t)edge_cap * sizeof(int));
    int *ed = malloc((size_t)edge_cap * sizeof(int));
    cbm_schema_info_t schema;
    memset(&schema, 0, sizeof(schema));
    if (deg && all_edges && es && ed &&
        cbm_store_get_schema(store, project, &schema) == CBM_STORE_OK) {
        for (int t = 0; t < schema.edge_type_count; t++) {
            cbm_edge_t *te = NULL;
            int tc = 0;
            if (cbm_store_find_edges_by_type(store, project, schema.edge_types[t].type, &te, &tc) ==
                CBM_STORE_OK) {
                for (int e = 0; e < tc; e++) {
                    int si = find_node_index(id_map, n, te[e].source_id);
                    int di = find_node_index(id_map, n, te[e].target_id);
                    if (si >= 0 && di >= 0) {
                        if (mapped >= edge_cap) {
                            int nc = edge_cap * PAIR_LEN;
                            cbm_edge_t *te2 = realloc(all_edges, (size_t)nc * sizeof(cbm_edge_t));
                            int *ts = realloc(es, (size_t)nc * sizeof(int));
                            int *td = realloc(ed, (size_t)nc * sizeof(int));
                            if (!te2 || !ts || !td) {
                                if (te2)
                                    all_edges = te2;
                                if (ts)
                                    es = ts;
                                if (td)
                                    ed = td;
                                free_edge_array(te + e, tc - e);
                                goto edges_done;
                            }
                            all_edges = te2;
                            es = ts;
                            ed = td;
                            edge_cap = nc;
                        }
                        all_edges[mapped] = te[e];
                        memset(&te[e], 0, sizeof(cbm_edge_t));
                        es[mapped] = si;
                        ed[mapped] = di;
                        deg[si]++;
                        deg[di]++;
                        mapped++;
                    } else {
                        free((void *)te[e].project);
                        free((void *)te[e].type);
                        free((void *)te[e].properties_json);
                    }
                }
                free(te);
            }
        }
    edges_done:
        cbm_store_schema_free(&schema);
    }
    free(id_map);

    /* 4. Call depth for z-axis */
    int *cdepth = calloc((size_t)n, sizeof(int));
    const char **lbls = malloc((size_t)n * sizeof(char *));
    if (lbls) {
        for (int i = 0; i < n; i++)
            lbls[i] = search_out.results[i].node.label;
        if (cdepth)
            compute_call_depth(n, es, ed, mapped, lbls, cdepth);
        free(lbls);
    }

    /* 4b. Louvain community detection (when requested by layout OR by coloring) */
    int *louvain_community = NULL;
    bool need_louvain = (cluster_mode == CBM_CLUSTER_LOUVAIN) ||
                        (color_mode == CBM_COLOR_LOUVAIN);
    if (need_louvain && mapped > 0) {
        /* Build edge list for Louvain from the already-fetched edges */
        int64_t *node_ids = malloc((size_t)n * sizeof(int64_t));
        cbm_louvain_edge_t *lv_edges = malloc((size_t)mapped * sizeof(cbm_louvain_edge_t));
        if (node_ids && lv_edges) {
            for (int i = 0; i < n; i++)
                node_ids[i] = search_out.results[i].node.id;
            for (int e = 0; e < mapped; e++) {
                lv_edges[e].src = search_out.results[es[e]].node.id;
                lv_edges[e].dst = search_out.results[ed[e]].node.id;
            }
            cbm_louvain_result_t *lv_out = NULL;
            int lv_count = 0;
            if (cbm_louvain(node_ids, n, lv_edges, mapped, &lv_out, &lv_count) == CBM_STORE_OK &&
                lv_out) {
                louvain_community = calloc((size_t)n, sizeof(int));
                if (louvain_community) {
                    /* Map Louvain results back to node indices */
                    for (int r = 0; r < lv_count; r++) {
                        for (int i = 0; i < n; i++) {
                            if (node_ids[i] == lv_out[r].node_id) {
                                louvain_community[i] = lv_out[r].community;
                                break;
                            }
                        }
                    }
                }
                free(lv_out);
            }
        }
        free(node_ids);
        free(lv_edges);
    }

    /* 5. Seed positions: ring by cluster key + z from call depth */
    body_t *bodies = calloc((size_t)n, sizeof(body_t));
    cbm_layout_result_t *result = calloc(CBM_ALLOC_ONE, sizeof(*result));
    if (!result || !bodies) {
        free(bodies);
        free(deg);
        free(es);
        free(ed);
        free(cdepth);
        free(louvain_community);
        cbm_layout_free(result);
        free_edge_array(all_edges, mapped);
        cbm_store_search_free(&search_out);
        return NULL;
    }
    result->nodes = calloc((size_t)n, sizeof(cbm_layout_node_t));
    result->node_count = n;
    result->total_nodes = total_count;

    for (int i = 0; i < n; i++) {
        const cbm_node_t *sn = &search_out.results[i].node;

        uint32_t h;
        if (cluster_mode == CBM_CLUSTER_LOUVAIN && louvain_community) {
            /* Louvain layout mode: hash the community ID for ring placement */
            char comm_key[32];
            snprintf(comm_key, sizeof(comm_key), "comm_%d", louvain_community[i]);
            h = fnv1a(comm_key);
        } else {
            /* Directory mode: hash first 3 dir components */
            const char *fp = sn->file_path ? sn->file_path : "";
            char ck[CBM_SZ_256] = {0};
            {
                const char *p = fp;
                int sl = 0, ki = 0;
                while (*p && ki < 255) {
                    if (*p == '/') {
                        sl++;
                        if (sl >= 3)
                            break;
                    }
                    ck[ki++] = *p++;
                }
            }
            h = fnv1a(ck);
        }

        float angle = ((float)(h & 0xFFFF) / 65535.0f) * 6.2832f;
        float r = 500.0f + ((float)((h >> 16) & 0xFF) / 255.0f) * 250.0f;

        uint32_t seed = fnv1a(sn->qualified_name);
        float jitter = 40.0f;
        float px = r * cosf(angle) + rand_float(&seed) * jitter;
        float py = r * sinf(angle) + rand_float(&seed) * jitter;
        float pz = cdepth ? -(float)cdepth[i] * Z_DEPTH_SPACING : 0;

        bodies[i].x = px;
        bodies[i].y = py;
        bodies[i].z = pz;
        bodies[i].ax = px;
        bodies[i].ay = py;
        bodies[i].az = pz; /* anchor = initial pos */
        bodies[i].mass = (float)(deg[i] + 1);

        result->nodes[i].id = sn->id;
        result->nodes[i].label = sn->label ? strdup(sn->label) : NULL;
        result->nodes[i].name = sn->name ? strdup(sn->name) : NULL;
        result->nodes[i].qualified_name = sn->qualified_name ? strdup(sn->qualified_name) : NULL;
        result->nodes[i].file_path = sn->file_path ? strdup(sn->file_path) : NULL;
        /* Coloring is independent of layout mode. Louvain coloring uses the
         * community id via a golden-ratio hue palette; stellar coloring maps
         * degree to a Hertzsprung-Russell-style spectral type. */
        if (color_mode == CBM_COLOR_LOUVAIN && louvain_community) {
            result->nodes[i].color = community_color(louvain_community[i]);
        } else {
            result->nodes[i].color = stellar_color(deg[i]);
        }
        /* Size: base from label + boost from degree (hubs are bigger stars) */
        float base_size = size_for_label(sn->label);
        float deg_boost = (deg[i] > 5) ? fminf((float)deg[i] * 0.3f, 10.0f) : 0;
        result->nodes[i].size = base_size + deg_boost;
    }

    /* 6. Gentle local optimization (anchor-preserving) — skipped when the
     * caller has asked for raw seeded positions so the ring layout stays
     * intact for camera-driven screenshots. */
    if (force_optimize) {
        local_optimize(bodies, n, es, ed, mapped);
    }

    /* 7. Copy positions */
    for (int i = 0; i < n; i++) {
        result->nodes[i].x = bodies[i].x;
        result->nodes[i].y = bodies[i].y;
        result->nodes[i].z = bodies[i].z;
    }

    /* 8. Output edges */
    if (mapped > 0) {
        result->edges = calloc((size_t)mapped, sizeof(cbm_layout_edge_t));
        result->edge_count = mapped;
        for (int e = 0; e < mapped && result->edges; e++) {
            result->edges[e].source = search_out.results[es[e]].node.id;
            result->edges[e].target = search_out.results[ed[e]].node.id;
            result->edges[e].type = all_edges[e].type ? strdup(all_edges[e].type) : NULL;
        }
    }

    free(bodies);
    free(deg);
    free(es);
    free(ed);
    free(cdepth);
    free(louvain_community);
    free_edge_array(all_edges, mapped);
    cbm_store_search_free(&search_out);
    return result;
}

void cbm_layout_free(cbm_layout_result_t *r) {
    if (!r)
        return;
    for (int i = 0; i < r->node_count; i++) {
        free((void *)r->nodes[i].label);
        free((void *)r->nodes[i].name);
        free((void *)r->nodes[i].qualified_name);
        free((void *)r->nodes[i].file_path);
    }
    free(r->nodes);
    for (int i = 0; i < r->edge_count; i++)
        free((void *)r->edges[i].type);
    free(r->edges);
    free(r);
}

char *cbm_layout_to_json(const cbm_layout_result_t *r) {
    if (!r)
        return NULL;
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_val *na = yyjson_mut_arr(doc);
    for (int i = 0; i < r->node_count; i++) {
        yyjson_mut_val *nd = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_int(doc, nd, "id", r->nodes[i].id);
        double nx = isfinite(r->nodes[i].x) ? (double)r->nodes[i].x : 0.0;
        double ny = isfinite(r->nodes[i].y) ? (double)r->nodes[i].y : 0.0;
        double nz = isfinite(r->nodes[i].z) ? (double)r->nodes[i].z : 0.0;
        yyjson_mut_obj_add_real(doc, nd, "x", nx);
        yyjson_mut_obj_add_real(doc, nd, "y", ny);
        yyjson_mut_obj_add_real(doc, nd, "z", nz);
        if (r->nodes[i].label)
            yyjson_mut_obj_add_str(doc, nd, "label", r->nodes[i].label);
        if (r->nodes[i].name)
            yyjson_mut_obj_add_str(doc, nd, "name", r->nodes[i].name);
        if (r->nodes[i].file_path)
            yyjson_mut_obj_add_str(doc, nd, "file_path", r->nodes[i].file_path);
        double nsz = isfinite(r->nodes[i].size) ? (double)r->nodes[i].size : 1.0;
        yyjson_mut_obj_add_real(doc, nd, "size", nsz);
        char hex[CBM_SZ_8];
        snprintf(hex, sizeof(hex), "#%06x", r->nodes[i].color);
        yyjson_mut_obj_add_strcpy(doc, nd, "color", hex);
        yyjson_mut_arr_append(na, nd);
    }
    yyjson_mut_obj_add_val(doc, root, "nodes", na);

    yyjson_mut_val *ea = yyjson_mut_arr(doc);
    for (int i = 0; i < r->edge_count; i++) {
        yyjson_mut_val *ed = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_int(doc, ed, "source", r->edges[i].source);
        yyjson_mut_obj_add_int(doc, ed, "target", r->edges[i].target);
        if (r->edges[i].type)
            yyjson_mut_obj_add_str(doc, ed, "type", r->edges[i].type);
        yyjson_mut_arr_append(ea, ed);
    }
    yyjson_mut_obj_add_val(doc, root, "edges", ea);
    yyjson_mut_obj_add_int(doc, root, "total_nodes", r->total_nodes);

    size_t len = 0;
    yyjson_write_err write_err = {0};
    char *json =
        yyjson_mut_write_opts(doc, YYJSON_WRITE_ALLOW_INVALID_UNICODE, NULL, &len, &write_err);
    yyjson_mut_doc_free(doc);
    if (!json) {
        char code[CBM_SZ_32];
        snprintf(code, sizeof(code), "%u", write_err.code);
        cbm_log_error("layout.json.fail", "code", code, "msg",
                      write_err.msg ? write_err.msg : "unknown");
    }
    return json;
}
