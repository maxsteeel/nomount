#ifndef NM_H
#define NM_H

#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stddef.h>
#include <errno.h>
#include <sys/syscall.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <stdlib.h>
#include <limits.h>

/* --- DEFS --- */
#define NOMOUNT_MAGIC_SIG 0x4E4F4D4F554E54ULL
#define PATH_MAX  4096
#define NM_EINTR  4

enum {
    NM_CMD_UNSPEC = 0,
    NM_CMD_GET_VERSION,
    NM_CMD_ADD_RULE,
    NM_CMD_DEL_RULE,
    NM_CMD_ADD_UID,
    NM_CMD_DEL_UID,
    NM_CMD_CLEAR_ALL,
    NM_CMD_CLEAR_RULES,
    NM_CMD_CLEAR_UIDS,
    NM_CMD_GET_LIST,
    NM_CMD_GET_UIDS,
    NM_CMD_BLOCK_ISOLATED_UIDS,
    NM_CMD_GET_ISOLATED_STATE,
};

enum nm_cli_action {
    ACTION_NONE = 0,
    ACTION_RULE_ADD,
    ACTION_RULE_DEL,
    ACTION_RULE_LIST,
    ACTION_RULE_CLEAR,
    ACTION_UID_ADD,
    ACTION_UID_DEL,
    ACTION_UID_LIST,
    ACTION_UID_CLEAR,
    ACTION_CLEAR_ALL,
    ACTION_VERSION,
    ACTION_BLOCK_ISOLATED_UIDS,
    ACTION_MODULE_LOAD,
    ACTION_MODULE_UNLOAD,
    ACTION_MODULE_LIST,
};

struct nm_payload {
    unsigned long long magic;
    unsigned int cmd;
    unsigned int target_uid;
    int status;
    unsigned int arg1;
    unsigned int data_size;
    char buffer[4068];
} __attribute__((packed));


struct nm_workspace {
    struct nm_payload payload;
    char cwd[PATH_MAX];
} __attribute__((aligned(4096)));

_Static_assert(sizeof(struct nm_payload) == 4096, "payload must occupy one page");
_Static_assert(__builtin_offsetof(struct nm_workspace, payload) == 0, "payload must start at the workspace boundary");

struct nm_rule_hdr {
    unsigned int flags;
    unsigned int uid;
    unsigned int uid_end;
    unsigned int deny_start;
    unsigned int deny_end;
    unsigned short v_len;
    unsigned short r_len;
} __attribute__((packed));

struct nm_del_hdr {
    unsigned int uid;
    unsigned int uid_end;
    unsigned int deny_start;
    unsigned int deny_end;
    unsigned short v_len;
} __attribute__((packed));

struct nm_args {
    enum nm_cli_action action;
    int data_start_idx;
    int is_json;
    int is_whiteout;
    int is_opaque;
    unsigned int target_uid;
    unsigned int target_uid_end;
    unsigned int deny_start;
    unsigned int deny_end;
    const char *mod_path;
    const char *mod_targets;     /* space-separated, NULL = default */
    const char *mod_only;        /* comma-separated, NULL = all */
};

#define NM_DEFAULT_TARGETS \
    "system system_ext vendor odm product apex oem optics prism mi_ext " \
    "my_bigball my_carrier my_company my_engineering my_heytap my_manifest " \
    "my_preload my_product my_region my_reserve my_stock"

struct nm_walk {
    struct nm_payload *payload;
    char *cursor;
    char *end;
    const char *mod_root;
    size_t mod_root_len;
    int status;
    int count;
};

/* --- UTILS --- */
#define noinline __attribute__((noinline))

static noinline void parse_uid_range(const char *s, unsigned int *start, unsigned int *end) {
    unsigned int first = 0, last = 0;
    while (*s && *s != '-') first = (first << 3) + (first << 1) + (*s++ - '0');
    if (*s == '-') s++;
    if (*s) while (*s) last = (last << 3) + (last << 1) + (*s++ - '0');
    else last = first;
    if (last < first) { unsigned int t = first; first = last; last = t; }
    *start = first;
    *end = last;
}

