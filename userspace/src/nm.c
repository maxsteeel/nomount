/*
 * nm.c - NoMount CLI Userspace Tool
 *
 */

/* --- ARCH --- */
#if defined(__aarch64__)
    #define SYS_GETCWD     17
    #define SYS_CLOSE      57
    #define SYS_WRITE      64
    #define SYS_EXIT       93
    #define SYS_SOCKET     198
    #define SYS_BIND       200
    #define SYS_SENDTO     206
    #define SYS_RECVFROM   207

    __attribute__((always_inline))
    static inline long sys6(long n, long a, long b, long c, long d, long e, long f) {
        register long x8 asm("x8") = n;
        register long x0 asm("x0") = a;
        register long x1 asm("x1") = b;
        register long x2 asm("x2") = c;
        register long x3 asm("x3") = d;
        register long x4 asm("x4") = e;
        register long x5 asm("x5") = f;
        __asm__ __volatile__("svc 0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5) : "memory", "cc");
        return x0;
    }
    #define sys1(n,a) sys6(n,a,0,0,0,0,0)
    #define sys2(n,a,b) sys6(n,a,b,0,0,0,0)
    #define sys3(n,a,b,c) sys6(n,a,b,c,0,0,0)
    #define sys4(n,a,b,c,d) sys6(n,a,b,c,d,0,0)
    __attribute__((naked)) void _start(void) { __asm__ volatile("mov x0, sp\n bl c_main\n"); }

#elif defined(__arm__)
    #define SYS_EXIT       1
    #define SYS_WRITE      4
    #define SYS_CLOSE      6
    #define SYS_GETCWD     183
    #define SYS_SOCKET     281
    #define SYS_BIND       282
    #define SYS_SENDTO     290
    #define SYS_RECVFROM   292

    __attribute__((always_inline))
    static inline long sys6(long n, long a, long b, long c, long d, long e, long f) {
        register long r7 asm("r7") = n;
        register long r0 asm("r0") = a;
        register long r1 asm("r1") = b;
        register long r2 asm("r2") = c;
        register long r3 asm("r3") = d;
        register long r4 asm("r4") = e;
        register long r5 asm("r5") = f;
        __asm__ __volatile__("svc 0" : "+r"(r0) : "r"(r7), "r"(r1), "r"(r2), "r"(r3), "r"(r4), "r"(r5) : "memory", "cc");
        return r0;
    }
    #define sys1(n,a) sys6(n,a,0,0,0,0,0)
    #define sys2(n,a,b) sys6(n,a,b,0,0,0,0)
    #define sys3(n,a,b,c) sys6(n,a,b,c,0,0,0)
    #define sys4(n,a,b,c,d) sys6(n,a,b,c,d,0,0)
    __attribute__((naked)) void _start(void) { __asm__ volatile("mov r0, sp\n bl c_main\n"); }
#else
    #error "Arch not supported"
#endif

/* --- UTILS --- */
typedef unsigned long size_t;
#define PATH_MAX  4096
#define NULL ((void *)0)

void *memset(void *dst, int c, size_t n) {
    char *d = (char *)dst;
    while (n--) *d++ = (char)c;
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n) {
    char *d = (char *)dst;
    const char *s = (const char *)src;
    while (n--) *d++ = *s++;
    return dst;
}

size_t strlen(const char *s) {
    size_t len = 0;
    while (s[len]) len++;
    return len;
}

#define printc(str) sys3(SYS_WRITE, 1, (long)str, sizeof(str) - 1)

/* --- NETLINK DEFS --- */
#define AF_NETLINK 16
#define SOCK_RAW 3
#define NETLINK_GENERIC 16

/* Netlink Message Flags */
#define NLM_F_REQUEST 1
#define NLM_F_ACK 4
#define NLM_F_ROOT 0x100
#define NLM_F_MATCH 0x200
#define NLM_F_DUMP (NLM_F_ROOT | NLM_F_MATCH)

#define NLMSG_ERROR 2
#define NLMSG_DONE 3

/* Alignment Macros (Crucial for Netlink) */
#define NLMSG_ALIGNTO 4U
#define NLMSG_ALIGN(len) (((len) + NLMSG_ALIGNTO - 1) & ~(NLMSG_ALIGNTO - 1))
#define NLMSG_HDRLEN NLMSG_ALIGN(sizeof(struct nlmsghdr))

#define NLMSG_OK(nlh, len) \
    ((len) >= (int)sizeof(struct nlmsghdr) && \
     (nlh)->nlmsg_len >= sizeof(struct nlmsghdr) && \
     (nlh)->nlmsg_len <= (len))

#define NLMSG_NEXT(nlh, len) \
    ((len) -= NLMSG_ALIGN((nlh)->nlmsg_len), \
     (struct nlmsghdr *)(((char *)(nlh)) + NLMSG_ALIGN((nlh)->nlmsg_len)))

#define NLA_ALIGNTO 4U
#define NLA_ALIGN(len) (((len) + NLA_ALIGNTO - 1) & ~(NLA_ALIGNTO - 1))
#define NLA_HDRLEN NLA_ALIGN(sizeof(struct nlattr))

struct sockaddr_nl {
    unsigned short nl_family;
    unsigned short nl_pad;
    unsigned int   nl_pid;
    unsigned int   nl_groups;
};

struct nlmsghdr {
    unsigned int   nlmsg_len;
    unsigned short nlmsg_type;
    unsigned short nlmsg_flags;
    unsigned int   nlmsg_seq;
    unsigned int   nlmsg_pid;
};

struct nlmsgerr {
    int error;
    struct nlmsghdr msg;
};

struct genlmsghdr {
    unsigned char cmd;
    unsigned char version;
    unsigned short reserved;
};

struct nlattr {
    unsigned short nla_len;
    unsigned short nla_type;
};

/* --- NOMOUNT GENL DEFS --- */
#define GENL_ID_CTRL 16
#define CTRL_CMD_GETFAMILY 3
#define CTRL_ATTR_FAMILY_ID 1
#define CTRL_ATTR_FAMILY_NAME 2

#define NOMOUNT_CMD_GET_VERSION 1
#define NOMOUNT_CMD_ADD_RULE 2
#define NOMOUNT_CMD_DEL_RULE 3
#define NOMOUNT_CMD_CLEAR_ALL 4
#define NOMOUNT_CMD_ADD_UID 5
#define NOMOUNT_CMD_DEL_UID 6
#define NOMOUNT_CMD_GET_LIST 7

#define NOMOUNT_ATTR_VIRTUAL_PATH 1
#define NOMOUNT_ATTR_REAL_PATH 2
#define NOMOUNT_ATTR_FLAGS 3
#define NOMOUNT_ATTR_UID 4
#define NOMOUNT_ATTR_VERSION 5
#define NOMOUNT_ATTR_PAYLOAD 6

/* --- NETLINK ENGINE --- */
static int nl_seq = 0;
static char tx_buf[16384];
static char rx_buf[16384];

/* Initialize a new Netlink message */
static struct nlmsghdr *init_msg(int type, int cmd, int flags) {
    memset(tx_buf, 0, sizeof(tx_buf));
    struct nlmsghdr *nlh = (struct nlmsghdr *)tx_buf;
    nlh->nlmsg_type = type;
    nlh->nlmsg_flags = flags;
    nlh->nlmsg_seq = ++nl_seq;
    nlh->nlmsg_pid = 0; /* Auto-assign */

    struct genlmsghdr *gnlh = (struct genlmsghdr *)(tx_buf + NLMSG_HDRLEN);
    gnlh->cmd = cmd;
    gnlh->version = 1;

    nlh->nlmsg_len = NLMSG_HDRLEN + NLMSG_ALIGN(sizeof(struct genlmsghdr));
    return nlh;
}

/* Append an attribute to the message */
static void add_attr(struct nlmsghdr *nlh, int type, const void *data, int len) {
    int attr_len = NLA_HDRLEN + len;
    struct nlattr *nla = (struct nlattr *)((char *)nlh + NLMSG_ALIGN(nlh->nlmsg_len));
    nla->nla_type = type;
    nla->nla_len = attr_len;
    memcpy((char *)nla + NLA_HDRLEN, data, len);
    nlh->nlmsg_len = NLMSG_ALIGN(nlh->nlmsg_len) + NLA_ALIGN(attr_len);
}

/* Helper to parse attributes from a packet */
static void parse_attrs(struct nlattr **tb, int max, struct nlattr *attr, int len) {
    memset(tb, 0, sizeof(struct nlattr *) * (max + 1));
    while (len >= NLA_HDRLEN) {
        if (attr->nla_len >= NLA_HDRLEN && attr->nla_type <= max) {
            tb[attr->nla_type] = attr;
        }
        int aligned_len = NLA_ALIGN(attr->nla_len);
        if (aligned_len == 0 || aligned_len > len) break;
        attr = (struct nlattr *)((char *)attr + aligned_len);
        len -= aligned_len;
    }
}

/* Send packet and wait for ACK or Data */
static int send_and_recv(int fd, struct nlmsghdr *nlh) {
    struct sockaddr_nl dest = { .nl_family = AF_NETLINK };
    long res = sys6(SYS_SENDTO, fd, (long)nlh, nlh->nlmsg_len, 0, (long)&dest, sizeof(dest));
    if (res < 0) return res;

    res = sys6(SYS_RECVFROM, fd, (long)rx_buf, sizeof(rx_buf), 0, 0, 0);
    if (res < 0) return res;

    struct nlmsghdr *rep = (struct nlmsghdr *)rx_buf;
    if (rep->nlmsg_type == NLMSG_ERROR) {
        struct nlmsgerr *err = (struct nlmsgerr *)((char *)rep + NLMSG_HDRLEN);
        return err->error; /* 0 means ACK success, <0 means error */
    }
    return res; /* Return bytes read */
}

/* Get the dynamic Family ID of NoMount */
static int get_nomount_family_id(int fd) {
    struct nlmsghdr *nlh = init_msg(GENL_ID_CTRL, CTRL_CMD_GETFAMILY, NLM_F_REQUEST);
    const char *name = "nomount";
    add_attr(nlh, CTRL_ATTR_FAMILY_NAME, name, strlen(name) + 1);

    if (send_and_recv(fd, nlh) > 0) {
        struct nlmsghdr *rep = (struct nlmsghdr *)rx_buf;
        if (rep->nlmsg_type != NLMSG_ERROR) {
            struct nlattr *tb[3]; // Max attr is CTRL_ATTR_FAMILY_NAME(2)
            struct nlattr *attrs = (struct nlattr *)((char *)rep + NLMSG_HDRLEN + NLMSG_ALIGN(sizeof(struct genlmsghdr)));
            int attr_len = rep->nlmsg_len - NLMSG_HDRLEN - NLMSG_ALIGN(sizeof(struct genlmsghdr));
            
            parse_attrs(tb, 2, attrs, attr_len);
            if (tb[CTRL_ATTR_FAMILY_ID]) {
                return *(unsigned short *)((char *)tb[CTRL_ATTR_FAMILY_ID] + NLA_HDRLEN);
            }
        }
    }
    return -1;
}

/* --- MAIN --- */
static char v_resolved[PATH_MAX];
static char r_resolved[PATH_MAX];
static char cwd_buf[PATH_MAX];

/* complete path resolution */
__attribute__((noinline))
static int resolve_path(char *result, const char *cwd, const char *rel_path, int max_len) {
    int r_pos = 0;
    int c_len = 0;

    if (rel_path[0] == '/') {
        while (rel_path[r_pos] && r_pos < max_len - 1) {
            result[r_pos] = rel_path[r_pos];
            r_pos++;
        }
    } else {
        if (cwd) {
            while (cwd[c_len] && r_pos < max_len - 1) {
                result[r_pos++] = cwd[c_len++];
            }
            if (r_pos > 0 && result[r_pos-1] != '/' && r_pos < max_len - 1) {
                result[r_pos++] = '/';
            }
        }
        int p_pos = 0;
        while (rel_path[p_pos] && r_pos < max_len - 1) {
            result[r_pos++] = rel_path[p_pos++];
        }
    }
    result[r_pos] = '\0';
    return r_pos;
}

__attribute__((noreturn, used))
void c_main(long *sp) {
    long argc = *sp;
    char **argv = (char **)(sp + 1);
    long exit_code = 1; 
    
    if (argc < 2) {
        printc("nm add|del|cls|blk|unblk|ls\n");
        goto do_exit;
    }

    /* 1. Setup Netlink Socket */
    int fd = sys3(SYS_SOCKET, AF_NETLINK, SOCK_RAW, NETLINK_GENERIC);
    if (fd < 0) { exit_code = 2; goto do_exit; }

    struct sockaddr_nl local = { .nl_family = AF_NETLINK };
    sys3(SYS_BIND, fd, (long)&local, sizeof(local));

    /* 2. Resolve 'nomount' Family ID */
    int nm_family = get_nomount_family_id(fd);
    if (nm_family < 0) {
        printc("Error: NoMount kernel module not loaded.\n");
        exit_code = 3; 
        goto do_exit;
    }

    char cmd = argv[1][0];
    struct nlmsghdr *nlh = NULL;
    int req_flags = NLM_F_REQUEST | NLM_F_ACK;

    switch (cmd) {
        case 'a':
        case 'd': {
            int is_add = (cmd == 'a');
            int step = is_add ? 2 : 1;
            if (argc < 2 + step) goto do_exit;

            long cwd_len = sys2(SYS_GETCWD, (long)cwd_buf, PATH_MAX);
            const char *cwd = (cwd_len > 0) ? cwd_buf : "/";
            exit_code = 0;

            for (int arg_idx = 2; arg_idx + step <= argc; arg_idx += step) {
                int v_len = resolve_path(v_resolved, cwd, argv[arg_idx], PATH_MAX);
                if (v_len == 0) continue;

                nlh = init_msg(nm_family, is_add ? NOMOUNT_CMD_ADD_RULE : NOMOUNT_CMD_DEL_RULE, req_flags);
                add_attr(nlh, NOMOUNT_ATTR_VIRTUAL_PATH, v_resolved, v_len + 1);

                if (is_add) {
                    int r_len = resolve_path(r_resolved, cwd, argv[arg_idx+1], PATH_MAX);
                    if (r_len == 0) continue;
                    add_attr(nlh, NOMOUNT_ATTR_REAL_PATH, r_resolved, r_len + 1);
                    unsigned int flags = 0;
                    add_attr(nlh, NOMOUNT_ATTR_FLAGS, &flags, sizeof(flags));
                }

                long res = send_and_recv(fd, nlh);
                if (res < 0) exit_code = -res;
            }
            goto do_exit;
        }
        case 'b':
        case 'u': {
            if (argc < 3) goto do_exit;
            unsigned int uid = 0;
            const char *s = argv[2];
            while (*s) uid = uid * 10 + (*s++ - '0');

            nlh = init_msg(nm_family, (cmd == 'b') ? NOMOUNT_CMD_ADD_UID : NOMOUNT_CMD_DEL_UID, req_flags);
            add_attr(nlh, NOMOUNT_ATTR_UID, &uid, sizeof(uid));
            break;
        }
        case 'c':
            nlh = init_msg(nm_family, NOMOUNT_CMD_CLEAR_ALL, req_flags);
            break;
        case 'v':
            nlh = init_msg(nm_family, NOMOUNT_CMD_GET_VERSION, req_flags);
            break;
        case 'l': {
            /* The Dumpit Magic for 'ls' */
            nlh = init_msg(nm_family, NOMOUNT_CMD_GET_LIST, NLM_F_REQUEST | NLM_F_DUMP);
            
            struct sockaddr_nl dest = { .nl_family = AF_NETLINK };
            sys6(SYS_SENDTO, fd, (long)nlh, nlh->nlmsg_len, 0, (long)&dest, sizeof(dest));

            int is_json = (argc > 2 && argv[2][0] == 'j');
            if (is_json) printc("[\n");
            int first_item = 1;

            while (1) {
                long len = sys6(SYS_RECVFROM, fd, (long)rx_buf, sizeof(rx_buf), 0, 0, 0);
                if (len <= 0) break;

                int stop = 0;

               for (struct nlmsghdr *msg = (struct nlmsghdr *)rx_buf; NLMSG_OK(msg, len); msg = NLMSG_NEXT(msg, len)) {
                    if (msg->nlmsg_type == NLMSG_DONE || msg->nlmsg_type == NLMSG_ERROR) {
                        stop = 1;
                        break;
                    }

                    struct nlattr *tb[NOMOUNT_ATTR_PAYLOAD + 1];
                    struct nlattr *attrs = (struct nlattr *)((char *)msg + NLMSG_HDRLEN + NLMSG_ALIGN(sizeof(struct genlmsghdr)));
                    int attr_len = msg->nlmsg_len - NLMSG_HDRLEN - NLMSG_ALIGN(sizeof(struct genlmsghdr));
                    
                    parse_attrs(tb, NOMOUNT_ATTR_PAYLOAD, attrs, attr_len);
                    
                    if (tb[NOMOUNT_ATTR_VIRTUAL_PATH] && tb[NOMOUNT_ATTR_REAL_PATH]) {
                        char *v = (char *)tb[NOMOUNT_ATTR_VIRTUAL_PATH] + NLA_HDRLEN;
                        char *r = (char *)tb[NOMOUNT_ATTR_REAL_PATH] + NLA_HDRLEN;
                        
                        if (is_json) {
                            if (!first_item) printc(",\n");
                            first_item = 0;
                            printc("  {\n    \"virtual\": \"");
                            sys3(SYS_WRITE, 1, (long)v, strlen(v));
                            printc("\",\n    \"real\": \"");
                            sys3(SYS_WRITE, 1, (long)r, strlen(r));
                            printc("\"\n  }");
                        } else {
                            sys3(SYS_WRITE, 1, (long)v, strlen(v));
                            printc(" -> ");
                            sys3(SYS_WRITE, 1, (long)r, strlen(r));
                            printc("\n");
                        }
                    }
                    msg = (struct nlmsghdr *)((char *)msg + NLMSG_ALIGN(msg->nlmsg_len));
                }
                if (stop) break;
            }
            if (is_json) printc("\n]\n");
            
            exit_code = 0;
            goto do_exit;
        }
    }

    if (nlh) {
        long res = send_and_recv(fd, nlh);
        exit_code = (res < 0) ? -res : 0;

        if (cmd == 'v' && res > 0) {
            /* Handle Version Reply */
            struct nlmsghdr *rep = (struct nlmsghdr *)rx_buf;
            struct nlattr *tb[6];
            struct nlattr *attrs = (struct nlattr *)((char *)rep + NLMSG_HDRLEN + NLMSG_ALIGN(sizeof(struct genlmsghdr)));
            int attr_len = rep->nlmsg_len - NLMSG_HDRLEN - NLMSG_ALIGN(sizeof(struct genlmsghdr));
            
            parse_attrs(tb, 5, attrs, attr_len);
            if (tb[NOMOUNT_ATTR_VERSION]) {
                unsigned int ver = *(unsigned int *)((char *)tb[NOMOUNT_ATTR_VERSION] + NLA_HDRLEN);
                char v_str[2] = { ver + '0', '\n' };
                sys3(SYS_WRITE, 1, (long)v_str, 2);
            }
        }
    }

do_exit:
    if (fd >= 0) sys1(SYS_CLOSE, fd);
    sys1(SYS_EXIT, exit_code);
    __builtin_unreachable();
}
