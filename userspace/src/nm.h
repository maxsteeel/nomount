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
#include <stdlib.h>
#include <limits.h>
#include <sys/xattr.h>

/* --- DEFS --- */
#define NOMOUNT_MAGIC_SIG 0x4E4F4D4F554E54ULL
#define PATH_MAX  4096
#ifndef NAME_MAX
#define NAME_MAX  255
#endif
enum { NM_FLAG_VIRTUAL_DIR = 2, NM_FLAG_WHITEOUT = 4, NM_FLAG_OPAQUE = 8 };

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

struct nm_modinfo {
    char *mod_id;
    char *name;
    char *root;
    int disable;
    int skip;
    int has_injectable;
    int count;
    int first_map;
    int nmaps;
};

struct nm_mapping {
    char *vpath;
    char *real;
    int dir;
};

struct nm_catalog {
    struct nm_modinfo *mods;
    int nmods, mods_cap;
    struct nm_mapping *maps;
    int nmaps, maps_cap;
};

struct nm_walk {
    struct nm_payload *payload;
    struct nm_catalog *catalog;
    int module_index;
    size_t root_len;
    int has_injectable;
    int error;
    char path[PATH_MAX];
};

static char *nm_strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

static int nm_catalog_add_module(struct nm_catalog *cat, const char *mod_id, const char *name,
                                 const char *root, int disable, int skip) {
    if (cat->nmods >= cat->mods_cap) {
        int cap = cat->mods_cap ? cat->mods_cap * 2 : 16;
        struct nm_modinfo *m = realloc(cat->mods, (size_t)cap * sizeof(*m));
        if (!m) return -ENOMEM;
        cat->mods = m;
        cat->mods_cap = cap;
    }
    struct nm_modinfo *m = &cat->mods[cat->nmods];
    memset(m, 0, sizeof(*m));
    m->mod_id = nm_strdup(mod_id);
    m->name = nm_strdup(name);
    m->root = nm_strdup(root);
    if (!m->mod_id || !m->name || !m->root) return -ENOMEM;
    m->disable = disable;
    m->skip = skip;
    m->first_map = cat->nmaps;
    return cat->nmods++;
}

static int nm_catalog_add_mapping(struct nm_catalog *cat, int module, const char *vpath,
                                  const char *real, int dir) {
    if (cat->nmaps >= cat->maps_cap) {
        int cap = cat->maps_cap ? cat->maps_cap * 2 : 32;
        struct nm_mapping *m = realloc(cat->maps, (size_t)cap * sizeof(*m));
        if (!m) return -ENOMEM;
        cat->maps = m;
        cat->maps_cap = cap;
    }
    struct nm_mapping *m = &cat->maps[cat->nmaps];
    m->vpath = nm_strdup(vpath);
    m->real = nm_strdup(real);
    if (!m->vpath || !m->real) return -ENOMEM;
    m->dir = dir;
    cat->nmaps++;
    cat->mods[module].nmaps++;
    return 0;
}

static void nm_catalog_free(struct nm_catalog *cat) {
    for (int i = 0; i < cat->nmods; i++) {
        free(cat->mods[i].mod_id);
        free(cat->mods[i].name);
        free(cat->mods[i].root);
    }
    for (int i = 0; i < cat->nmaps; i++) {
        free(cat->maps[i].vpath);
        free(cat->maps[i].real);
    }
    free(cat->mods);
    free(cat->maps);
}

static int nm_path_prefix(const char *path, size_t len, const char *prefix, size_t n) {
    return n <= len && (n == len || path[n] == '/') && !memcmp(path, prefix, n);
}