static noinline void parse_arguments(long argc, char **argv, struct nm_args *parsed_args) {
    unsigned char *p = (unsigned char *)parsed_args;
    for (unsigned long i = 0; i < sizeof(*parsed_args); i++) p[i] = 0;
    parsed_args->data_start_idx = 2;
    parsed_args->mod_path = NULL;
    parsed_args->mod_targets = NULL;
    parsed_args->mod_only = NULL;

    if (argc < 2) return;

    const char *c1 = argv[1];
    if (strcmp(c1, "rule") == 0 && argc >= 3) {
        const char *c2 = argv[2];
        if (strcmp(c2, "add") == 0) parsed_args->action = ACTION_RULE_ADD;
        else if (strcmp(c2, "del") == 0) parsed_args->action = ACTION_RULE_DEL;
        else if (strcmp(c2, "list") == 0) parsed_args->action = ACTION_RULE_LIST;
        else if (strcmp(c2, "clear") == 0) parsed_args->action = ACTION_RULE_CLEAR;
        parsed_args->data_start_idx = 3;
    } else if (strcmp(c1, "uid") == 0 && argc >= 3) {
        const char *c2 = argv[2];
        if (strcmp(c2, "add") == 0) parsed_args->action = ACTION_UID_ADD;
        else if (strcmp(c2, "del") == 0) parsed_args->action = ACTION_UID_DEL;
        else if (strcmp(c2, "list") == 0) parsed_args->action = ACTION_UID_LIST;
        else if (strcmp(c2, "clear") == 0) parsed_args->action = ACTION_UID_CLEAR;
        else if (strcmp(c2, "block_isolated") == 0) parsed_args->action = ACTION_BLOCK_ISOLATED_UIDS;
        parsed_args->data_start_idx = 3;
    } else if (strcmp(c1, "clear") == 0) {
        if (argc >= 3 && strcmp(argv[2], "rules") == 0) parsed_args->action = ACTION_RULE_CLEAR;
        else if (argc >= 3 && strcmp(argv[2], "uid") == 0) parsed_args->action = ACTION_UID_CLEAR;
        else parsed_args->action = ACTION_CLEAR_ALL;
        parsed_args->data_start_idx = (argc >= 3) ? 3 : 2;
    } else if (strcmp(c1, "module") == 0 && argc >= 3) {
        const char *c2 = argv[2];
        if (strcmp(c2, "load") == 0) parsed_args->action = ACTION_MODULE_LOAD;
        else if (strcmp(c2, "unload") == 0) parsed_args->action = ACTION_MODULE_UNLOAD;
        else if (strcmp(c2, "list") == 0) parsed_args->action = ACTION_MODULE_LIST;
        parsed_args->data_start_idx = 3;
        for (int i = 3; i < argc; i++) {
            const char *a = argv[i];
            if (!strncmp(a, "--targets=", 10)) parsed_args->mod_targets = a + 10;
            else if (!strncmp(a, "--only=", 7)) parsed_args->mod_only = a + 7;
            else if (strcmp(a, "--json") == 0) parsed_args->is_json = 1;
            else if (!parsed_args->mod_path) parsed_args->mod_path = a;
        }
    }

    /* Legacy Commands */
    else if (strcmp(c1, "add") == 0 || strcmp(c1, "a") == 0) { parsed_args->action = ACTION_RULE_ADD; }
    else if (strcmp(c1, "w") == 0 || strcmp(c1, "whiteout") == 0) { parsed_args->action = ACTION_RULE_ADD; parsed_args->is_whiteout = 1; }
    else if (strcmp(c1, "del") == 0 || strcmp(c1, "d") == 0) { parsed_args->action = ACTION_RULE_DEL; }
    else if (strcmp(c1, "block") == 0 || strcmp(c1, "b") == 0) { parsed_args->action = ACTION_UID_ADD; }
    else if (strcmp(c1, "unblock") == 0 || strcmp(c1, "u") == 0) { parsed_args->action = ACTION_UID_DEL; }
    else if (strcmp(c1, "list") == 0 || strcmp(c1, "l") == 0) {
        if (argc >= 3 && strcmp(argv[2], "uid") == 0) { parsed_args->action = ACTION_UID_LIST; parsed_args->data_start_idx = 3; }
        else { parsed_args->action = ACTION_RULE_LIST; }
    } else if (strcmp(c1, "version") == 0 || strcmp(c1, "v") == 0 || strcmp(c1, "-v") == 0) { parsed_args->action = ACTION_VERSION; }
}

