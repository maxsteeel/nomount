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
    __attribute__((naked)) void _start(void) { __asm__ volatile("mov r0, sp\n bl c_main\n"); }
#else
    #error "Arch not supported"
#endif

/* Helper macros to pad unused arguments with zeros */
#define sys0(n)                sys6(n, 0, 0, 0, 0, 0, 0)
#define sys1(n, a)             sys6(n, a, 0, 0, 0, 0, 0)
#define sys2(n, a, b)          sys6(n, a, b, 0, 0, 0, 0)
#define sys3(n, a, b, c)       sys6(n, a, b, c, 0, 0, 0)
#define sys4(n, a, b, c, d)    sys6(n, a, b, c, d, 0, 0)
#define sys5(n, a, b, c, d, e) sys6(n, a, b, c, d, e, 0)

/* Internal macro to dispatch based on argument count */
#define __GET_SYSCALL_MACRO(_1, _2, _3, _4, _5, _6, _7, NAME, ...) NAME

/* * Unified syscall wrapper.
 * It automatically expands to the correct sysX macro depending on how many
 * arguments you provide (up to 6 arguments + the syscall number).
 */
#define syscall(...) __GET_SYSCALL_MACRO(__VA_ARGS__, sys6, sys5, sys4, sys3, sys2, sys1, sys0)(__VA_ARGS__)

/* --- UTILS --- */
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long size_t;
typedef unsigned long long u64;
#define PATH_MAX  4096
#define NULL ((void *)0)


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
#define RX_BUF_SIZE (PATH_MAX + 4)
static char tx_buf[RX_BUF_SIZE]; 
static char rx_buf[RX_BUF_SIZE];
static char v_resolved[PATH_MAX];
static char r_resolved[PATH_MAX];
static char cwd_buf[PATH_MAX];
#define MAX_PAYLOAD (sizeof(tx_buf) - NLMSG_HDRLEN - NLMSG_ALIGN(sizeof(struct genlmsghdr)) - NLA_HDRLEN - 64)

__attribute__((noinline))
static inline void *memcpy(void *dst, const void *src, size_t n) {
    char *d = dst; const char *s = src;
    while (n--) *d++ = *s++;
    return dst;
}

__attribute__((noinline))
static void print_str(const char *s) {
    const char *p = s;
    while (*p) p++;
    syscall(SYS_WRITE, 1, (long)s, p - s);
}

/* complete path resolution */
__attribute__((noinline))
static int resolve_path(char *result, const char *cwd, const char *rel_path) {
    char *p = result;
    if (*rel_path == '/') {
        while (*rel_path) *p++ = *rel_path++;
    } else {
        if (cwd) {
            while (*cwd) *p++ = *cwd++;
            if (p > result && p[-1] != '/') *p++ = '/';
        }
        while (*rel_path) *p++ = *rel_path++;
    }
    *p = '\0';
    return p - result;
}

__attribute__((noinline))
static void *get_attr(struct nlmsghdr *msg, int type) {
    int rem = msg->nlmsg_len - NLMSG_HDRLEN - NLMSG_ALIGN(sizeof(struct genlmsghdr));
    struct nlattr *attr = (struct nlattr *)((char *)msg + NLMSG_HDRLEN + NLMSG_ALIGN(sizeof(struct genlmsghdr)));
    while (rem >= NLA_HDRLEN) {
        if (attr->nla_type == type) return (char *)attr + NLA_HDRLEN;
        int alen = NLA_ALIGN(attr->nla_len);
        if (!alen || alen > rem) break;
        attr = (struct nlattr *)((char *)attr + alen);
        rem -= alen;
    }
    return NULL;
}

/* init_msg + add_attr + send_and_recv unified */
__attribute__((noinline))
static long do_nm_cmd(int fd, int fam, int cmd, int atype, const void *data, int len, int flags) {
    struct nlmsghdr *nlh = (struct nlmsghdr *)tx_buf;
    struct genlmsghdr *gnlh = (struct genlmsghdr *)(tx_buf + NLMSG_HDRLEN);
    long *p = (long *)tx_buf, res;
    p[0] = 0; p[1] = 0; p[2] = 0;

    nlh->nlmsg_type = fam;
    nlh->nlmsg_flags = flags;
    nlh->nlmsg_seq = ++nl_seq;
    gnlh->cmd = cmd;
    gnlh->version = 1;
    nlh->nlmsg_len = NLMSG_HDRLEN + NLMSG_ALIGN(sizeof(*gnlh));

    if (data) {
        struct nlattr *nla = (struct nlattr *)(tx_buf + NLMSG_ALIGN(nlh->nlmsg_len));
        nla->nla_type = atype;
        nla->nla_len = NLA_HDRLEN + len;
        memcpy((char *)nla + NLA_HDRLEN, data, len);
        nlh->nlmsg_len = NLMSG_ALIGN(nlh->nlmsg_len) + NLA_ALIGN(nla->nla_len);
    }

    struct sockaddr_nl dest = { .nl_family = AF_NETLINK };
    if (syscall(SYS_SENDTO, fd, (long)nlh, nlh->nlmsg_len, 0, (long)&dest, sizeof(dest)) < 0 ||
        (res = syscall(SYS_RECVFROM, fd, (long)rx_buf, RX_BUF_SIZE, 0, 0, 0)) < 0) {
        return -1;
    }

    struct nlmsghdr *rep = (struct nlmsghdr *)rx_buf;
    return (rep->nlmsg_type == NLMSG_ERROR) ? ((struct nlmsgerr *)((char *)rep + NLMSG_HDRLEN))->error : res;
}
