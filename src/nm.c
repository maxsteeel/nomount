/*
 * nm.c - NoMount CLI Userspace Tool 
 */

/* --- ARCH --- */
#if defined(__aarch64__)
    #define SYS_GETCWD     17
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
#define O_RDWR 2

#define IOCTL_ADD     0x40184E01
#define IOCTL_DEL     0x40184E02
#define IOCTL_CLEAR   0x4E03
#define IOCTL_VER     0x80044E04
#define IOCTL_ADD_UID 0x40044E05
#define IOCTL_DEL_UID 0x40044E06
#define IOCTL_LIST    0x80044E07

#define NM_ACTIVE 1
#define NM_DIR    128
#define PATH_MAX  4096

/* --- MAIN --- */
__attribute__((noreturn, used))
void c_main(long *sp) {
    long argc = *sp;
    char **argv = (char **)(sp + 1);
    long exit_code = 1; 
    
    if (argc < 2) {
        sys3(SYS_WRITE, 1, (long)"nm add|del|clear|blk|unb|list\n", 30);
        goto do_exit;
    }

    int fd = sys4(SYS_OPENAT, AT_FDCWD, (long)"/dev/nomount", O_RDWR, 0);
    if (fd < 0) {
        exit_code = 2;
        goto do_exit;
    }

    char cmd = argv[1][0];
    struct ioctl_data data;
    void *ioctl_arg = 0;
    unsigned int uid = 0;
    long ioctl_code = 0;
    int needed = 2;
    if (cmd == 'a') needed = 4;
    else if (cmd != 'c' && cmd != 'v' && cmd != 'l') needed = 3; 
    
    if (argc < needed) goto do_exit;

    if (cmd == 'a' || cmd == 'd') {
        #if defined(__aarch64__)
            data.vp = (unsigned long)argv[2];
        #else
            data.vp_lo = (unsigned int)argv[2];
            data.vp_hi = 0;
        #endif
        ioctl_arg = &data;

        if (cmd == 'd') {
            ioctl_code = IOCTL_DEL;
        } else { 
            char *src = argv[3];
            char *dst_ptr;
            char *path_buf = (char *)sp;

            if (src[0] != '/') {
                long l = sys2(SYS_GETCWD, (long)path_buf, PATH_MAX);
                if (l > 0) {
                    if (path_buf[l-1] == 0) l--;
                    path_buf[l] = '/';
                    char *s = src;
                    char *d = path_buf + l + 1;
                    while ((*d++ = *s++));
                    dst_ptr = path_buf;
                } else {
                    dst_ptr = src;
                }
            } else {
                dst_ptr = src;
            }

            #if defined(__aarch64__)
                data.rp = (unsigned long)dst_ptr;
            #else
                data.rp_lo = (unsigned int)dst_ptr;
                data.rp_hi = 0;
            #endif
            
            data.flags = NM_ACTIVE;
            
            unsigned int *stat_buf = (unsigned int *)((char*)sp + 256);
            if (sys4(SYS_FSTATAT, AT_FDCWD, (long)dst_ptr, (long)stat_buf, 0) == 0) {
                unsigned int mode = stat_buf[STAT_MODE_IDX];
                if ((mode & 0170000) == 0040000) data.flags |= NM_DIR;
            }
            ioctl_code = IOCTL_ADD;
        }
    } 
    else if (cmd == 'b' || cmd == 'u') {
        const char *s = argv[2];
        while (*s) uid = uid * 10 + (*s++ - '0');
        ioctl_arg = &uid;
        ioctl_code = (cmd == 'b') ? IOCTL_ADD_UID : IOCTL_DEL_UID;
    }
    else if (cmd == 'c') {
        ioctl_code = IOCTL_CLEAR;
    }
    else if (cmd == 'v') {
        ioctl_code = IOCTL_VER;
    }
    else if (cmd == 'l') {
        ioctl_code = IOCTL_LIST;
        ioctl_arg = (void *)((char *)sp - 65536); 
    }

    if (ioctl_code) {
        long res = sys3(SYS_IOCTL, fd, ioctl_code, (long)ioctl_arg);
        
        if (cmd == 'v' && res > 0) {
            char v_buf[2] = {res + '0', '\n'};
            sys3(SYS_WRITE, 1, (long)v_buf, 2);
        }
        else if (cmd == 'l' && res > 0) {
            sys3(SYS_WRITE, 1, (long)ioctl_arg, res);
        }
    }

    exit_code = 0;

do_exit:
    sys1(SYS_EXIT, exit_code);
    __builtin_unreachable();
}