static noinline void print_strn(const char *s, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(1, s + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        off += n;
    }
}
#define print_literal(s) print_strn((s), sizeof(s) - 1)

struct nm_output {
    char *buffer;
    unsigned int used;
};

static noinline void list_flush(struct nm_output *out) {
    size_t off = 0;
    while (off < out->used) {
        ssize_t n = write(1, out->buffer + off, out->used - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        off += n;
    }
    out->used = 0;
}

static noinline void list_print_strn(struct nm_output *out, const char *s, unsigned long len) {
    while (len) {
        if (out->used == PATH_MAX) list_flush(out);
        unsigned long n = PATH_MAX - out->used;
        if (n > len) n = len;
        char *p = out->buffer + out->used;
        for (unsigned long i = 0; i < n; i++) *p++ = *s++;
        out->used += n;
        len -= n;
    }
}

static noinline void list_print_json_strn(struct nm_output *out, const char *s, unsigned long len) {
    const char *start = s;
    while (len--) {
        unsigned char c = *s++;
        if (c >= 0x20 && c != '"' && c != '\\') continue;
        list_print_strn(out, start, s - start - 1);
        char escaped[6] = {'\\', 'u', '0', '0', '0' + (c >> 4), "0123456789abcdef"[c & 15]};
        if (c >= 0x20) escaped[1] = c;
        list_print_strn(out, escaped, c < 0x20 ? sizeof(escaped) : 2);
        start = s;
    }
    list_print_strn(out, start, s - start);
}

#define list_print_literal(out, s) list_print_strn((out), (s), sizeof(s) - 1)
#define list_print_literal_offset(out, s, offset) list_print_strn((out), (s) + (offset), sizeof(s) - 1 - (offset))

static noinline void list_print_uint(struct nm_output *out, unsigned int n) {
    char buf[10];
    int i = sizeof(buf);
    do {
        buf[--i] = (n % 10) + '0';
        n /= 10;
    } while (n > 0);
    list_print_strn(out, &buf[i], sizeof(buf) - i);
}

/* path resolution */
static noinline char* resolve_path(char *p, char *end, const char *cwd, const char *rel) {
    if (cwd && *rel != '/') {
        while (*cwd) {
            if (p == end) return 0;
            *p++ = *cwd++;
        }
        if (p == end) return 0;
        *p++ = '/';
    }
    while (*rel) {
        if (p == end) return 0;
        *p++ = *rel++;
    }
    return p;
}

static noinline int nm_send_payload(struct nm_payload *payload) {
    payload->status = -1;
    unsigned long ptr = (unsigned long)payload;
    syscall(SYS_add_key, "nomount", "trigger", &ptr, sizeof(ptr), -1);
    return payload->status;
}

/* --- module walk helpers --- */

static const char *const nm_partitions[] = {
    "vendor", "system_ext", "product", "odm", "apex",
    "oem", "optics", "prism", "mi_ext", NULL,
};

static int nm_is_partition(const char *p, size_t len) {
    for (int i = 0; nm_partitions[i]; i++) {
        if (strlen(nm_partitions[i]) == len && !memcmp(p, nm_partitions[i], len)) return 1;
    }
    return len > 3 && !memcmp(p, "my_", 3);
}

static noinline int nm_to_vpath(const char *rel, char *out, size_t out_size) {
    const char *p = rel;
    if (!strncmp(p, "system/", 7)) {
        const char *rest = p + 7;
        const char *slash = strchr(rest, '/');
        size_t plen = slash ? (size_t)(slash - rest) : strlen(rest);
        if (nm_is_partition(rest, plen)) p = rest;
    }
    size_t plen = strlen(p);
    if (plen + 2 > out_size) return -1;
    out[0] = '/';
    memcpy(out + 1, p, plen);
    out[plen + 1] = '\0';
    return 0;
}

static noinline int nm_path_join(char *dst, size_t dst_size, const char *a, const char *b) {
    size_t alen = strlen(a), blen = strlen(b);
    if (alen + 1 + blen + 1 > dst_size) return -1;
    memcpy(dst, a, alen);
    dst[alen] = '/';
    memcpy(dst + alen + 1, b, blen);
    dst[alen + 1 + blen] = '\0';
    return 0;
}

static noinline int nm_dir_has_content(const char *abs_path) {
    DIR *d = opendir(abs_path);
    if (!d) return 0;
    int has = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' && (e->d_name[1] == '\0' ||
            (e->d_name[1] == '.' && e->d_name[2] == '\0'))) continue;
        if (!strcmp(e->d_name, ".replace") || !strcmp(e->d_name, ".remove")) continue;
        has = 1;
        break;
    }
    closedir(d);
    return has;
}