static void nm_catalog_count(struct nm_catalog *cat, const char *virtual, size_t vlen, const char *real, size_t rlen) {
    for (int i = 0; i < cat->nmods; i++) {
        struct nm_modinfo *m = &cat->mods[i];
        if (nm_path_prefix(real, rlen, m->root, strlen(m->root))) {
            m->count++;
            continue;
        }
        for (int k = 0; k < m->nmaps; k++) {
            struct nm_mapping *mp = &cat->maps[m->first_map + k];
            size_t vl = strlen(mp->vpath);
            if ((!mp->dir && vlen != vl) || !nm_path_prefix(virtual, vlen, mp->vpath, vl)) continue;
            size_t suffix = vlen - vl, rl = strlen(mp->real);
            if (suffix && rl && mp->real[rl - 1] == '/') rl--;
            if (rlen != rl + suffix || memcmp(real, mp->real, rl) || memcmp(real + rl, virtual + vl, suffix)) continue;
            m->count++;
            break;
        }
    }
}

struct nm_ancestor {
    const struct nm_ancestor *parent;
    dev_t dev;
    ino_t ino;
};

/* --- UTILS --- */
#define noinline __attribute__((noinline))

static noinline int nm_parse_uid(const char *s, unsigned int *out) {
    unsigned int uid = 0;
    if (!*s) return -EINVAL;
    for (; *s; s++) {
        unsigned int d = (unsigned int)(unsigned char)*s - '0';
        if (d > 9 || uid > (UINT_MAX - d) / 10) return -EINVAL;
        uid = uid * 10 + d;
    }
    *out = uid;
    return 0;
}

static noinline int parse_uid_range(const char *s, unsigned int *start, unsigned int *end) {
    unsigned int first = 0, last = 0;
    const char *dash = strchr(s, '-');
    size_t head = dash ? (size_t)(dash - s) : strlen(s);
    char buf[16];
    if (head >= sizeof(buf)) return -EINVAL;
    memcpy(buf, s, head);
    buf[head] = '\0';
    if (nm_parse_uid(buf, &first)) return -EINVAL;
    if (!dash || !dash[1]) {
        last = first;
    } else {
        if (nm_parse_uid(dash + 1, &last)) return -EINVAL;
    }
    if (last < first) { unsigned int t = first; first = last; last = t; }
    *start = first;
    *end = last;
    return 0;
}

static noinline void parse_arguments(long argc, char **argv, struct nm_args *parsed_args) {
    unsigned char *p = (unsigned char *)parsed_args;
    for (unsigned long i = 0; i < sizeof(*parsed_args); i++) p[i] = 0;
    parsed_args->data_start_idx = 2;

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

static noinline void write_strn(int fd, const char *s, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, s + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        off += n;
    }
}
#define print_strn(s, len) write_strn(STDOUT_FILENO, (s), (len))
#define print_literal(s) print_strn((s), sizeof(s) - 1)

struct nm_output {
    char *buffer;
    unsigned int used;
    int fd;
};

