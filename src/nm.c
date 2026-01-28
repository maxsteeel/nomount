/*
 * nm.c - NoMount CLI Userspace Tool 
 */

/* --- ARCH --- */
#if defined(__aarch64__)
    #define SYS_GETCWD     17
    #define SYS_GETDENTS64 61
    #define SYS_IOCTL      29
    #define SYS_OPENAT     56
    #define SYS_CLOSE      57
    #define SYS_WRITE      64
    #define SYS_FSTATAT    79 
    #define SYS_EXIT       93
    #define STAT_MODE_IDX  4   

    struct ioctl_data {
        unsigned long vp;
        unsigned long rp;
        unsigned int flags;
        unsigned int _pad;
    };

    __attribute__((always_inline))
    static inline long sys1(long n, long a) {
        register long x8 asm("x8") = n;
        register long x0 asm("x0") = a;
        __asm__ __volatile__("svc 0" : "+r"(x0) : "r"(x8) : "memory", "cc");
        return x0;
    }
    __attribute__((always_inline))
    static inline long sys2(long n, long a, long b) {
        register long x8 asm("x8") = n;
        register long x0 asm("x0") = a;
        register long x1 asm("x1") = b;
        __asm__ __volatile__("svc 0" : "+r"(x0) : "r"(x8), "r"(x1) : "memory", "cc");
        return x0;
    }
    __attribute__((always_inline))
    static inline long sys3(long n, long a, long b, long c) {
        register long x8 asm("x8") = n;
        register long x0 asm("x0") = a;
        register long x1 asm("x1") = b;
        register long x2 asm("x2") = c;
        __asm__ __volatile__("svc 0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory", "cc");
        return x0;
    }
    __attribute__((always_inline))
    static inline long sys4(long n, long a, long b, long c, long d) {
        register long x8 asm("x8") = n;
        register long x0 asm("x0") = a;
        register long x1 asm("x1") = b;
        register long x2 asm("x2") = c;
        register long x3 asm("x3") = d;
        __asm__ __volatile__("svc 0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3) : "memory", "cc");
        return x0;
    }
    __attribute__((naked)) void _start(void) { __asm__ volatile("mov x0, sp\n bl c_main\n"); }

#elif defined(__arm__)
    #define SYS_EXIT       1
    #define SYS_WRITE      4
    #define SYS_CLOSE      6
    #define SYS_IOCTL      54
    #define SYS_GETCWD     183
    #define SYS_GETDENTS64 141
    #define SYS_OPENAT     322
    #define SYS_FSTATAT    327
    #define STAT_MODE_IDX  4

    struct ioctl_data {
        unsigned int vp_lo;
        unsigned int vp_hi;
        unsigned int rp_lo;
        unsigned int rp_hi;
        unsigned int flags;
        unsigned int _pad;
    };

    __attribute__((always_inline))
    static inline long sys1(long n, long a) {
        register long r7 asm("r7") = n;
        register long r0 asm("r0") = a;
        __asm__ __volatile__("svc 0" : "+r"(r0) : "r"(r7) : "memory", "cc");
        return r0;
    }
    __attribute__((always_inline))
    static inline long sys2(long n, long a, long b) {
        register long r7 asm("r7") = n;
        register long r0 asm("r0") = a;
        register long r1 asm("r1") = b;
        __asm__ __volatile__("svc 0" : "+r"(r0) : "r"(r7), "r"(r1) : "memory", "cc");
        return r0;
    }
    __attribute__((always_inline))
    static inline long sys3(long n, long a, long b, long c) {
        register long r7 asm("r7") = n;
        register long r0 asm("r0") = a;
        register long r1 asm("r1") = b;
        register long r2 asm("r2") = c;
        __asm__ __volatile__("svc 0" : "+r"(r0) : "r"(r7), "r"(r1), "r"(r2) : "memory", "cc");
        return r0;
    }
    __attribute__((always_inline))
    static inline long sys4(long n, long a, long b, long c, long d) {
        register long r7 asm("r7") = n;
        register long r0 asm("r0") = a;
        register long r1 asm("r1") = b;
        register long r2 asm("r2") = c;
        register long r3 asm("r3") = d;
        __asm__ __volatile__("svc 0" : "+r"(r0) : "r"(r7), "r"(r1), "r"(r2), "r"(r3) : "memory", "cc");
        return r0;
    }
    __attribute__((naked)) void _start(void) { __asm__ volatile("mov r0, sp\n bl c_main\n"); }
#else
    #error "Arch not supported"
#endif

/* --- DEFS --- */
typedef unsigned long size_t;
#define AT_FDCWD -100
#define AT_SYMLINK_NOFOLLOW 0x100
#define O_RDWR 2

#define IOCTL_ADD     0x40184E01
#define IOCTL_DEL     0x40184E02
#define IOCTL_CLEAR   0x4E03
#define IOCTL_VER     0x80044E04
#define IOCTL_ADD_UID 0x40044E05
#define IOCTL_DEL_UID 0x40044E06
#define IOCTL_LIST    0x80044E07
#define IOCTL_REFRESH   0x4E08

#define NM_ACTIVE 1
#define NM_DIR    128
#define PATH_MAX  4096

/* Helpers */
struct linux_dirent64 {
    unsigned long long d_ino;
    long long          d_off;
    unsigned short     d_reclen;
    unsigned char      d_type;
    char               d_name[];
};

static void fast_concat(char *dest, const char *s1, const char *s2) {
    while (*s1) *dest++ = *s1++;
    if (dest > (char *)dest && *(dest - 1) != '/') *dest++ = '/';
    while ((*dest++ = *s2++));
}

/* --- MAIN --- */
__attribute__((noreturn, used))
void c_main(long *sp) {
    long argc = *sp;
    char **argv = (char **)(sp + 1);
    long exit_code = 1; 
    
    if (argc < 2) {
        sys3(SYS_WRITE, 1, (long)"nm add|del|cls|blk|unblk|list\n", 30);
        goto do_exit;
    }

    int fd = sys4(SYS_OPENAT, AT_FDCWD, (long)"/dev/nomount", O_RDWR, 0);
    if (fd < 0) {
        exit_code = 2;
        goto do_exit;
    }

    char cmd = argv[1][0];
    struct ioctl_data data;
    long ioctl_code = 0;
    void *ioctl_ptr = &data;
    unsigned long uid = 0;

    if (cmd == 'a' || cmd == 'd') {
        char *v_base = argv[2];
        char *r_base = (cmd == 'a') ? argv[3] : 0;
        int v_len = 0; while(v_base[v_len]) v_len++;
        
        if (v_base[v_len-1] == '*') {
            v_base[v_len-1] = 0;
            char *p_open = (cmd == 'a') ? r_base : v_base;
            if (cmd == 'a') {
                int r_len = 0; while(r_base[r_len]) r_len++;
                if (r_base[r_len-1] == '*') r_base[r_len-1] = 0;
            }

            int dfd = sys4(SYS_OPENAT, AT_FDCWD, (long)p_open, 0, 0);
            if (dfd >= 0) {
                char *dbuf = (char *)sp - 65536; // dents buffer 
                char *fv = dbuf - 66560;        // full virtual path buffer
                char *fr = fv - 67584;          // full real path buffer
                long nr;
                while ((nr = sys3(SYS_GETDENTS64, dfd, (long)dbuf, 8192)) > 0) {
                    for (long p = 0; p < nr; ) {
                        struct linux_dirent64 *d = (struct linux_dirent64 *)(dbuf + p);
                        if (d->d_name[0] != '.' || (d->d_name[1] && (d->d_name[1] != '.' || d->d_name[2]))) {
                            fast_concat(fv, v_base, d->d_name);
                            #if defined(__aarch64__)
                                data.vp = (long)fv;
                                if (cmd == 'a') {
                                    fast_concat(fr, r_base, d->d_name);
                                    data.rp = (long)fr;
                                    data.flags = (d->d_type == 4) ? (NM_ACTIVE | NM_DIR) : NM_ACTIVE;
                                }
                            #else
                                data.vp_lo = (long)fv;
                                if (cmd == 'a') {
                                    fast_concat(fr, r_base, d->d_name);
                                    data.rp_lo = (long)fr;
                                    data.flags = (d->d_type == 4) ? (NM_ACTIVE | NM_DIR) : NM_ACTIVE;
                                }
                            #endif
                            sys3(SYS_IOCTL, fd, (cmd == 'a') ? IOCTL_ADD : IOCTL_DEL, (long)&data);
                        }
                        p += d->d_reclen;
                    }
                }
            }
            exit_code = 0;
            goto do_exit;
        }

        #if defined(__aarch64__)
            data.vp = (long)v_base;
            data.rp = (long)r_base;
        #else
            data.vp_lo = (long)v_base;
            data.rp_lo = (long)r_base;
        #endif
        data.flags = NM_ACTIVE;
        
        if (cmd == 'a') {
            unsigned int st[32];
            if (!sys4(SYS_FSTATAT, AT_FDCWD, (long)r_base, (long)st, AT_SYMLINK_NOFOLLOW))
                if ((st[4] & 0xF000) == 0x4000) data.flags |= NM_DIR;
            ioctl_code = IOCTL_ADD;
        } else ioctl_code = IOCTL_DEL;
    } else {
        if (cmd == 'c') ioctl_code = IOCTL_CLEAR;
        else if (cmd == 'l') {
            ioctl_code = IOCTL_LIST;
            ioctl_ptr = (char *)sp - 131072;
        }
        else if (cmd == 'r') ioctl_code = IOCTL_REFRESH;
        else if (cmd == 'v') ioctl_code = IOCTL_VER;
        else if (cmd == 'b' || cmd == 'u') {
            char *s = argv[2]; while (*s) uid = uid * 10 + (*s++ - '0');
            ioctl_ptr = (void *)(long)uid;
            ioctl_code = (cmd == 'b') ? IOCTL_ADD_UID : IOCTL_DEL_UID;
        }
    }

    if (ioctl_code) {
        long res = sys3(SYS_IOCTL, fd, ioctl_code, (long)ioctl_ptr);
        if (cmd == 'l' && res > 0) sys3(SYS_WRITE, 1, (long)ioctl_ptr, res);
        if (cmd == 'v') {
            char v = res + '0'; sys3(SYS_WRITE, 1, (long)&v, 1);
        }
    }

    exit_code = 0;

do_exit:
    sys1(SYS_EXIT, exit_code);
    __builtin_unreachable();
}