static noinline long long nm_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static noinline int nm_flush(struct nm_walk *ctx) {
    if (ctx->cursor == (char *)ctx->payload->buffer) return 0;
    ctx->payload->data_size = ctx->cursor - (char *)ctx->payload->buffer;
    if (nm_send_payload(ctx->payload) < 0) return -1;
    ctx->payload->arg1 = 0;
    ctx->cursor = (char *)ctx->payload->buffer;
    return 0;
}

static noinline int nm_emit(struct nm_walk *ctx, unsigned int flags, const char *vpath, size_t vlen, const char *rpath, size_t rlen) {
    size_t need = sizeof(struct nm_rule_hdr) + vlen + rlen;
    if (ctx->end - ctx->cursor < (long)need) {
        if (nm_flush(ctx) < 0) return -1;
        if (ctx->end - ctx->cursor < (long)need) return -1;
    }
    struct nm_rule_hdr *h = (void *)ctx->cursor;
    h->flags = flags;
    h->uid = 0; h->uid_end = 0;
    h->deny_start = 0; h->deny_end = 0;
    h->v_len = vlen; h->r_len = rlen;
    ctx->cursor += sizeof(*h);
    if (vlen) { memcpy(ctx->cursor, vpath, vlen); ctx->cursor += vlen; }
    if (rlen) { memcpy(ctx->cursor, rpath, rlen); ctx->cursor += rlen; }
    ctx->count++;
    return 0;
}

static int nm_emit_vpath_only(struct nm_walk *ctx, unsigned int flags, const char *rel) {
    char vpath[PATH_MAX];
    if (nm_to_vpath(rel, vpath, sizeof(vpath))) return -1;
    return nm_emit(ctx, flags, vpath, strlen(vpath), NULL, 0);
}

static int nm_emit_pair(struct nm_walk *ctx, unsigned int flags, const char *rel, const char *abs) {
    char vpath[PATH_MAX];
    if (nm_to_vpath(rel, vpath, sizeof(vpath))) return -1;
    return nm_emit(ctx, flags, vpath, strlen(vpath), abs, strlen(abs));
}

/* --- module list helpers --- */

static noinline int nm_mod_in_list(const char *mod_id, const char *list) {
    size_t id_len = strlen(mod_id);
    const char *p = list;
    while (*p) {
        const char *comma = strchr(p, ',');
        size_t plen = comma ? (size_t)(comma - p) : strlen(p);
        if (plen == id_len && !memcmp(p, mod_id, id_len)) return 1;
        if (!comma) break;
        p = comma + 1;
    }
    return 0;
}

static noinline int nm_read_module_name(const char *prop_path, char *name_out, size_t name_size) {
    name_out[0] = '\0';
    int fd = open(prop_path, O_RDONLY);
    if (fd < 0) return -1;

    char buf[512];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';

    const char *p = buf;
    while (*p) {
        if (!strncmp(p, "name=", 5)) {
            const char *val = p + 5;
            const char *end = strchr(val, '\n');
            if (!end) end = buf + n;
            size_t len = end - val;
            if (len >= name_size) len = name_size - 1;
            memcpy(name_out, val, len);
            name_out[len] = '\0';
            return 0;
        }
        p = strchr(p, '\n');
        if (!p) break;
        p++;
    }
    return -1;
}

static noinline int nm_check_partition(const char *mod_path, const char *part_name) {
    char path[PATH_MAX];
    struct stat st;

    if (nm_path_join(path, sizeof(path), mod_path, part_name)) return 0;
    if (stat(path, &st) < 0 || !S_ISDIR(st.st_mode)) return 0;

    if (nm_path_join(path, sizeof(path), "", part_name) == 0 &&
        stat(path, &st) == 0 && S_ISDIR(st.st_mode))
        return 1;

    if (nm_path_join(path, sizeof(path), "/system", part_name) == 0 &&
        stat(path, &st) == 0 && S_ISDIR(st.st_mode))
        return 1;

    return 0;
}

