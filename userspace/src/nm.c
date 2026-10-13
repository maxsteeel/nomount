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

    struct nm_workspace workspace;
    struct nm_payload *payload = &workspace.payload;
    struct nm_args args;
    parse_arguments(argc, argv, &args);
    payload->magic = NOMOUNT_MAGIC_SIG;

    int p_count = 0;
    if (argc >= 2) {
        for (int i = args.data_start_idx; i < argc; i++) {
            if (strcmp(argv[i], "--uid") == 0 && i + 1 < argc) {
                const char *s = argv[++i];
                while (*s) args.target_uid = (args.target_uid << 3) + (args.target_uid << 1) + (*s++ - '0');
            } 
            else if (strcmp(argv[i], "--json") == 0 || strcmp(argv[i], "json") == 0) { args.is_json = 1; }
            else if (strcmp(argv[i], "--whiteout") == 0) { args.is_whiteout = 1; }
            else { argv[p_count++] = argv[i]; }
        }
    }

    switch (args.action) {
        case ACTION_RULE_ADD:
        case ACTION_RULE_DEL: {
            int step = (args.action == ACTION_RULE_ADD && !args.is_whiteout) ? 2 : 1;
            if (p_count < step) { exit_code = 0; goto do_exit; }

            char *cwd_buf = workspace.cwd;
            const char *cwd = (sys3(SYS_GETCWD, (long)cwd_buf, PATH_MAX, 0) > 0) ? cwd_buf : "/";
            int target_cmd = (args.action == ACTION_RULE_DEL) ? NM_CMD_DEL_RULE : NM_CMD_ADD_RULE;

            exit_code = 0;
            payload->cmd = target_cmd;
            payload->arg1 = 0;
            payload->data_size = 0;
            char *cursor = payload->buffer;

            for (int i = 0; i + step - 1 < p_count; i += step) {
                int v_len = resolved_path_length(cwd, argv[i]);
                if (v_len < 0) { exit_code = 3; continue; }
                if (!v_len) { exit_code = 3; continue; }

                int r_len = 0;
                if (args.action == ACTION_RULE_ADD && !args.is_whiteout) {
                    r_len = resolved_path_length(cwd, argv[i+1]);
                    if (r_len < 0) { exit_code = 3; continue; }
                    if (!r_len) { exit_code = 3; continue; }
                }

                int header_size = (target_cmd == NM_CMD_ADD_RULE) ? sizeof(struct nm_rule_hdr) : sizeof(struct nm_del_hdr);
                int record_size = header_size + v_len + r_len;
                if (record_size > sizeof(payload->buffer)) { exit_code = 3; continue; }
                if ((cursor - payload->buffer) + record_size > sizeof(payload->buffer)) {
                    payload->data_size = cursor - payload->buffer;
                    exit_code |= (nm_send_payload(payload) < 0);
                    cursor = payload->buffer;
                    payload->arg1 = 0;
                }

                if (target_cmd == NM_CMD_ADD_RULE) {
                    struct nm_rule_hdr *h = (void *)cursor;
                    h->flags = (args.is_whiteout) ? 4 : 0; h->uid = args.target_uid;
                    h->v_len = v_len; h->r_len = r_len;

                    cursor = resolve_path(cursor + sizeof(*h), cwd, argv[i]);
                    if (r_len > 0) cursor = resolve_path(cursor, cwd, argv[i+1]);
                } else {
                    struct nm_del_hdr *h = (void *)cursor;
                    h->uid = args.target_uid; h->v_len = v_len;

                    cursor = resolve_path(cursor + sizeof(*h), cwd, argv[i]);
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
            payload->cmd = (args.action == ACTION_UID_ADD) ? NM_CMD_ADD_UID : NM_CMD_DEL_UID;
            exit_code = 0;
            for (int i = 0; i < p_count; i++) {
                unsigned int uid = 0;
                const char *s = argv[i];
                while (*s) uid = (uid << 3) + (uid << 1) + (*s++ - '0');
                payload->target_uid = uid;
                exit_code |= (nm_send_payload(payload) < 0);
            }
            break;
        }

        case ACTION_BLOCK_ISOLATED_UIDS: {
            if (p_count == 0) {
                payload->cmd = NM_CMD_GET_ISOLATED_STATE;
                exit_code = (nm_send_payload(payload) < 0);
                if (exit_code == 0 && payload->data_size > 0)
                    print_strn(payload->buffer, payload->data_size);
            } else {
                payload->cmd = NM_CMD_BLOCK_ISOLATED_UIDS;
                if (strcmp(argv[0], "on") == 0 || strcmp(argv[0], "1") == 0) {
                    payload->arg1 = 1;
                } else if (strcmp(argv[0], "off") == 0 || strcmp(argv[0], "0") == 0) {
                    payload->arg1 = 0;
                } else {
                    exit_code = 1;
                    goto do_exit;
                }
                exit_code = (nm_send_payload(payload) < 0);
            }
            break;
        }

        case ACTION_CLEAR_ALL: {
            payload->cmd = NM_CMD_CLEAR_ALL;
            exit_code = (nm_send_payload(payload) < 0);
            break;
        }

        case ACTION_RULE_CLEAR:
        case ACTION_UID_CLEAR: {
            payload->cmd = (args.action == ACTION_RULE_CLEAR) ? NM_CMD_CLEAR_RULES : NM_CMD_CLEAR_UIDS;
            exit_code = (nm_send_payload(payload) < 0);
            break;
        }

        case ACTION_VERSION: {
            payload->cmd = NM_CMD_GET_VERSION;
            if (nm_send_payload(payload) == 0) {
                payload->buffer[payload->data_size++] = '\n';
                print_strn(payload->buffer, payload->data_size);
                exit_code = 0;
            }
            break;
        }

        case ACTION_RULE_LIST:
        case ACTION_UID_LIST: {
            struct nm_output output = { workspace.cwd, 0 };
            int is_uids = (args.action == ACTION_UID_LIST);
            if (is_uids) args.is_json = 1;
            if (args.is_json) list_print_literal(&output, "[\n");
            int offset = 2;

            payload->cmd = is_uids ? NM_CMD_GET_UIDS : NM_CMD_GET_LIST;
            payload->arg1 = 0;
            while (1) {
                exit_code = (nm_send_payload(payload) < 0);
                if (exit_code || payload->data_size == 0) break;

                char *data = payload->buffer;
                int pos = 0;
                while (pos < payload->data_size) {
                    if (is_uids) {
                        unsigned int uid = *(unsigned int *)(data + pos);
                        pos += 4;
                        if (offset == 0) list_print_literal(&output, ",\n");
                        list_print_literal(&output, "  "); list_print_uint(&output, uid);
                        offset = 0;
                    } else {
                        struct nm_rule_hdr *h = (void *)(data + pos);
                        unsigned int flags = h->flags, uid = h->uid;
                        unsigned short vlen = h->v_len, rlen = h->r_len;
                        pos += sizeof(*h);

                        char *v = data + pos; pos += vlen;
                        char *r = data + pos; pos += rlen;
                        int is_white_flag  = (flags & 4);
                        int is_virtual_dir = (flags & 2);

                        if (args.is_json) {
                            list_print_literal_offset(&output, ",\n  {\n    \"virtual\": \"", offset); offset = 0;
                            list_print_json_strn(&output, v, vlen);
                            if (is_white_flag) list_print_literal(&output, "\",\n    \"whiteout\": true");
                            else if (is_virtual_dir) list_print_literal(&output, "\",\n    \"virtual_dir\": true");
                            else { list_print_literal(&output, "\",\n    \"real\": \""); list_print_json_strn(&output, r, rlen); list_print_literal(&output, "\""); }
                            if (uid != 0) { list_print_literal(&output, ",\n    \"uid\": "); list_print_uint(&output, uid); }
                            list_print_literal(&output, "\n  }");
                        } else {
                            list_print_strn(&output, v, vlen);
                            if (is_white_flag) list_print_literal(&output, " (whiteout)");
                            else if (is_virtual_dir) list_print_literal(&output, " (virtual dir)");
                            else { list_print_literal(&output, " -> "); list_print_strn(&output, r, rlen); }
                            if (uid != 0) { list_print_literal(&output, " [UID: "); list_print_uint(&output, uid); list_print_literal(&output, "]"); }
                            list_print_literal(&output, "\n");
                        }
                    }
                }
            }

            if (args.is_json) list_print_literal(&output, "\n]\n");
            list_flush(&output);
            break;
        }

        case ACTION_NONE:
        default: {
            print_literal(
                "NoMount CLI (nm) - Usage Guide:\n\n"
                "Rule Commands:\n"
                "  nm rule add <virtual_path> <real_path>  Add a path redirection\n"
                "  nm rule add --whiteout <virtual_path>   Hide/whiteout a path\n"
                "  nm rule del <virtual_path>              Delete a specific rule\n"
                "  nm rule list [--json]                   List all active rules\n"
                "  nm rule clear                           Clear all rules\n\n"
                "UID Commands (Exceptions):\n"
                "  nm uid add <uid>                        Add app UID to exception list\n"
                "  nm uid del <uid>                        Remove app UID from exceptions\n"
                "  nm uid list                             List all UID exceptions\n"
                "  nm uid clear                            Clear all UID exceptions\n"
                "  nm uid block_isolated [on/1 | off/0]    Toggle isolated process blocking\n\n"
                "General Commands:\n"
                "  nm clear all                            Clear all rules and UIDs\n"
                "  nm version, v, -v                       Show driver version\n"
            );
            exit_code = 1;
            break;
        }
    }

do_exit:
    sys1(SYS_EXIT, exit_code);
    __builtin_unreachable();
}
