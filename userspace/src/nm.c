/*
 * nm.c - NoMount CLI Userspace Tool
 */
#include "nm.h"

/* --- MAIN --- */
__attribute__((noreturn, used))
void c_main(long *sp) {
    long argc = *sp;
    char **argv = (char **)(sp + 1);
    long exit_code = 1;

    if (argc < 2) {
        print_str("nm <command>\n");
        goto do_exit;
    }

    int fd = syscall(SYS_SOCKET, AF_NETLINK, SOCK_RAW, NETLINK_GENERIC);
    if (fd < 0) { exit_code = 2; goto do_exit; }

    struct sockaddr_nl local = { .nl_family = AF_NETLINK };
    syscall(SYS_BIND, fd, (long)&local, sizeof(local));

    int nm_family = -1;
    if (do_nm_cmd(fd, GENL_ID_CTRL, CTRL_CMD_GETFAMILY, CTRL_ATTR_FAMILY_NAME, "nomount", 8, NLM_F_REQUEST) > 0) {
        unsigned short *fam_id = get_attr((struct nlmsghdr *)rx_buf, CTRL_ATTR_FAMILY_ID);
        if (fam_id) nm_family = *fam_id;
    }

    if (nm_family < 0) { exit_code = 3; goto do_exit; }

    char cmd = argv[1][0];
    if (cmd == 'a' || cmd == 'd') {
        int is_add = (cmd == 'a');
        int step = is_add ? 2 : 1;
        if (argc < 2 + step) goto do_exit;

        long cwd_len = syscall(SYS_GETCWD, (long)cwd_buf, PATH_MAX);
        const char *cwd = (cwd_len > 0) ? cwd_buf : "/";
        static char payload[MAX_PAYLOAD];
        char *cursor = payload;
        int target_cmd = is_add ? NOMOUNT_CMD_ADD_RULE : NOMOUNT_CMD_DEL_RULE;
        exit_code = 0;

        for (int arg_idx = 2; arg_idx + step <= argc; arg_idx += step) {
            int v_len = resolve_path(v_resolved, cwd, argv[arg_idx]);
            if (!v_len) { exit_code = 3; continue; }

            int r_len = 0;
            if (is_add) {
                r_len = resolve_path(r_resolved, cwd, argv[arg_idx+1]);
                if (!r_len) { exit_code = 3; continue; }
            }

            int item_size = is_add ? (8 + v_len + r_len) : (2 + v_len);
            if ((cursor - payload) + item_size > MAX_PAYLOAD) {
                if (do_nm_cmd(fd, nm_family, target_cmd, NOMOUNT_ATTR_PAYLOAD, payload, cursor - payload, NLM_F_REQUEST | NLM_F_ACK) < 0) exit_code = 1;
                cursor = payload;
            }

            u16 vp_len_u = (u16)v_len;
            if (is_add) {
                *(u32*)cursor = 0; cursor += 4;
                *(u16*)cursor = vp_len_u; cursor += 2;
                *(u16*)cursor = (u16)r_len; cursor += 2;
                memcpy(cursor, v_resolved, v_len); cursor += v_len;
                memcpy(cursor, r_resolved, r_len); cursor += r_len;
            } else {
                *(u16*)cursor = vp_len_u; cursor += 2;
                memcpy(cursor, v_resolved, v_len); cursor += v_len;
            }
        }

        if (cursor > payload) {
            if (do_nm_cmd(fd, nm_family, target_cmd, NOMOUNT_ATTR_PAYLOAD, payload, cursor - payload, NLM_F_REQUEST | NLM_F_ACK) < 0) exit_code = 1;
        }
        goto do_exit;

    } else if (cmd == 'b' || cmd == 'u') {
        if (argc < 3) goto do_exit;
        unsigned int uid = 0; const char *s = argv[2];
        while (*s) uid = (uid << 3) + (uid << 1) + (*s++ - '0');
        do_nm_cmd(fd, nm_family, (cmd == 'b') ? NOMOUNT_CMD_ADD_UID : NOMOUNT_CMD_DEL_UID, NOMOUNT_ATTR_UID, &uid, 4, NLM_F_REQUEST | NLM_F_ACK);

    } else if (cmd == 'c') {
        do_nm_cmd(fd, nm_family, NOMOUNT_CMD_CLEAR_ALL, 0, NULL, 0, NLM_F_REQUEST | NLM_F_ACK);

    } else if (cmd == 'v') {
        if (do_nm_cmd(fd, nm_family, NOMOUNT_CMD_GET_VERSION, 0, NULL, 0, NLM_F_REQUEST | NLM_F_ACK) > 0) {
            unsigned int *ver = get_attr((struct nlmsghdr *)rx_buf, NOMOUNT_ATTR_VERSION);
            if (ver) {
                char v_str[16]; char *p = v_str + 15;
                unsigned int v = *ver;
                *p-- = '\0'; *p-- = '\n';
                do { *p-- = (v % 10) + '0'; v /= 10; } while (v);
                print_str(p + 1);
                exit_code = 0;
            }
        }

    } else if (cmd == 'l') {
        long len = do_nm_cmd(fd, nm_family, NOMOUNT_CMD_GET_LIST, 0, NULL, 0, NLM_F_REQUEST | NLM_F_DUMP);
        int is_json = (argc > 2 && argv[2][0] == 'j');
        int first = 1;
        if (is_json) print_str("[\n");

        while (len > 0) {
            for (struct nlmsghdr *msg = (struct nlmsghdr *)rx_buf; NLMSG_OK(msg, len); msg = NLMSG_NEXT(msg, len)) {
                if (msg->nlmsg_type == NLMSG_DONE || msg->nlmsg_type == NLMSG_ERROR) goto list_done;
                char *v = get_attr(msg, NOMOUNT_ATTR_VIRTUAL_PATH);
                char *r = get_attr(msg, NOMOUNT_ATTR_REAL_PATH);

                if (v && r) {
                    if (is_json) {
                        if (!first) print_str(",\n");
                        first = 0;
                        print_str("  {\n    \"virtual\": \""); print_str(v);
                        print_str("\",\n    \"real\": \""); print_str(r);
                        print_str("\"\n  }");
                    } else {
                        print_str(v); print_str(" -> "); print_str(r); print_str("\n");
                    }
                }
            }
            len = syscall(SYS_RECVFROM, fd, (long)rx_buf, RX_BUF_SIZE, 0, 0, 0);
        }
list_done:
        if (is_json) print_str("\n]\n");
        exit_code = 0; goto do_exit;
    }

do_exit:
    if (fd >= 0) syscall(SYS_CLOSE, fd);
    syscall(SYS_EXIT, exit_code);
    __builtin_unreachable();
}
