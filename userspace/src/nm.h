/* --- ARCH --- */
#if defined(__aarch64__)
    #define SYS_GETCWD     17
    #define SYS_READ       63
    #define SYS_CLOSE      57
    #define SYS_LSEEK      62
    #define SYS_WRITE      64
    #define SYS_EXIT       93
    #define SYS_ADD_KEY    217
    #define SYS_OPENAT     56
    #define SYS_FLOCK      32
    #define SYS_NANOSLEEP  101
    #define SYS_RENAMEAT   38
    #define SYS_SYNC       81

    __attribute__((always_inline)) static inline long sys1(long n, long a) {
        register long x8 asm("x8") = n; register long x0 asm("x0") = a;
        __asm__ __volatile__("svc 0" : "+r"(x0) : "r"(x8) : "memory", "cc");
        return x0;
    }
    __attribute__((always_inline)) static inline long sys3(long n, long a, long b, long c) {
        register long x8 asm("x8") = n; register long x0 asm("x0") = a; register long x1 asm("x1") = b; register long x2 asm("x2") = c;
        __asm__ __volatile__("svc 0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory", "cc");
        return x0;
    }
    __attribute__((always_inline)) static inline long sys5(long n, long a, long b, long c, long d, long e) {
        register long x8 asm("x8") = n; register long x0 asm("x0") = a; register long x1 asm("x1") = b;
        register long x2 asm("x2") = c; register long x3 asm("x3") = d; register long x4 asm("x4") = e;
        __asm__ __volatile__("svc 0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4) : "memory", "cc");
        return x0;
    }
    __asm__( ".global _start\n" ".type _start, %function\n" "_start:\n" "mov x0, sp\n" "b c_main\n" );

#elif defined(__arm__)
    #define SYS_EXIT       1
    #define SYS_READ       3
    #define SYS_WRITE      4
    #define SYS_GETCWD     183
    #define SYS_CLOSE      6
    #define SYS_LSEEK      19
    #define SYS_ADD_KEY    309
    #define SYS_OPENAT     322
    #define SYS_FLOCK      143
    #define SYS_NANOSLEEP  162
    #define SYS_RENAMEAT   329
    #define SYS_SYNC       36

    __attribute__((always_inline)) static inline long sys1(long n, long a) {
        register long r7 asm("r7") = n; register long r0 asm("r0") = a;
        __asm__ __volatile__("svc 0" : "+r"(r0) : "r"(r7) : "memory", "cc");
        return r0;
    }
    __attribute__((always_inline)) static inline long sys3(long n, long a, long b, long c) {
        register long r7 asm("r7") = n; register long r0 asm("r0") = a; register long r1 asm("r1") = b; register long r2 asm("r2") = c;
        __asm__ __volatile__("svc 0" : "+r"(r0) : "r"(r7), "r"(r1), "r"(r2) : "memory", "cc");
        return r0;
    }
    __attribute__((always_inline)) static inline long sys5(long n, long a, long b, long c, long d, long e) {
        register long r7 asm("r7") = n; register long r0 asm("r0") = a; register long r1 asm("r1") = b;
        register long r2 asm("r2") = c; register long r3 asm("r3") = d; register long r4 asm("r4") = e;
        __asm__ __volatile__("svc 0" : "+r"(r0) : "r"(r7), "r"(r1), "r"(r2), "r"(r3), "r"(r4) : "memory", "cc");
        return r0;
    }
    __asm__( ".global _start\n" ".type _start, %function\n" "_start:\n" "mov r0, sp\n" "b c_main\n");

#elif defined(__x86_64__)
    #define SYS_READ       0
    #define SYS_WRITE      1
    #define SYS_EXIT       60
    #define SYS_GETCWD     79
    #define SYS_CLOSE      3
    #define SYS_LSEEK      8
    #define SYS_ADD_KEY    248
    #define SYS_OPENAT     257
    #define SYS_FLOCK      72
    #define SYS_NANOSLEEP  35
    #define SYS_RENAMEAT   264
    #define SYS_SYNC       162

    __attribute__((always_inline)) static inline long sys1(long n, long a) {
        long ret; __asm__ __volatile__("syscall" : "=a"(ret) : "a"(n), "D"(a) : "rcx", "r11", "memory", "cc");
        return ret;
    }
    __attribute__((always_inline)) static inline long sys3(long n, long a, long b, long c) {
        long ret; __asm__ __volatile__("syscall" : "=a"(ret) : "a"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory", "cc");
        return ret;
    }
    __attribute__((always_inline)) static inline long sys5(long n, long a, long b, long c, long d, long e) {
        long ret; register long r10 asm("r10") = d; register long r8 asm("r8") = e;
        __asm__ __volatile__("syscall" : "=a"(ret) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8) : "rcx", "r11", "memory", "cc");
        return ret;
    }
    __asm__( ".global _start\n" ".type _start, @function\n" "_start:\n" "mov %rsp, %rdi\n" "jmp c_main\n" );

#else
    #error "Arch not supported"
#endif

/* --- DEFS --- */
#define NOMOUNT_MAGIC_SIG 0x4E4F4D4F554E54ULL
#define PATH_MAX  4096
#define NM_EINTR  4
#define NM_EAGAIN 11
#define NM_EINVAL 22
#define NM_EPIPE  32
#define NM_ENOBUFS 132
#define AT_FDCWD  -100
#define O_RDONLY  0
#define O_WRONLY  1
#define O_RDWR    2
#define O_CREAT   64
#define O_APPEND  1024
#define SEEK_END  2
#define LOCK_EX   2
#define LOCK_NB   4

#define NM_LOG_DIR      "/data/adb/nomount"
#define NM_LOG_PATH     NM_LOG_DIR "/nomount.log"
#define NM_WATCH_LOG    NM_LOG_DIR "/reboot-watch.log"
#define NM_WATCH_LOCK   NM_LOG_DIR "/.reboot-watch.lock"

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
    ACTION_REBOOT_WATCH,
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
static noinline int strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) { s1++; s2++; }
    return *(unsigned char *)s1 - *(unsigned char *)s2;
}