static noinline int nm_mod_has_injectable(const char *mod_path, const char *targets) {
    char buf[PATH_MAX];
    strncpy(buf, targets, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *save = buf;
    char *p;
    while ((p = strchr(save, ' '))) {
        *p = '\0';
        if (*save && nm_check_partition(mod_path, save)) return 1;
        save = p + 1;
    }
    if (*save && nm_check_partition(mod_path, save)) return 1;
    return 0;
}

/* --- load walker --- */

static int walk_load(struct nm_walk *ctx, const char *abs_dir, const char *rel_dir) {
    DIR *d = opendir(abs_dir);
    if (!d) return 0;

    struct dirent *e;
    while ((e = readdir(d))) {
        const char *name = e->d_name;
        if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) continue;

        char abs_entry[PATH_MAX];
        char rel_entry[PATH_MAX];
        if (nm_path_join(abs_entry, sizeof(abs_entry), abs_dir, name) ||
            nm_path_join(rel_entry, sizeof(rel_entry), rel_dir, name)) continue;

        struct stat st;
        if (lstat(abs_entry, &st) < 0) continue;

        if (!strcmp(name, ".replace")) {
            char parent_abs[PATH_MAX], parent_rel[PATH_MAX];
            size_t alen = strlen(abs_entry) - 9;
            size_t rlen = strlen(rel_entry) - 9;
            if (alen >= sizeof(parent_abs) || rlen >= sizeof(parent_rel)) continue;
            memcpy(parent_abs, abs_entry, alen); parent_abs[alen] = '\0';
            memcpy(parent_rel, rel_entry, rlen); parent_rel[rlen] = '\0';
            if (nm_dir_has_content(parent_abs)) {
                if (nm_emit_pair(ctx, 8 /* NM_FLAG_OPAQUE */, parent_rel, parent_abs)) return -1;
            } else {
                if (nm_emit_vpath_only(ctx, 4 /* NM_FLAG_WHITEOUT */, parent_rel)) return -1;
            }
        } else if (!strcmp(name, ".remove")) {
            char parent_rel[PATH_MAX];
            size_t rlen = strlen(rel_entry) - 8;
            if (rlen >= sizeof(parent_rel)) continue;
            memcpy(parent_rel, rel_entry, rlen); parent_rel[rlen] = '\0';
            if (nm_emit_vpath_only(ctx, 4 /* NM_FLAG_WHITEOUT */, parent_rel)) return -1;
        } else if (S_ISDIR(st.st_mode)) {
            if (walk_load(ctx, abs_entry, rel_entry)) return -1;
        } else if (S_ISLNK(st.st_mode)) {
            struct stat tgt;
            if (stat(abs_entry, &tgt) < 0) continue;   /* broken link: skip */
            if (S_ISDIR(tgt.st_mode)) {
                if (walk_load(ctx, abs_entry, rel_entry)) return -1;
            } else {
                if (nm_emit_pair(ctx, 0, rel_entry, abs_entry)) return -1;
            }
        } else if (S_ISREG(st.st_mode)) {
            if (nm_emit_pair(ctx, 0, rel_entry, abs_entry)) return -1;
        }
    }
    closedir(d);
    return 0;
}

/* --- unload walker --- */

