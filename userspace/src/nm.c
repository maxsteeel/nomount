/*
 * nm.c - NoMount CLI Userspace Tool
 */
#include "nm.h"

/* --- MAIN --- */
__attribute__((noreturn, used))
void c_main(long *sp) {
    struct nm_mem mem __attribute__((aligned(16)));
    long argc = *sp;
    char **argv = (char **)(sp + 1);
    int exit_code = 1;

    if (argc < 2) {
        print_str("nm <command>\n");
        goto do_exit;
    }

    int fd = sys3(SYS_SOCKET, AF_NETLINK, SOCK_RAW, NETLINK_GENERIC);
    if (fd < 0) { exit_code = 2; goto do_exit; }

    int nm_family = -1;
    if (do_nm_cmd(fd, 16, 3, 2, "nomount", 8, 1, &mem) > 0) {
        unsigned short *fam_id = get_attr(mem.rx_buf, 1);
        if (fam_id) nm_family = *fam_id;
    }
    if (nm_family < 0) { exit_code = 3; goto do_exit; }

    char cmd = argv[1][0];

    if (cmd == 'a' || cmd == 'd' || cmd == 'w') {
        int step = 1 + (cmd == 'a');
        if (argc < 2 + step) { exit_code = 0; goto do_exit; }

        const char *cwd = (sys3(SYS_GETCWD, (long)mem.cwd_buf, PATH_MAX, 0) > 0) ? mem.cwd_buf : "/";
        char *cursor = mem.payload;

        int target_cmd = 2 + (cmd == 'd');
        exit_code = 0;

        for (int i = 2; i + step - 1 < argc; i += step) {
            char *v_end = resolve_path(mem.v_resolved, cwd, argv[i]);
            int v_len = v_end - mem.v_resolved;
            if (!v_len) { exit_code = 3; continue; }

            int r_len = 0;
            if (cmd == 'a') {
                char *r_end = resolve_path(mem.r_resolved, cwd, argv[i+1]);
                r_len = r_end - mem.r_resolved;
                if (!r_len) { exit_code = 3; continue; }
            }

            if ((cursor - mem.payload) + 8 + v_len + r_len > MAX_PAYLOAD) {
                exit_code |= (do_nm_cmd(fd, nm_family, target_cmd, 6, mem.payload, cursor - mem.payload, 5, &mem) < 0);
                cursor = mem.payload;
            }

            if (target_cmd == 2) {
                *(unsigned int*)cursor = (cmd == 'w') ? 4 : 0; 
                *(unsigned short*)(cursor + 4) = v_len;
                *(unsigned short*)(cursor + 6) = r_len;
                memcpy(cursor + 8, mem.v_resolved, v_len);
                if (r_len > 0) memcpy(cursor + 8 + v_len, mem.r_resolved, r_len);
                cursor += 8 + v_len + r_len;
            } else {
                *(unsigned short*)cursor = v_len;
                memcpy(cursor + 2, mem.v_resolved, v_len);
                cursor += 2 + v_len;
            }
        }

        if (cursor > mem.payload)
            exit_code |= (do_nm_cmd(fd, nm_family, target_cmd, 6, mem.payload, cursor - mem.payload, 5, &mem) < 0);

        goto do_exit;

    } else if (cmd == 'b' || cmd == 'u') {
        if (argc < 3) goto do_exit;
        unsigned int uid = 0; const char *s = argv[2];
        while (*s) uid = (uid << 3) + (uid << 1) + (*s++ - '0');
        exit_code = (do_nm_cmd(fd, nm_family, 6 - (cmd == 'b'), 4, &uid, 4, 5, &mem) < 0);
        goto do_exit;

    } else if (cmd == 'c') {
        exit_code = (do_nm_cmd(fd, nm_family, 4, 0, (void *)0, 0, 5, &mem) < 0);
        goto do_exit;

    } else if (cmd == 'v') {
        if (do_nm_cmd(fd, nm_family, 1, 0, (void *)0, 0, 1, &mem) > 0) {
            unsigned int *ver = get_attr(mem.rx_buf, 5);
            if (ver) {
                unsigned int v = *ver; char v_str[4] = {0};
                unsigned char tens = ((v << 7) + (v << 6) + (v << 3) + (v << 2) + v) >> 11;
                v = v - ((tens << 3) + (tens << 1));
                v_str[0] = tens + '0'; v_str[1] = v + '0'; v_str[2] = '\n';
                print_str(v_str);
                exit_code = 0; goto do_exit;
            }
        }

    } else if (cmd == 'l') {
        unsigned int len = do_nm_cmd(fd, nm_family, 7, 0, (void *)0, 0, 0x301, &mem);
        int is_json = (argc > 2 && argv[2][0] == 'j');
        int offset = 2;
        if (is_json) print_str("[\n");

        while (len > 0) {
            for (struct nlmsghdr *msg = (void *)mem.rx_buf; msg->nlmsg_len && msg->nlmsg_len <= len;
                    len -= msg->nlmsg_len, msg = (void *)((char *)msg + msg->nlmsg_len)) {
                if (msg->nlmsg_type == 3 || msg->nlmsg_type == 2) goto list_done; 
                char *v = get_attr(msg, 1); 
                char *r = get_attr(msg, 2); 
                unsigned int *flags = get_attr(msg, 3);

                if (v && r) {
                    int is_whiteout = (flags && (*flags & 4));
                    if (is_json) {
                        print_str((const char *)",\n  {\n    \"virtual\": \"" + offset); offset = 0;
                        print_str(v);
                        if (is_whiteout) {
                            print_str("\",\n    \"whiteout\": true\n  }");
                        } else {
                            print_str("\",\n    \"real\": \""); print_str(r); print_str("\"\n  }");
                        }
                    } else {
                        print_str(v);
                        if (is_whiteout) {
                            print_str(" (whiteout)\n");
                        } else {
                            print_str(" -> "); print_str(r); print_str("\n");
                        }
                    }
                }
            }
            len = sys3(SYS_READ, fd, (long)mem.rx_buf, RX_BUF_SIZE);
        }
list_done:
        if (is_json) print_str("\n]\n");
        exit_code = 0;
    }

do_exit:
    sys1(SYS_EXIT, exit_code);
    __builtin_unreachable();
}
