/*
 * nm.c - NoMount CLI Userspace Tool
 */
#include "nm.h"

/* --- MAIN --- */
__attribute__((noreturn, used))
void c_main(long *sp) {
    long argc = *sp;
    char **argv = (char **)(sp + 1);
    int exit_code = 1;

    struct nm_payload *payload = (void *)(((long)sp - 16384) & ~4095L);
    enum nm_cli_action action = ACTION_NONE;
    int data_start_idx = 2;
    int is_json = 0;
    int is_whiteout = 0;
    unsigned int target_uid = 0;

    if (argc >= 2) {
        const char *c1 = argv[1];
        if (strcmp(c1, "rule") == 0 && argc >= 3) {
            const char *c2 = argv[2];
            if (strcmp(c2, "add") == 0) action = ACTION_RULE_ADD;
            else if (strcmp(c2, "del") == 0) action = ACTION_RULE_DEL;
            else if (strcmp(c2, "list") == 0) action = ACTION_RULE_LIST;
            else if (strcmp(c2, "clear") == 0) action = ACTION_RULE_CLEAR;
            data_start_idx = 3;
        } else if (strcmp(c1, "uid") == 0 && argc >= 3) {
            const char *c2 = argv[2];
            if (strcmp(c2, "add") == 0) action = ACTION_UID_ADD;
            else if (strcmp(c2, "del") == 0) action = ACTION_UID_DEL;
            else if (strcmp(c2, "list") == 0) action = ACTION_UID_LIST;
            else if (strcmp(c2, "clear") == 0) action = ACTION_UID_CLEAR;
            data_start_idx = 3;
        } else if (strcmp(c1, "clear") == 0) {
            if (argc >= 3 && strcmp(argv[2], "rules") == 0) action = ACTION_RULE_CLEAR;
            else if (argc >= 3 && strcmp(argv[2], "uid") == 0) action = ACTION_UID_CLEAR;
            else if (argc >= 3 && strcmp(argv[2], "all") == 0) action = ACTION_CLEAR_ALL;
            else action = ACTION_CLEAR_ALL;
            data_start_idx = (argc >= 3) ? 3 : 2;
        }
        /* legacy commands */
        else if (strcmp(c1, "add") == 0 || strcmp(c1, "a") == 0) { action = ACTION_RULE_ADD; }
        else if (strcmp(c1, "w") == 0 || strcmp(c1, "whiteout") == 0) { action = ACTION_RULE_ADD; is_whiteout = 1; }
        else if (strcmp(c1, "del") == 0 || strcmp(c1, "d") == 0) { action = ACTION_RULE_DEL; }
        else if (strcmp(c1, "block") == 0 || strcmp(c1, "b") == 0) { action = ACTION_UID_ADD; }
        else if (strcmp(c1, "unblock") == 0 || strcmp(c1, "u") == 0) { action = ACTION_UID_DEL; }
        else if (strcmp(c1, "list") == 0 || strcmp(c1, "l") == 0) {
            if (argc >= 3 && strcmp(argv[2], "uid") == 0) { action = ACTION_UID_LIST; data_start_idx = 3; }
            else { action = ACTION_RULE_LIST; }
        } else if (strcmp(c1, "version") == 0 || strcmp(c1, "v") == 0 || strcmp(c1, "-v") == 0) { action = ACTION_VERSION; }
    }

    const char *p_args[argc > 0 ? argc : 1];
    int p_count = 0;

    if (argc >= 2) {
        for (int i = data_start_idx; i < argc; i++) {
            if (strcmp(argv[i], "--uid") == 0 && i + 1 < argc) {
                const char *s = argv[++i];
                while (*s) target_uid = (target_uid << 3) + (target_uid << 1) + (*s++ - '0');
            } 
            else if (strcmp(argv[i], "--json") == 0 || strcmp(argv[i], "json") == 0) { is_json = 1; }
            else if (strcmp(argv[i], "--whiteout") == 0) { is_whiteout = 1; }
            else { p_args[p_count++] = argv[i]; }
        }
    }

    switch (action) {
        case ACTION_RULE_ADD:
        case ACTION_RULE_DEL: {
            int step = (action == ACTION_RULE_ADD && !is_whiteout) ? 2 : 1;
            if (p_count < step) { exit_code = 0; goto do_exit; }

            char *cwd_buf = (char *)sp - 12288;
            const char *cwd = (sys3(SYS_GETCWD, (long)cwd_buf, PATH_MAX, 0) > 0) ? cwd_buf : "/";
            int target_cmd = (action == ACTION_RULE_DEL) ? NM_CMD_DEL_RULE : NM_CMD_ADD_RULE;

            exit_code = 0;
            payload->cmd = target_cmd;
            payload->arg1 = 0;
            payload->data_size = 0;
            char *cursor = payload->buffer;

            for (int i = 0; i + step - 1 < p_count; i += step) {
                char *v_resolved = (char *)sp - 8196;
                char *v_end = resolve_path(v_resolved, cwd, p_args[i]);
                int v_len = v_end - v_resolved;
                if (!v_len) { exit_code = 3; continue; }

                int r_len = 0;
                char *r_resolved = (char *)sp - 4096;
                if (action == ACTION_RULE_ADD && !is_whiteout) {
                    char *r_end = resolve_path(r_resolved, cwd, p_args[i+1]);
                    r_len = r_end - r_resolved;
                    if (!r_len) { exit_code = 3; continue; }
                }

                int header_size = (target_cmd == NM_CMD_ADD_RULE) ? 12 : 6;
                if ((cursor - payload->buffer) + header_size + v_len + r_len > 3900) {
                    payload->data_size = cursor - payload->buffer;
                    exit_code |= (nm_send_payload(payload) < 0);
                    cursor = payload->buffer;
                    payload->cmd = target_cmd;
                    payload->arg1 = 0;
                }

                if (target_cmd == NM_CMD_ADD_RULE) {
                    *(unsigned int*)cursor = (is_whiteout) ? 4 : 0;
                    *(unsigned int*)(cursor + 4) = target_uid;
                    *(unsigned short*)(cursor + 8) = v_len;
                    *(unsigned short*)(cursor + 10) = r_len;
                    memcpy(cursor + 12, v_resolved, v_len);
                    if (r_len > 0) memcpy(cursor + 12 + v_len, r_resolved, r_len);
                    cursor += 12 + v_len + r_len;
                } else {
                    *(unsigned int*)cursor = target_uid;
                    *(unsigned short*)(cursor + 4) = v_len;
                    memcpy(cursor + 6, v_resolved, v_len);
                    cursor += 6 + v_len;
                }
            }

            if (cursor > payload->buffer) {
                payload->data_size = cursor - payload->buffer;
                exit_code |= (nm_send_payload(payload) < 0);
            }
            break;
        }

        case ACTION_UID_ADD:
        case ACTION_UID_DEL: {
            if (p_count < 1) goto do_exit;
            unsigned int uid = 0; const char *s = p_args[0];
            while (*s) uid = (uid << 3) + (uid << 1) + (*s++ - '0');
            payload->cmd = (action == ACTION_UID_ADD) ? NM_CMD_ADD_UID : NM_CMD_DEL_UID;
            payload->target_uid = uid;
            exit_code = (nm_send_payload(payload) < 0);
            break;
        }

        case ACTION_CLEAR_ALL: {
            payload->cmd = NM_CMD_CLEAR_ALL;
            exit_code = (nm_send_payload(payload) < 0);
            break;
        }

        case ACTION_RULE_CLEAR:
        case ACTION_UID_CLEAR: {
            payload->cmd = (action == ACTION_RULE_CLEAR) ? NM_CMD_CLEAR_RULES : NM_CMD_CLEAR_UIDS;
            exit_code = (nm_send_payload(payload) < 0);
            break;
        }

        case ACTION_VERSION: {
            payload->cmd = NM_CMD_GET_VERSION;
            if (nm_send_payload(payload) == 0) {
                print_strn(payload->buffer, payload->data_size); print_str("\n");
                exit_code = 0;
            }
            break;
        }

        case ACTION_RULE_LIST:
        case ACTION_UID_LIST: {
            int is_uids = (action == ACTION_UID_LIST);
            if (is_uids) is_json = 1;
            if (is_json) print_str("[\n");
            int offset = 2;

            payload->cmd = is_uids ? NM_CMD_GET_UIDS : NM_CMD_GET_LIST;
            payload->arg1 = 0;
            while (1) {
                if (nm_send_payload(payload) < 0 || payload->data_size == 0) break;

                char *data = payload->buffer;
                int pos = 0;
                while (pos < payload->data_size) {
                    if (is_uids) {
                        unsigned int uid = *(unsigned int *)(data + pos);
                        pos += 4;
                        if (offset == 0) print_str(",\n");
                        print_str("  "); print_uint(uid);
                        offset = 0;
                    } else {
                        unsigned int flags = *(unsigned int *)(data + pos);
                        unsigned int uid   = *(unsigned int *)(data + pos + 4);
                        unsigned short vlen = *(unsigned short *)(data + pos + 8);
                        unsigned short rlen = *(unsigned short *)(data + pos + 10);
                        pos += 12;

                        char *v = data + pos; pos += vlen;
                        char *r = data + pos; pos += rlen;
                        int is_white_flag  = (flags & 4);
                        int is_virtual_dir = (flags & 2); 

                        if (is_json) {
                            print_str((const char *)",\n  {\n    \"virtual\": \"" + offset); offset = 0;
                            print_strn(v, vlen);
                            if (is_white_flag) print_str("\",\n    \"whiteout\": true");
                            else if (is_virtual_dir) print_str("\",\n    \"virtual_dir\": true");
                            else { print_str("\",\n    \"real\": \""); print_strn(r, rlen); print_str("\""); }
                            if (uid != 0) { print_str(",\n    \"uid\": "); print_uint(uid); }
                            print_str("\n  }");
                        } else {
                            print_strn(v, vlen);
                            if (is_white_flag) print_str(" (whiteout)");
                            else if (is_virtual_dir) print_str(" (virtual dir)");
                            else { print_str(" -> "); print_strn(r, rlen); }
                            if (uid != 0) { print_str(" [UID: "); print_uint(uid); print_str("]"); }
                            print_str("\n");
                        }
                    }
                }
                payload->cmd = is_uids ? NM_CMD_GET_UIDS : NM_CMD_GET_LIST;
            }

            if (is_json) print_str("\n]\n");
            exit_code = 0;
            break;
        }

        case ACTION_NONE:
        default: {
            print_str("Usage:\n"
                      "  nm rule {add, del, list, clear}\n"
                      "  nm uid {add, del, list, clear}\n"
                      "  nm clear all\n");
            exit_code = 1;
            break;
        }
    }

do_exit:
    sys1(SYS_EXIT, exit_code);
    __builtin_unreachable();
}