static int walk_unload(struct nm_walk *ctx, const char *abs_dir, const char *rel_dir) {
    DIR *d = opendir(abs_dir);
    if (!d) return 0;

    struct dirent *e;
    while ((e = readdir(d))) {
        const char *name = e->d_name;
        if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) continue;

        char abs_entry[PATH_MAX];
        char rel_entry[PATH_MAX];
        if (nm_path_join(abs_entry, sizeof(abs_entry), abs_dir, name) ||
            nm_path_join(rel_entry, sizeof(rel_entry), rel_dir, name)) continue;

        struct stat st;
        if (lstat(abs_entry, &st) < 0) continue;

        const char *rel_del = rel_entry;
        char rel_del_buf[PATH_MAX];

        if (!strcmp(name, ".replace") || !strcmp(name, ".remove")) {
            size_t strip = strlen(name) + 1;
            size_t rlen = strlen(rel_entry) - strip;
            if (rlen >= sizeof(rel_del_buf)) continue;
            memcpy(rel_del_buf, rel_entry, rlen);
            rel_del_buf[rlen] = '\0';
            rel_del = rel_del_buf;
        }

        char vpath[PATH_MAX];
        if (nm_to_vpath(rel_del, vpath, sizeof(vpath))) continue;

        size_t vlen = strlen(vpath);
        size_t need = sizeof(struct nm_del_hdr) + vlen;
        if (ctx->end - ctx->cursor < (long)need) {
            if (nm_flush(ctx) < 0) return -1;
            if (ctx->end - ctx->cursor < (long)need) return -1;
        }
        struct nm_del_hdr *h = (void *)ctx->cursor;
        h->uid = 0; h->uid_end = 0;
        h->deny_start = 0; h->deny_end = 0;
        h->v_len = vlen;
        ctx->cursor += sizeof(*h);
        memcpy(ctx->cursor, vpath, vlen);
        ctx->cursor += vlen;
        ctx->count++;

        if (S_ISDIR(st.st_mode) && walk_unload(ctx, abs_entry, rel_entry)) return -1;
    }
    closedir(d);
    return 0;
}

/* --- list walker --- */

static int walk_links(struct nm_output *out, const char *abs_dir, const char *rel_dir, const char *mod_id, int *first, int depth) {
    if (depth > 16) return 0;
    DIR *d = opendir(abs_dir);
    if (!d) return 0;

    struct dirent *e;
    while ((e = readdir(d))) {
        const char *name = e->d_name;
        if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) continue;

        char abs_entry[PATH_MAX];
        char rel_entry[PATH_MAX];
        if (nm_path_join(abs_entry, sizeof(abs_entry), abs_dir, name)) continue;
        if (nm_path_join(rel_entry, sizeof(rel_entry), rel_dir, name)) continue;

        struct stat lst;
        if (lstat(abs_entry, &lst) < 0) continue;

        if (S_ISLNK(lst.st_mode)) {
            char target[PATH_MAX];
            ssize_t tlen = readlink(abs_entry, target, sizeof(target) - 1);
            if (tlen < 0) continue;
            target[tlen] = '\0';

            struct stat tgt;
            int isdir = (stat(abs_entry, &tgt) == 0 && S_ISDIR(tgt.st_mode));

            if (!*first) list_print_literal(out, ",");
            *first = 0;
            list_print_literal(out, "\n    {\"modId\": \"");
            list_print_json_strn(out, mod_id, strlen(mod_id));
            list_print_literal(out, "\", \"virtual\": \"/");
            list_print_json_strn(out, rel_entry, strlen(rel_entry));
            list_print_literal(out, "\", \"real\": \"");
            list_print_json_strn(out, target, (size_t)tlen);
            if (isdir)
                list_print_literal(out, "\", \"dir\": true}");
            else
                list_print_literal(out, "\", \"dir\": false}");

            if (isdir) walk_links(out, abs_entry, rel_entry, mod_id, first, depth + 1);
        } else if (S_ISDIR(lst.st_mode)) {
            walk_links(out, abs_entry, rel_entry, mod_id, first, depth);
        }
    }
    closedir(d);
    return 0;
}

/* --- module target iteration --- */

