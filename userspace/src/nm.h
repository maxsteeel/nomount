#ifndef NM_H
#define NM_H

#include <unistd.h>
#include <string.h>
#include <stddef.h>
#include <errno.h>
#include <sys/syscall.h>

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

#endif // #ifndef NM_H
