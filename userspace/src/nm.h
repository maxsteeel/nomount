/* --- ARCH --- */
#if defined(__aarch64__)
    #define SYS_GETCWD     17
    #define SYS_READ       63
    #define SYS_WRITE      64
    #define SYS_EXIT       93
    #define SYS_SOCKET     198

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
    __asm__( ".global _start\n" ".type _start, %function\n" "_start:\n" "mov x0, sp\n" "b c_main\n" );

#elif defined(__arm__)
    #define SYS_EXIT       1
    #define SYS_READ       3
    #define SYS_WRITE      4
    #define SYS_GETCWD     183
    #define SYS_SOCKET     281

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
    __asm__( ".global _start\n" ".type _start, %function\n" "_start:\n" "mov r0, sp\n" "b c_main\n");

#elif defined(__x86_64__)
    #define SYS_READ       0
    #define SYS_WRITE      1
    #define SYS_SOCKET     41
    #define SYS_EXIT       60
    #define SYS_GETCWD     79

    __attribute__((always_inline)) static inline long sys1(long n, long a) {
        long ret; __asm__ __volatile__("syscall" : "=a"(ret) : "a"(n), "D"(a) : "rcx", "r11", "memory", "cc");
        return ret;
    }
    __attribute__((always_inline)) static inline long sys3(long n, long a, long b, long c) {
        long ret; __asm__ __volatile__("syscall" : "=a"(ret) : "a"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory", "cc");
        return ret;
    }
    __asm__( ".global _start\n" ".type _start, @function\n" "_start:\n" "mov %rsp, %rdi\n" "jmp c_main\n" );

#else
    #error "Arch not supported"
#endif

/* --- NETLINK DEFS --- */
#define AF_NETLINK 16
#define SOCK_RAW 3
#define NETLINK_GENERIC 16

struct nlmsghdr {
    unsigned int   nlmsg_len;
    unsigned short nlmsg_type;
    unsigned short nlmsg_flags;
    unsigned int   nlmsg_seq;
    unsigned int   nlmsg_pid;
};

#define PATH_MAX  4096
#define RX_BUF_SIZE 32768
#define TX_BUF_SIZE 16384
#define MAX_PAYLOAD (TX_BUF_SIZE - 88)

struct nm_mem {
    char rx_buf[RX_BUF_SIZE];
    char tx_buf[TX_BUF_SIZE];
    char v_resolved[PATH_MAX];
    char r_resolved[PATH_MAX];
    char cwd_buf[PATH_MAX];
    char payload[MAX_PAYLOAD];
} __attribute__((aligned(16)));

#define noinline __attribute__((noinline))
#if defined(__x86_64__)
static noinline void *memcpy(void *dst, const void *src, unsigned long n) {
    void *ret = dst;
    __asm__ __volatile__("rep movsb" : "+D"(dst), "+S"(src), "+c"(n) : : "memory");
    return ret;
}
#else
static noinline void *memcpy(void *dst, const void *src, unsigned long n) {
    char *d = dst;
    const char *s = src;
    while (n--) { *d++ = *s++; }
    return dst;
}
#endif

static noinline void print_str(const char *s) {
    long len = 0;
    while (s[len]) len++;
    sys3(SYS_WRITE, 1, (long)s, len);
}

/* path resolution */
static noinline char* resolve_path(char *p, const char *cwd, const char *rel) {
    if (cwd && *rel != '/') {
        while (*cwd) *p++ = *cwd++;
        *p++ = '/'; /* Linux VFS treats "//" exactly as "/" */
    }
    while ((*p++ = *rel++));
    return p - 1; /* Points exactly to '\0' */
}

static noinline void *get_attr(const void *nh, int type) {
    unsigned int max_len = ((struct nlmsghdr *)nh)->nlmsg_len;
    char *attr = (char *)nh + 20;
    while ((attr - (char *)nh) + 4 <= max_len) {
        unsigned short alen = *(unsigned short *)attr;
        if (alen < 4) break;
        if (*(unsigned short *)(attr + 2) == type) return attr + 4;
        attr += (alen + 3) & -4;
    }
    return (void *)0;
}

/* init_msg + add_attr + send_and_recv unified */
static noinline int do_nm_cmd(int fd, int fam, int cmd, int atype, const void *data, int len, int flags, struct nm_mem *mem) {
    struct nlmsghdr *nlh = (void *)mem->tx_buf;
    nlh->nlmsg_type = fam;
    nlh->nlmsg_flags = flags;
    nlh->nlmsg_len = 20;
    mem->tx_buf[16] = cmd;
    mem->tx_buf[17] = 1;

    if (data) {
        unsigned short *nla = (void *)(mem->tx_buf + 20);
        nla[0] = 4 + len; nla[1] = atype;
        memcpy(nla + 2, data, len);
        nlh->nlmsg_len = 20 + nla[0];
    }

    int res = sys3(SYS_WRITE, fd, (long)nlh, nlh->nlmsg_len);
    if (res < 0) return res;
    res = sys3(SYS_READ, fd, (long)mem->rx_buf, RX_BUF_SIZE);
    if (res >= 16 && ((struct nlmsghdr *)mem->rx_buf)->nlmsg_type == 2) res = *(int *)(mem->rx_buf + 16);

    return res;
}