static int cmd_module_load_or_unload(struct nm_workspace *ws, struct nm_args *args, int is_load) {
    const char *mod_path = args->mod_path;
    if (!mod_path) {
        print_literal("nm: module path required\n");
        return 1;
    }

    char mod_path_abs[PATH_MAX];
    if (mod_path[0] == '/') {
        strncpy(mod_path_abs, mod_path, sizeof(mod_path_abs) - 1);
        mod_path_abs[sizeof(mod_path_abs) - 1] = '\0';
    } else {
        char cwd[PATH_MAX];
        const char *cwd_p = getcwd(cwd, sizeof(cwd)) ? cwd : "/";
        if (nm_path_join(mod_path_abs, sizeof(mod_path_abs), cwd_p, mod_path)) {
            print_literal("nm: path too long\n");
            return 1;
        }
    }

    const char *targets = args->mod_targets ? args->mod_targets : NM_DEFAULT_TARGETS;
    char targets_buf[PATH_MAX];
    strncpy(targets_buf, targets, sizeof(targets_buf) - 1);
    targets_buf[sizeof(targets_buf) - 1] = '\0';

    struct nm_walk ctx = {
        .payload = &ws->payload,
        .mod_root = mod_path_abs,
        .mod_root_len = strlen(mod_path_abs),
        .status = 0,
    };
    ctx.payload->cmd = is_load ? NM_CMD_ADD_RULE : NM_CMD_DEL_RULE;
    ctx.payload->arg1 = 0;
    ctx.cursor = (char *)ctx.payload->buffer;
    ctx.end = ctx.cursor + sizeof(ctx.payload->buffer);

    long long t0 = nm_now_ms();

    char *save = targets_buf;
    char *p;
    while ((p = strchr(save, ' '))) { *p = '\0'; if (*save) {
        char abs_part[PATH_MAX];
        if (nm_path_join(abs_part, sizeof(abs_part), mod_path_abs, save) == 0) {
            struct stat st;
            if (stat(abs_part, &st) == 0 && S_ISDIR(st.st_mode)) {
                if (is_load) { if (walk_load(&ctx, abs_part, save)) { ctx.status = 1; break; } }
                else { if (walk_unload(&ctx, abs_part, save)) { ctx.status = 1; break; } }
            }
        }
        save = p + 1;
    } }
    if (*save) {
        char abs_part[PATH_MAX];
        if (nm_path_join(abs_part, sizeof(abs_part), mod_path_abs, save) == 0) {
            struct stat st;
            if (stat(abs_part, &st) == 0 && S_ISDIR(st.st_mode)) {
                if (is_load) { if (walk_load(&ctx, abs_part, save)) ctx.status = 1; }
                else { if (walk_unload(&ctx, abs_part, save)) ctx.status = 1; }
            }
        }
    }

    if (nm_flush(&ctx) < 0) {
        print_literal("nm: payload flush failed\n");
        return 1;
    }

    long long t1 = nm_now_ms();

    char out[128];
    int n = 0;
    const char *s = "nm_phase_walk=";
    while (*s) out[n++] = *s++;
    long long dt = t1 - t0;
    char tmp[24]; int ti = 0;
    if (dt == 0) tmp[ti++] = '0';
    while (dt > 0) { tmp[ti++] = '0' + (dt % 10); dt /= 10; }
    while (ti > 0) out[n++] = tmp[--ti];
    out[n++] = 'm'; out[n++] = 's'; out[n++] = '\n';
    write(2, out, n);

    char out2[64];
    int n2 = 0;
    const char *s2 = is_load ? "nm_load_status=" : "nm_unload_status=";
    while (*s2) out2[n2++] = *s2++;
    out2[n2++] = ctx.status ? '1' : '0';
    out2[n2++] = '\n';
    write(2, out2, n2);

    return ctx.status;
}

/* --- module listing iteration --- */