static noinline void parse_arguments(long argc, char **argv, struct nm_args *parsed_args) {
    parsed_args->action = ACTION_NONE;
    parsed_args->data_start_idx = 2;
    parsed_args->is_json = 0;
    parsed_args->is_whiteout = 0;
    parsed_args->is_opaque = 0;
    parsed_args->target_uid = 0;
    parsed_args->target_uid_end = 0;
    parsed_args->deny_start = 0;
    parsed_args->deny_end = 0;

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
        else if (argc >= 3 && strcmp(argv[2], "all") == 0) parsed_args->action = ACTION_CLEAR_ALL;
        else parsed_args->action = ACTION_CLEAR_ALL;
        parsed_args->data_start_idx = (argc >= 3) ? 3 : 2;
    } else if (strcmp(c1, "reboot-watch") == 0) { parsed_args->action = ACTION_REBOOT_WATCH; }

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

static noinline void print_strn(const char *s, unsigned long len) {
    if (len > 0) sys3(SYS_WRITE, 1, (long)s, len);
}

#define print_literal(s) print_strn((s), sizeof(s) - 1)

struct nm_output {
    char *buffer;
    unsigned int used;
};

static noinline void list_flush(struct nm_output *out) {
    unsigned int sent = 0;
    while (sent < out->used) {
        long n = sys3(SYS_WRITE, 1, (long)(out->buffer + sent), out->used - sent);
        if (n == -NM_EINTR) continue;
        if (n <= 0) break;
        sent += n;
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
static noinline int resolved_path_length(const char *cwd, const char *rel) {
    int length = 0;
    if (cwd && *rel != '/') {
        while (*cwd++) {
            if (length == PATH_MAX) return -1;
            length++;
        }
        if (length == PATH_MAX) return -1;
        length++;
    }
    while (*rel++) {
        if (length == PATH_MAX) return -1;
        length++;
    }
    return length;
}

static noinline char* resolve_path(char *p, const char *cwd, const char *rel) {
    if (cwd && *rel != '/') {
        while (*cwd) {
            *p++ = *cwd++;
        }
        *p++ = '/';
    }
    while (*rel) {
        *p++ = *rel++;
    }
    return p;
}

static noinline int nm_send_payload(struct nm_payload *payload) {
    payload->status = -1;
    unsigned long ptr = (unsigned long)payload;
    sys5(SYS_ADD_KEY, (long)"nomount", (long)"trigger", (long)&ptr, sizeof(ptr), -1);
    return payload->status;
}

static noinline int buf_contains(const char *buf, long len, const char *needle) {
    long nlen = 0;
    while (needle[nlen]) nlen++;
    if (len < nlen) return 0;
    for (long i = 0; i <= len - nlen; i++) {
        long j = 0;
        while (j < nlen && buf[i + j] == needle[j]) j++;
        if (j == nlen) return 1;
    }
    return 0;
}

static noinline void nm_watch_log(const char *message, unsigned long len) {

    long fd = sys5(SYS_OPENAT, AT_FDCWD, (long)NM_WATCH_LOG,
                   O_WRONLY | O_CREAT | O_APPEND, 0600, 0);
    if (fd < 0) {
        fd = sys5(SYS_OPENAT, AT_FDCWD, (long)"/dev/kmsg", O_WRONLY, 0, 0);
        if (fd < 0) {
            print_strn(message, len);
            return;
        }
    }

    unsigned long sent = 0;
    while (sent < len) {
        long n = sys3(SYS_WRITE, fd, (long)(message + sent), len - sent);
        if (n == -NM_EINTR) continue;
        if (n <= 0) break;
        sent += n;
    }
    sys1(SYS_CLOSE, fd);
}

#define nm_watch_log_literal(s) nm_watch_log((s), sizeof(s) - 1)

static noinline int nm_watch_lock(void) {
    int fd = (int)sys5(SYS_OPENAT, AT_FDCWD, (long)NM_WATCH_LOCK,
                       O_RDWR | O_CREAT, 0600, 0);
    if (fd < 0) return -1;
    long f = sys3(SYS_FLOCK, fd, LOCK_EX | LOCK_NB, 0);
    if (f) {
        sys1(SYS_CLOSE, fd);
        return (f == -NM_EAGAIN) ? -2 : -1;
    }

    return fd;
}

static noinline void nm_rotate_log(const char *log_path) {
    char prev_path[64];
    unsigned int len = 0;
    while (1) {
        char c = log_path[len];
        if (c == '\0') break;
        if (len + sizeof(".old") > sizeof(prev_path)) return;
        prev_path[len] = c;
        len++;
    }
    prev_path[len] = '.'; prev_path[len + 1] = 'o';
    prev_path[len + 2] = 'l'; prev_path[len + 3] = 'd';
    prev_path[len + 4] = '\0';
    sys5(SYS_RENAMEAT, AT_FDCWD, (long)log_path, AT_FDCWD, (long)prev_path, 0);
}

static noinline int nm_send_clear_rules(struct nm_payload *payload) {
    payload->cmd = NM_CMD_CLEAR_RULES;
    payload->arg1 = 0;
    payload->data_size = 0;
    return nm_send_payload(payload);
}

static noinline int nm_reboot_watch(struct nm_payload *payload) {
    int lk = nm_watch_lock();
    if (lk == -2) return 0;
    if (lk < 0) {
        nm_watch_log_literal("NoMount reboot-watch: cannot acquire watcher lock\n");
        return 1;
    }


    char line[10240];
    int kmsg = (int)sys3(SYS_OPENAT, AT_FDCWD, (long)"/dev/kmsg", O_RDONLY);
    if (kmsg < 0) {
        nm_watch_log_literal("NoMount reboot-watch: cannot open /dev/kmsg\n");
        return 1;
    }


    if (sys3(SYS_LSEEK, kmsg, 0, SEEK_END) < 0) {
        nm_watch_log_literal("NoMount reboot-watch: cannot seek /dev/kmsg to its live end\n");
        sys1(SYS_CLOSE, kmsg);
        return 1;
    }
    nm_watch_log_literal("NoMount reboot-watch: armed; waiting for init reboot marker\n");

    int overrun_logged = 0, read_errors = 0;
    while (1) {
        long n = sys3(SYS_READ, kmsg, (long)line, sizeof(line));
        if (n == -NM_EINTR) continue;
        if (n == -NM_EPIPE || n == -NM_ENOBUFS || n == -NM_EINVAL) {

            if (!overrun_logged) {
                nm_watch_log_literal("NoMount reboot-watch: kmsg ring churn; resuming at oldest retained record\n");
                overrun_logged = 1;
            }
            read_errors = 0;
            continue;
        }
        if (n <= 0) {

            if (++read_errors < 64) continue;
            nm_watch_log_literal("NoMount reboot-watch: /dev/kmsg read failed before reboot marker\n");
            sys1(SYS_CLOSE, kmsg);
            return 1;
        }
        read_errors = 0;
        if (buf_contains(line, n, "Reboot start, reason:")) break;
    }
    sys1(SYS_CLOSE, kmsg);

    int cleared = 0;
    for (int attempt = 0; attempt < 3 && !cleared; attempt++) {
        if (attempt > 0) {

            struct { long tv_sec; long tv_nsec; } ts = {0, 25 * 1000 * 1000};
            sys3(SYS_NANOSLEEP, (long)&ts, 0, 0);
        }
        cleared = (nm_send_clear_rules(payload) == 0);
    }


    if (cleared) nm_watch_log_literal("NoMount reboot-watch: init marker seen; runtime rules cleared\n");
    else nm_watch_log_literal("NoMount reboot-watch: init marker seen, but clearing rules failed\n");

    nm_rotate_log(NM_LOG_PATH);
    nm_rotate_log(NM_WATCH_LOG);

    if (!cleared) {

        sys1(SYS_SYNC, 0);
        return 1;
    }
    return 0;
}
