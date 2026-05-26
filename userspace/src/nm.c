/*
 * nm.c - NoMount CLI Userspace Tool
 *
 */
#include "nm.h"

/* --- MAIN --- */
__attribute__((noreturn, used))
void c_main(long *sp) {
    long argc = *sp;
    char **argv = (char **)(sp + 1);
    long exit_code = 1; 
    
    if (argc < 2) {
        print_str("nm add|del|cls|blk|unblk|ls\n");
        goto do_exit;
    }

    int fd = sys3(SYS_SOCKET, AF_NETLINK, SOCK_RAW, NETLINK_GENERIC);
    if (fd < 0) { exit_code = 2; goto do_exit; }

    struct sockaddr_nl local = { .nl_family = AF_NETLINK };
    sys3(SYS_BIND, fd, (long)&local, sizeof(local));

    int nm_family = get_nomount_family_id(fd);
    if (nm_family < 0) {
        print_str("Error: NoMount kernel module not loaded.\n");
        exit_code = 3; goto do_exit;
    }

    char cmd = argv[1][0];
    struct nlmsghdr *nlh = NULL;

    switch (cmd) {
        case 'a':
        case 'd': {
            int is_add = (cmd == 'a');
            int step = is_add ? 2 : 1;
            if (argc < 2 + step) goto do_exit;

            long cwd_len = sys2(SYS_GETCWD, (long)cwd_buf, PATH_MAX);
            const char *cwd = (cwd_len > 0) ? cwd_buf : "/";
            static char payload[MAX_PAYLOAD];
            char *cursor = payload;
            int target_cmd = is_add ? NOMOUNT_CMD_ADD_RULE : NOMOUNT_CMD_DEL_RULE;
            exit_code = 0;

            for (int arg_idx = 2; arg_idx + step <= argc; arg_idx += step) {
                int v_len = resolve_path(v_resolved, cwd, argv[arg_idx], PATH_MAX);
                if (!v_len) { exit_code = 3; continue; }

                int r_len = 0;
                if (is_add) {
                    r_len = resolve_path(r_resolved, cwd, argv[arg_idx+1], PATH_MAX);
                    if (!r_len) { exit_code = 3; continue; }
                }

                int item_size = is_add ? (8 + v_len + r_len) : (2 + v_len);
                if ((cursor - payload) + item_size > MAX_PAYLOAD) {
                    nlh = init_msg(nm_family, target_cmd, NLM_F_REQUEST | NLM_F_ACK);
                    add_attr(nlh, NOMOUNT_ATTR_PAYLOAD, payload, cursor - payload);
                    if (send_and_recv(fd, nlh) < 0) exit_code = 1;
                    cursor = payload;
                }

                u16 vp_len_u = (u16)v_len;
                if (is_add) {
                    u32 flags = 0; u16 rp_len_u = (u16)r_len;
                    memcpy(cursor, &flags, 4); cursor += 4;
                    memcpy(cursor, &vp_len_u, 2); cursor += 2;
                    memcpy(cursor, &rp_len_u, 2); cursor += 2;
                    memcpy(cursor, v_resolved, v_len); cursor += v_len;
                    memcpy(cursor, r_resolved, r_len); cursor += r_len;
                } else {
                    memcpy(cursor, &vp_len_u, 2); cursor += 2;
                    memcpy(cursor, v_resolved, v_len); cursor += v_len;
                }
            }

            if (cursor > payload) {
                nlh = init_msg(nm_family, target_cmd, NLM_F_REQUEST | NLM_F_ACK);
                add_attr(nlh, NOMOUNT_ATTR_PAYLOAD, payload, cursor - payload);
                if (send_and_recv(fd, nlh) < 0) exit_code = 1;
            }
            goto do_exit;
        }
        case 'b':
        case 'u': {
            if (argc < 3) goto do_exit;
            unsigned int uid = 0; const char *s = argv[2];
            while (*s) uid = uid * 10 + (*s++ - '0');
            nlh = init_msg(nm_family, (cmd == 'b') ? NOMOUNT_CMD_ADD_UID : NOMOUNT_CMD_DEL_UID, NLM_F_REQUEST | NLM_F_ACK);
            add_attr(nlh, NOMOUNT_ATTR_UID, &uid, sizeof(uid));
            break;
        }
        case 'c':
            nlh = init_msg(nm_family, NOMOUNT_CMD_CLEAR_ALL, NLM_F_REQUEST | NLM_F_ACK);
            break;
        case 'v':
            nlh = init_msg(nm_family, NOMOUNT_CMD_GET_VERSION, NLM_F_REQUEST | NLM_F_ACK);
            break;
        case 'l': {
            nlh = init_msg(nm_family, NOMOUNT_CMD_GET_LIST, NLM_F_REQUEST | NLM_F_DUMP);
            struct sockaddr_nl dest = { .nl_family = AF_NETLINK };
            sys6(SYS_SENDTO, fd, (long)nlh, nlh->nlmsg_len, 0, (long)&dest, sizeof(dest));

            int is_json = (argc > 2 && argv[2][0] == 'j');
            if (is_json) print_str("[\n");
            int first = 1;

            while (1) {
                long len = sys6(SYS_RECVFROM, fd, (long)rx_buf, RX_BUF_SIZE, 0, 0, 0);
                if (len <= 0) break;
                int stop = 0;
                for (struct nlmsghdr *msg = (struct nlmsghdr *)rx_buf; NLMSG_OK(msg, len); msg = NLMSG_NEXT(msg, len)) {
                    if (msg->nlmsg_type == NLMSG_DONE || msg->nlmsg_type == NLMSG_ERROR) { stop = 1; break; }
                    struct nlattr *tb[NOMOUNT_ATTR_PAYLOAD + 1];
                    parse_attrs(tb, NOMOUNT_ATTR_PAYLOAD, (struct nlattr *)((char *)msg + NLMSG_HDRLEN + NLMSG_ALIGN(sizeof(struct genlmsghdr))), msg->nlmsg_len - NLMSG_HDRLEN - NLMSG_ALIGN(sizeof(struct genlmsghdr)));
                    if (tb[NOMOUNT_ATTR_VIRTUAL_PATH] && tb[NOMOUNT_ATTR_REAL_PATH]) {
                        char *v = (char *)tb[NOMOUNT_ATTR_VIRTUAL_PATH] + NLA_HDRLEN;
                        char *r = (char *)tb[NOMOUNT_ATTR_REAL_PATH] + NLA_HDRLEN;
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
                if (stop) break;
            }
            if (is_json) print_str("\n]\n");
            exit_code = 0; goto do_exit;
        }
    }

    if (nlh && send_and_recv(fd, nlh) >= 0) {
        exit_code = 0;
        if (cmd == 'v') {
            struct nlattr *tb[6];
            parse_attrs(tb, 5, (struct nlattr *)(rx_buf + NLMSG_HDRLEN + NLMSG_ALIGN(sizeof(struct genlmsghdr))), 
                        ((struct nlmsghdr *)rx_buf)->nlmsg_len - NLMSG_HDRLEN - NLMSG_ALIGN(sizeof(struct genlmsghdr)));
            if (tb[NOMOUNT_ATTR_VERSION]) {
                char v_str[2] = { *(unsigned int *)((char *)tb[NOMOUNT_ATTR_VERSION] + NLA_HDRLEN) + '0', '\n' };
                print_str(v_str);
            }
        }
    }

do_exit:
    if (fd >= 0) sys1(SYS_CLOSE, fd);
    sys1(SYS_EXIT, exit_code);
    __builtin_unreachable();
}