static int cmd_module_list(struct nm_workspace *ws, struct nm_args *args) {
    static const char *modules_dir = "/data/adb/modules";
    struct nm_output out = { ws->cwd, 0 };

    list_print_literal(&out, "{\n  \"mods\": [");
    int first = 1;

    DIR *d = opendir(modules_dir);
    if (!d) {
        list_print_literal(&out, "],\n  \"rules\": [],\n  \"links\": []\n}\n");
        list_flush(&out);
        return 1;
    }

    char mod_ids[64][64];
    int mod_count = 0;

    struct dirent *e;
    while ((e = readdir(d))) {
        const char *mod_id = e->d_name;
        if (mod_id[0] == '.') continue;
        if (!strcmp(mod_id, "nomount")) continue;
        if (args->mod_only && !nm_mod_in_list(mod_id, args->mod_only)) continue;
        if (mod_count >= 64) break;

        char mod_path[PATH_MAX];
        if (nm_path_join(mod_path, sizeof(mod_path), modules_dir, mod_id)) continue;

        struct stat st;
        if (stat(mod_path, &st) < 0 || !S_ISDIR(st.st_mode)) continue;

        char prop_path[PATH_MAX];
        if (nm_path_join(prop_path, sizeof(prop_path), mod_path, "module.prop")) continue;
        if (access(prop_path, F_OK) != 0) continue;

        const char *targets_for_check = args->mod_targets ? args->mod_targets : NM_DEFAULT_TARGETS;
        if (!nm_mod_has_injectable(mod_path, targets_for_check)) continue;

        char name_buf[256];
        nm_read_module_name(prop_path, name_buf, sizeof(name_buf));

        int disable = 0, skip = 0;
        if (nm_path_join(prop_path, sizeof(prop_path), mod_path, "disable") == 0 &&
            stat(prop_path, &st) == 0) disable = 1;
        if (nm_path_join(prop_path, sizeof(prop_path), mod_path, "skip_mount") == 0 &&
            stat(prop_path, &st) == 0) skip = 1;

        char real_root[PATH_MAX];
        const char *root = realpath(mod_path, real_root) ? real_root : mod_path;

        if (!first) list_print_literal(&out, ",");
        first = 0;
        list_print_literal(&out, "\n    {\"modId\": \"");
        list_print_json_strn(&out, mod_id, strlen(mod_id));
        list_print_literal(&out, "\", \"realName\": \"");
        list_print_json_strn(&out, name_buf, strlen(name_buf));
        if (disable)
            list_print_literal(&out, "\", \"disable\": true, \"skipMount\": ");
        else
            list_print_literal(&out, "\", \"disable\": false, \"skipMount\": ");
        if (skip)
            list_print_literal(&out, "true, \"root\": \"");
        else
            list_print_literal(&out, "false, \"root\": \"");
        list_print_json_strn(&out, root, strlen(root));
        list_print_literal(&out, "\"}");

        strncpy(mod_ids[mod_count++], mod_id, 63);
        mod_ids[mod_count - 1][63] = '\0';
    }
    closedir(d);
    list_print_literal(&out, "\n  ],\n  \"rules\": [");

    ws->payload.cmd = NM_CMD_GET_LIST;
    ws->payload.arg1 = 0;
    int first_rule = 1;
    do {
        if (nm_send_payload(&ws->payload) < 0) break;
        if (ws->payload.data_size == 0) break;
        char *data = ws->payload.buffer;
        int pos = 0;
        while (pos < (int)ws->payload.data_size) {
            struct nm_rule_hdr *h = (void *)(data + pos);
            unsigned short vlen = h->v_len, rlen = h->r_len;
            pos += sizeof(*h);
            char *v = data + pos; pos += vlen;
            char *r = data + pos; pos += rlen;

            if (!first_rule) list_print_literal(&out, ",");
            first_rule = 0;
            list_print_literal(&out, "\n    {\"virtual\": \"");
            list_print_json_strn(&out, v, vlen);
            list_print_literal(&out, "\", \"real\": \"");
            list_print_json_strn(&out, r, rlen);
            list_print_literal(&out, "\"}");
        }
    } while (ws->payload.arg1 != ~0U);

    list_print_literal(&out, "\n  ],\n  \"links\": [");
    int first_link = 1;

    for (int m = 0; m < mod_count; m++) {
        char mod_path[PATH_MAX];
        if (nm_path_join(mod_path, sizeof(mod_path), modules_dir, mod_ids[m])) continue;

        char targets_buf[PATH_MAX];
        strncpy(targets_buf, NM_DEFAULT_TARGETS, sizeof(targets_buf) - 1);
        targets_buf[sizeof(targets_buf) - 1] = '\0';

        char *save = targets_buf;
        char *p;
        while ((p = strchr(save, ' '))) {
            *p = '\0';
            if (*save) {
                char part_path[PATH_MAX];
                if (nm_path_join(part_path, sizeof(part_path), mod_path, save) == 0) {
                    struct stat st;
                    if (stat(part_path, &st) == 0 && S_ISDIR(st.st_mode))
                        walk_links(&out, part_path, save, mod_ids[m], &first_link, 0);
                }
            }
            save = p + 1;
        }
        if (*save) {
            char part_path[PATH_MAX];
            if (nm_path_join(part_path, sizeof(part_path), mod_path, save) == 0) {
                struct stat st;
                if (stat(part_path, &st) == 0 && S_ISDIR(st.st_mode))
                    walk_links(&out, part_path, save, mod_ids[m], &first_link, 0);
            }
        }
    }

    list_print_literal(&out, "\n  ]\n}\n");
    list_flush(&out);
    return 0;
}

#endif // #ifndef NM_H