static noinline void list_flush(struct nm_output *out) {
    write_strn(out->fd, out->buffer, out->used);
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

static inline void list_print_str(struct nm_output *out, const char *s) {
    list_print_strn(out, s, strlen(s));
}

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
static noinline int resolve_path(char *base, char *end, const char *cwd, const char *rel, int lexical) {
    if (!rel || !*rel || (*rel != '/' && (!cwd || *cwd != '/'))) return -EINVAL;
    if (base == end) return 0;

    char *out = base;
    for (int pass = (*rel == '/'); pass < 2; pass++) {
        for (const char *s = pass ? rel : cwd; *s; ) {
            while (*s == '/') s++;
            if (!*s) break;
            const char *c = s;
            while (*s && *s != '/') s++;
            size_t len = (size_t)(s - c);
            if (len > NAME_MAX) return -ENAMETOOLONG;
            if (lexical && c[0] == '.') {
                if (len == 1) continue;
                if (len == 2 && c[1] == '.') {
                    while (out > base && *--out != '/') {}
                    continue;
                }
            }
            if ((size_t)(end - out) <= len) return 0;
            *out++ = '/';
            memcpy(out, c, len);
            out += len;
        }
    }
    if (!lexical) {
        size_t rel_len = strlen(rel);
        if (rel_len && rel[rel_len - 1] == '/') {
            if (out == end) return 0;
            *out++ = '/';
        }
    }
    return out == base ? -EINVAL : (int)(out - base);
}

static noinline int nm_invalid_path(const char *path, int error) {
    print_literal("nm: invalid path: ");
    print_strn(path, strlen(path));
    if (error == -ENAMETOOLONG) print_literal(" (name too long)\n");
    else print_literal("\n");
    return error;
}

static noinline int nm_send_payload(struct nm_payload *payload) {
    payload->status = INT_MIN;
    unsigned long ptr = (unsigned long)payload;
    long ret = syscall(SYS_add_key, "nomount", "trigger", &ptr, sizeof(ptr), -1);
    return payload->status == INT_MIN ? (ret < 0 ? -errno : -EIO) : payload->status;
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

static const char *nm_vpath(const struct nm_walk *ctx) {
    const char *path = ctx->path + ctx->root_len;
    if (!strncmp(path, "/system/", 8)) {
        const char *part = path + 8;
        size_t len = 0;
        while (part[len] && part[len] != '/') len++;
        if (nm_is_partition(part, len)) return path + 7;
    }
    return path;
}

static noinline int nm_path_join(char *dst, size_t dst_size, const char *a, const char *b) {
    size_t alen = strlen(a), blen = strlen(b);
    if (alen + 1 + blen + 1 > dst_size) return -ENAMETOOLONG;
    memcpy(dst, a, alen);
    dst[alen] = '/';
    memcpy(dst + alen + 1, b, blen);
    dst[alen + 1 + blen] = '\0';
    return 0;
}

static int nm_error(const char *what, int error) {
    char buffer[PATH_MAX];
    struct nm_output out = { buffer, 0, STDERR_FILENO };
    list_print_literal(&out, "nm: ");
    list_print_strn(&out, what, strlen(what));
    list_print_literal(&out, ": errno ");
    list_print_uint(&out, -error);
    list_print_literal(&out, "\n");
    list_flush(&out);
    return 1;
}

static int nm_flush_rules(struct nm_payload *p) {
    if (!p->data_size) return 0;
    int error = nm_send_payload(p);
    p->arg1 = p->data_size = 0;
    return error;
}

static int nm_queue_rule(struct nm_payload *p, const struct nm_args *args, unsigned flags,
                         const char *cwd, const char *vpath, const char *rpath) {
    int error = 0;
    unsigned header = p->cmd == NM_CMD_ADD_RULE ? sizeof(struct nm_rule_hdr) : sizeof(struct nm_del_hdr);
    for (;;) {
        char *start = p->buffer + p->data_size, *end = p->buffer + sizeof(p->buffer);
        if (end - start < header) goto full;
        char *v = start + header;
        int vlen = resolve_path(v, end, cwd, vpath, 1), rlen = 0;
        if (vlen < 0) return nm_invalid_path(vpath, vlen);
        if (!vlen) goto full;
        if (rpath && (rlen = resolve_path(v + vlen, end, cwd, rpath, 0)) <= 0) {
            if (rlen < 0) return nm_invalid_path(rpath, rlen);
            goto full;
        }
        if (p->cmd == NM_CMD_ADD_RULE) {
            struct nm_rule_hdr *h = (void *)start;
            h->flags = flags;
            h->uid = args->target_uid; h->uid_end = args->target_uid_end;
            h->deny_start = args->deny_start; h->deny_end = args->deny_end;
            h->v_len = (unsigned short)vlen; h->r_len = (unsigned short)rlen;
        } else {
            struct nm_del_hdr *h = (void *)start;
            h->uid = args->target_uid; h->uid_end = args->target_uid_end;
            h->deny_start = args->deny_start; h->deny_end = args->deny_end;
            h->v_len = (unsigned short)vlen;
        }
        p->data_size = (unsigned int)((v + vlen + rlen) - p->buffer);
        return error;
    full:
        if (!p->data_size) return -E2BIG;
        int rc = nm_flush_rules(p);
        if (!error) error = rc;
    }
}

static noinline int nm_collect_mapping(struct nm_walk *ctx, unsigned flags) {
    char target[PATH_MAX];
    target[0] = '\0';
    if (!(flags & NM_FLAG_WHITEOUT) && !realpath(ctx->path, target)) return -errno;
    return nm_catalog_add_mapping(ctx->catalog, ctx->module_index, nm_vpath(ctx), target,
                                   (flags & NM_FLAG_VIRTUAL_DIR) ? 1 : 0);
}

static void nm_emit(struct nm_walk *ctx, unsigned flags) {
    static const struct nm_args global_rule;
    int rc;
    if (ctx->catalog) {
        if (!(flags & NM_FLAG_WHITEOUT)) return;
        rc = nm_collect_mapping(ctx, flags);
    } else {
        const char *real = ctx->payload->cmd == NM_CMD_ADD_RULE && !(flags & NM_FLAG_WHITEOUT) ? ctx->path : NULL;
        rc = nm_queue_rule(ctx->payload, &global_rule, flags, NULL, nm_vpath(ctx), real);
    }
    if (rc && !ctx->error) ctx->error = rc;
}

static DIR *nm_open_dir(int parent, const char *name) {
    int fd = openat(parent, name, O_RDONLY | O_DIRECTORY);
    if (fd < 0) return NULL;
    DIR *d = fdopendir(fd);
    if (!d) { int saved = errno; close(fd); errno = saved; }
    return d;
}

static int nm_dir_has_content(int parent, const char *name) {
    DIR *d = nm_open_dir(parent, name);
    if (!d) return -errno;
    int result = 0;
    for (;;) {
        errno = 0;
        struct dirent *e = readdir(d);
        if (!e) { if (errno) result = -errno; break; }
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (strcmp(e->d_name, ".replace") && strcmp(e->d_name, ".remove")) { result = 1; break; }
        unsigned char type = e->d_type;
        if (type == DT_UNKNOWN) {
            struct stat st;
            if (fstatat(dirfd(d), e->d_name, &st, AT_SYMLINK_NOFOLLOW)) { result = -errno; break; }
            type = st.st_mode >> 12;
        }
        if (type == DT_DIR && (result = nm_dir_has_content(dirfd(d), e->d_name))) break;
    }
    closedir(d);
    return result;
}

/* --- module list helpers --- */

static noinline int nm_mod_in_list(const char *mod_id, const char *list) {
    size_t id_len = strlen(mod_id);
    if (id_len == strlen(list) && !memcmp(mod_id, list, id_len)) return 1;
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

static int nm_read_module_name(const char *prop_path, char *name_out, size_t name_size) {
    name_out[0] = '\0';
    int fd = open(prop_path, O_RDONLY);
    if (fd < 0) return -errno;
    char buf[4096];
    size_t used = 0;
    int error = 0;
    while (used < sizeof(buf) - 1) {
        ssize_t n = read(fd, buf + used, sizeof(buf) - 1 - used);
        if (n < 0) {
            if (errno == EINTR) continue;
            error = -errno;
            break;
        }
        if (n == 0) break;
        used += n;
    }
    close(fd);
    if (error) return error;
    buf[used] = '\0';

    const char *p = buf;
    while (*p) {
        if (!strncmp(p, "name=", 5)) {
            size_t len = 0;
            while (p[5 + len] && p[5 + len] != '\r' && p[5 + len] != '\n') len++;
            if (len >= name_size) len = name_size - 1;
            memcpy(name_out, p + 5, len);
            name_out[len] = '\0';
            return 0;
        }
        p = strchr(p, '\n');
        if (!p) break;
        p++;
    }
    return 0;
}

static int nm_dir_exists(const char *path) {
    struct stat st;
    if (stat(path, &st)) return errno == ENOENT || errno == ENOTDIR ? 0 : -errno;
    return S_ISDIR(st.st_mode);
}

static int nm_next_partition(const char **targets, char *part) {
    const char *p = *targets;
    while (*p == ' ') p++;
    size_t n = 0;
    while (p[n] && p[n] != ' ') n++;
    if (n >= PATH_MAX) return -ENAMETOOLONG;
    memcpy(part, p, n);
    part[n] = '\0';
    *targets = p + n;
    return n != 0;
}

static int nm_check_partition(const char *mod_path, const char *part_name) {
    char path[PATH_MAX];
    int rc = nm_path_join(path, sizeof(path), mod_path, part_name);
    if (rc) return rc;
    if ((rc = nm_dir_exists(path)) <= 0) return rc;
    if ((rc = nm_path_join(path, sizeof(path), "", part_name))) return rc;
    if ((rc = nm_dir_exists(path))) return rc;
    if ((rc = nm_path_join(path, sizeof(path), "/system", part_name))) return rc;
    return nm_dir_exists(path);
}

/* --- module traversal --- */

static int nm_marker(int fd, const char *name) {
    struct stat st;
    if (fstatat(fd, name, &st, AT_SYMLINK_NOFOLLOW)) return errno == ENOENT ? 0 : -errno;
    if (S_ISLNK(st.st_mode) && fstatat(fd, name, &st, 0))
        return errno == ENOENT || errno == ENOTDIR ? 1 : -errno;
    return !S_ISDIR(st.st_mode);
}

static int nm_directory_rule(int fd, int check_contents) {
    int rc = nm_marker(fd, ".remove");
    if (rc < 0) return rc;
    if (rc) return NM_FLAG_WHITEOUT;
    int opaque = nm_marker(fd, ".replace");
    if (opaque < 0) return opaque;
    if (!opaque) {
        char value;
        opaque = fgetxattr(fd, "trusted.overlay.opaque", &value, 1) == 1 && value == 'y';
    }
    if (!opaque) return 0;
    if (!check_contents) return NM_FLAG_OPAQUE;
    int entries = nm_dir_has_content(fd, ".");
    if (entries < 0) return entries;
    return entries ? NM_FLAG_OPAQUE : NM_FLAG_WHITEOUT;
}

static int nm_walk_module(struct nm_walk *ctx, int parent_fd, const char *name,
                           unsigned char type, const struct nm_ancestor *parent) {
    struct stat st;
    int rc = 0;
    if (type == DT_UNKNOWN) {
        if (fstatat(parent_fd, name, &st, AT_SYMLINK_NOFOLLOW)) return -errno;
        type = st.st_mode >> 12;
    }
    if (type == DT_LNK) {
        if (fstatat(parent_fd, name, &st, 0)) return errno == ENOENT || errno == ENOTDIR ? 0 : -errno;
        type = st.st_mode >> 12;
        if (ctx->catalog && (rc = nm_collect_mapping(ctx, type == DT_DIR ? NM_FLAG_VIRTUAL_DIR : 0))) return rc;
    }
    if (type != DT_DIR) {
        if (!strcmp(name, ".replace") || !strcmp(name, ".remove")) return 0;
        if (type == DT_CHR) nm_emit(ctx, NM_FLAG_WHITEOUT);
        else if (type == DT_REG && !ctx->catalog) nm_emit(ctx, 0);
        return 0;
    }
    DIR *d = nm_open_dir(parent_fd, name);
    if (!d) return -errno;
    if (fstat(dirfd(d), &st)) { rc = -errno; goto done; }
    struct nm_ancestor here = { parent, st.st_dev, st.st_ino };
    for (const struct nm_ancestor *a = parent; a; a = a->parent)
        if (a->dev == here.dev && a->ino == here.ino) { rc = -ELOOP; goto done; }
    unsigned cmd = ctx->payload ? ctx->payload->cmd : NM_CMD_UNSPEC;
    int flags = nm_directory_rule(dirfd(d), cmd != NM_CMD_DEL_RULE);
    if (flags < 0) { rc = flags; goto done; }
    if (flags) nm_emit(ctx, flags);
    if (flags == NM_FLAG_WHITEOUT && cmd != NM_CMD_DEL_RULE) goto done;
    size_t len = strlen(ctx->path);
    for (;;) {
        errno = 0;
        struct dirent *e = readdir(d);
        if (!e) { if (errno) rc = -errno; break; }
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (e->d_type == DT_REG && ctx->catalog) continue;
        rc = nm_path_join(ctx->path + len, sizeof(ctx->path) - len, "", e->d_name);
        if (rc) break;
        rc = nm_walk_module(ctx, dirfd(d), e->d_name, e->d_type, &here);
        ctx->path[len] = '\0';
        if (rc) break;
    }
done:
    closedir(d);
    return rc;
}

static int nm_walk_partitions(struct nm_walk *ctx, const char *targets) {
    char part[PATH_MAX];
    int rc;
    while ((rc = nm_next_partition(&targets, part)) > 0) {
        if ((rc = nm_check_partition(ctx->path, part)) < 0) return rc;
        if (!rc) continue;
        ctx->has_injectable = 1;
        if (!ctx->payload && !ctx->catalog) continue;
        rc = nm_path_join(ctx->path + ctx->root_len, sizeof(ctx->path) - ctx->root_len, "", part);
        if (rc) return rc;
        rc = nm_walk_module(ctx, AT_FDCWD, ctx->path, DT_UNKNOWN, NULL);
        ctx->path[ctx->root_len] = '\0';
        if (rc) return rc;
    }
    return rc;
}

/* --- module load / unload --- */

static int cmd_module_load_or_unload(struct nm_workspace *ws, struct nm_args *args, int is_load) {
    struct nm_walk ctx = { .payload = &ws->payload };
    int rc = 0;
    if (!args->mod_path) rc = -EINVAL;
    else if (!realpath(args->mod_path, ctx.path)) rc = -errno;
    else if ((rc = nm_dir_exists(ctx.path)) > 0) {
        ctx.root_len = strlen(ctx.path);
        ctx.payload->cmd = is_load ? NM_CMD_ADD_RULE : NM_CMD_DEL_RULE;
        ctx.payload->arg1 = ctx.payload->data_size = 0;
        rc = nm_walk_partitions(&ctx, args->mod_targets ? args->mod_targets : NM_DEFAULT_TARGETS);
        if (!rc) rc = nm_flush_rules(ctx.payload);
        if (!rc) rc = ctx.error;
    } else if (!rc) rc = -ENOTDIR;
    if (rc) nm_error(is_load ? "module load" : "module unload", rc);
    const char *status = is_load ? "nm_load_status=" : "nm_unload_status=";
    write_strn(STDERR_FILENO, status, strlen(status));
    write_strn(STDERR_FILENO, rc ? "1\n" : "0\n", 2);
    return rc != 0;
}

/* --- module listing iteration --- */

static int cmd_module_list(struct nm_workspace *ws, struct nm_args *args) {
    static const char *modules_dir = "/data/adb/modules";
    DIR *d = opendir(modules_dir);
    if (!d) return nm_error("module list", -errno);
    struct nm_output out = { ws->cwd, 0, STDOUT_FILENO };
    struct nm_catalog cat = { 0 };
    const char *targets = args->mod_targets ? args->mod_targets : NM_DEFAULT_TARGETS;

    ws->payload.cmd = NM_CMD_GET_LIST;
    ws->payload.arg1 = 0;
    int rc = nm_send_payload(&ws->payload);
    while (!rc) {
        errno = 0;
        struct dirent *e = readdir(d);
        if (!e) { if (errno) rc = -errno; break; }
        const char *mod_id = e->d_name;
        if (mod_id[0] == '.' || !strcmp(mod_id, "nomount")) continue;
        if (args->mod_only && !nm_mod_in_list(mod_id, args->mod_only)) continue;
        char mod_path[PATH_MAX], prop_path[PATH_MAX], name[256];
        struct nm_walk ctx = { .catalog = ws->payload.data_size ? &cat : NULL };
        if ((rc = nm_path_join(mod_path, sizeof(mod_path), modules_dir, mod_id))) break;
        if ((rc = nm_dir_exists(mod_path)) < 0) break;
        if (!rc) continue;
        if ((rc = nm_path_join(prop_path, sizeof(prop_path), mod_path, "module.prop"))) break;
        rc = nm_read_module_name(prop_path, name, sizeof(name));
        if (rc == -ENOENT) { rc = 0; continue; }
        if (rc) break;
        if (!realpath(mod_path, ctx.path)) { rc = -errno; break; }
        ctx.root_len = strlen(ctx.path);
        if ((rc = nm_path_join(prop_path, sizeof(prop_path), mod_path, "disable"))) break;
        int disable = access(prop_path, F_OK) == 0;
        if (!disable && errno != ENOENT && errno != ENOTDIR) { rc = -errno; break; }
        if ((rc = nm_path_join(prop_path, sizeof(prop_path), mod_path, "skip_mount"))) break;
        int skip = access(prop_path, F_OK) == 0;
        if (!skip && errno != ENOENT && errno != ENOTDIR) { rc = -errno; break; }
        int index = nm_catalog_add_module(&cat, mod_id, name, ctx.path, disable, skip);
        if (index < 0) { rc = -ENOMEM; break; }
        ctx.module_index = index;
        rc = nm_walk_partitions(&ctx, targets);
        if (!rc) rc = ctx.error;
        cat.mods[index].has_injectable = ctx.has_injectable;
        if (rc) break;
    }
    closedir(d);

    while (!rc && ws->payload.data_size) {
        char *data = ws->payload.buffer;
        int pos = 0;
        while (pos < (int)ws->payload.data_size) {
            struct nm_rule_hdr *h = (void *)(data + pos);
            unsigned short vlen = h->v_len, rlen = h->r_len;
            pos += sizeof(*h);
            char *v = data + pos; pos += vlen;
            char *r = data + pos; pos += rlen;
            if (vlen >= PATH_MAX || rlen >= PATH_MAX) continue;
            nm_catalog_count(&cat, v, vlen, r, rlen);
        }
        if (ws->payload.arg1 == ~0U) break;
        rc = nm_send_payload(&ws->payload);
    }
    if (rc) goto skip_list;

    list_print_literal(&out, "{\n  \"mods\": [");
    for (int i = 0; i < cat.nmods; i++) {
        struct nm_modinfo *m = &cat.mods[i];
        if (i) list_print_literal(&out, ",");
        list_print_literal(&out, "\n    {\"modId\": \"");
        list_print_json_strn(&out, m->mod_id, strlen(m->mod_id));
        list_print_literal(&out, "\", \"realName\": \"");
        list_print_json_strn(&out, m->name, strlen(m->name));
        list_print_str(&out, m->disable ? "\", \"disable\": true" : "\", \"disable\": false");
        list_print_str(&out, m->skip ? ", \"skipMount\": true" : ", \"skipMount\": false");
        list_print_str(&out, m->has_injectable ? ", \"hasInjectable\": true, " : ", \"hasInjectable\": false, ");
        list_print_literal(&out, "\"fileCount\": ");
        list_print_uint(&out, m->count);
        list_print_literal(&out, "}");
    }
    list_print_literal(&out, "\n  ]\n}\n");
    list_flush(&out);
skip_list:
    nm_catalog_free(&cat);
    return rc ? nm_error("module list", rc) : 0;
}

#endif // #ifndef NM_H
