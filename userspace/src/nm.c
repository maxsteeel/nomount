/*
 * nm.c - NoMount CLI Userspace Tool
 */
#include "nm.h"
#include <sys/file.h>

/* --- MAIN --- */
int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--try-lock")) {
        if (!flock(STDIN_FILENO, LOCK_EX | LOCK_NB)) return 0;
        return errno == EWOULDBLOCK ? 1 : 2;
    }

    int exit_code = 1;
    struct nm_workspace workspace;
    struct nm_payload *payload = &workspace.payload;
    struct nm_args args;
    parse_arguments(argc, argv, &args);
    payload->magic = NOMOUNT_MAGIC_SIG;

    if (args.action == ACTION_MODULE_LIST) return cmd_module_list(&workspace, &args);
    if (args.action == ACTION_MODULE_LOAD || args.action == ACTION_MODULE_UNLOAD)
        return cmd_module_load_or_unload(&workspace, &args, args.action == ACTION_MODULE_LOAD);

    int p_count = 0, needs_cwd = 0;
    if (argc >= 2) {
        for (int i = args.data_start_idx; i < argc; i++) {
            if (argv[i][0] == '/') { argv[p_count++] = argv[i]; }
            else if (strcmp(argv[i], "--uid") == 0) {
                unsigned int uid = 0;
                if (i + 1 >= argc || nm_parse_uid(argv[++i], &uid)) {
                    print_literal("nm: invalid --uid value\n");
                    goto do_exit;
                }
                args.target_uid = uid;
                args.target_uid_end = uid;
            }
            else if (strcmp(argv[i], "--uid-range") == 0) {
                const char *s = i + 1 < argc ? argv[++i] : "";
                if (strcmp(s, "all") == 0) {
                    args.target_uid = 0;
                    args.target_uid_end = 0xFFFFFFFFU;
                } else if (parse_uid_range(s, &args.target_uid, &args.target_uid_end)) {
                    print_literal("nm: invalid --uid-range value\n");
                    goto do_exit;
                }
            }
            else if (strcmp(argv[i], "--uid-excluded") == 0) {
                if (i + 1 >= argc || parse_uid_range(argv[++i], &args.deny_start, &args.deny_end)) {
                    print_literal("nm: invalid --uid-excluded value\n");
                    goto do_exit;
                }
            }
            else if (strcmp(argv[i], "--json") == 0 || strcmp(argv[i], "json") == 0) { args.is_json = 1; }
            else if (strcmp(argv[i], "--whiteout") == 0) { args.is_whiteout = 1; }
            else if (strcmp(argv[i], "--opaque") == 0) { args.is_opaque = 1; }
            else { needs_cwd = 1; argv[p_count++] = argv[i]; }
        }
    }

    switch (args.action) {
        case ACTION_RULE_ADD:
        case ACTION_RULE_DEL: {
            if (args.action == ACTION_RULE_ADD && args.is_whiteout && args.is_opaque) {
                print_literal("nm: --whiteout and --opaque flags are mutually exclusive\n");
                goto do_exit;
            }
            if (args.deny_start || args.deny_end) {
                if (!args.deny_start || !args.deny_end) {
                    print_literal("nm: --uid-excluded requires a valid range\n");
                    goto do_exit;
                }
                if (args.deny_start < args.target_uid || args.deny_end > args.target_uid_end) {
                    print_literal("nm: --uid-excluded must be inside --uid / --uid-range\n");
                    goto do_exit;
                }
            }
            int step = (args.action == ACTION_RULE_ADD && !args.is_whiteout) ? 2 : 1;
            if (!p_count) { exit_code = 0; goto do_exit; }
            if (p_count % step) { print_literal("nm: paths must be supplied in <vpath> <rpath> pairs\n"); goto do_exit; }

            const char *cwd = "/";
            if (needs_cwd && !(cwd = getcwd(workspace.cwd, PATH_MAX)))
                return nm_error("getcwd", -errno);
            int target_cmd = (args.action == ACTION_RULE_DEL) ? NM_CMD_DEL_RULE : NM_CMD_ADD_RULE;

            exit_code = 0;
            payload->cmd = target_cmd;
            payload->arg1 = 0;
            payload->data_size = 0;
            unsigned flags = (args.is_whiteout ? NM_FLAG_WHITEOUT : 0) | (args.is_opaque ? NM_FLAG_OPAQUE : 0);
            for (int i = 0; i + step - 1 < p_count; i += step) {
                int rc = nm_queue_rule(payload, &args, flags, cwd, argv[i], step == 2 ? argv[i + 1] : NULL);
                exit_code |= rc == -E2BIG ? 3 : rc < 0;
            }
            exit_code |= nm_flush_rules(payload) < 0;
            break;
        }

        case ACTION_UID_ADD:
        case ACTION_UID_DEL: {
            if (p_count < 1) goto do_exit;
            for (int i = 0; i < p_count; i++) {
                unsigned int uid = 0;
                if (nm_parse_uid(argv[i], &uid)) {
                    print_literal("nm: invalid uid value\n");
                    exit_code = 1;
                    goto do_exit;
                }
            }
            payload->cmd = (args.action == ACTION_UID_ADD) ? NM_CMD_ADD_UID : NM_CMD_DEL_UID;
            exit_code = 0;
            for (int i = 0; i < p_count; i++) {
                unsigned int uid = 0;
                nm_parse_uid(argv[i], &uid);
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
            struct nm_output output = { workspace.cwd, 0, STDOUT_FILENO };
            int is_uids = (args.action == ACTION_UID_LIST);
            if (is_uids) args.is_json = 1;
            if (args.is_json) list_print_literal(&output, "[\n");
            int offset = 2;

            payload->cmd = is_uids ? NM_CMD_GET_UIDS : NM_CMD_GET_LIST;
            payload->arg1 = 0;
            do {
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
                        unsigned int flags = h->flags, uid = h->uid, uid_end = h->uid_end;
                        unsigned int deny_start = h->deny_start, deny_end = h->deny_end;
                        unsigned short vlen = h->v_len, rlen = h->r_len;
                        pos += sizeof(*h);

                        char *v = data + pos; pos += vlen;
                        char *r = data + pos; pos += rlen;
                        int is_white_flag  = (flags & NM_FLAG_WHITEOUT);
                        int is_virtual_dir = (flags & NM_FLAG_VIRTUAL_DIR);
                        int is_opaque      = (flags & NM_FLAG_OPAQUE);

                        if (args.is_json) {
                            list_print_literal_offset(&output, ",\n  {\n    \"virtual\": \"", offset); offset = 0;
                            list_print_json_strn(&output, v, vlen);
                            if (is_white_flag) list_print_literal(&output, "\",\n    \"whiteout\": true");
                            else if (is_virtual_dir) list_print_literal(&output, "\",\n    \"virtual_dir\": true");
                            else {
                                list_print_literal(&output, "\",\n    \"real\": \""); list_print_json_strn(&output, r, rlen); list_print_literal(&output, "\"");
                                if (is_opaque) list_print_literal(&output, ",\n    \"opaque\": true");
                            }
                            if (uid || uid_end || deny_start || deny_end) {
                                if (uid == 0 && uid_end == 0) list_print_literal(&output, ",\n    \"default\": true");
                                else {
                                    list_print_literal(&output, ",\n    \"uid\": "); list_print_uint(&output, uid);
                                    list_print_literal(&output, ",\n    \"uid_end\": "); list_print_uint(&output, uid_end);
                                }
                                if (deny_start) {
                                    list_print_literal(&output, ",\n    \"deny_start\": "); list_print_uint(&output, deny_start);
                                    list_print_literal(&output, ",\n    \"deny_end\": "); list_print_uint(&output, deny_end);
                                }
                            }
                            list_print_literal(&output, "\n  }");
                        } else {
                            list_print_strn(&output, v, vlen);
                            if (is_white_flag) list_print_literal(&output, " (whiteout)");
                            else if (is_virtual_dir) list_print_literal(&output, " (virtual dir)");
                            else {
                                list_print_literal(&output, " -> "); list_print_strn(&output, r, rlen);
                                if (is_opaque) list_print_literal(&output, " (opaque)");
                            }
                            if (uid || uid_end) {
                                list_print_literal(&output, " [UID: ");
                                if (uid == uid_end) list_print_uint(&output, uid);
                                else { list_print_uint(&output, uid); list_print_literal(&output, "-"); list_print_uint(&output, uid_end); }
                                if (deny_start) {
                                    list_print_literal(&output, " excl ");
                                    if (deny_start == deny_end) list_print_uint(&output, deny_start);
                                    else { list_print_uint(&output, deny_start); list_print_literal(&output, "-"); list_print_uint(&output, deny_end); }
                                }
                                list_print_literal(&output, "]");
                            }
                            list_print_literal(&output, "\n");
                        }
                    }
                }
            } while (payload->arg1 != ~0U);

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
                "  nm rule add --opaque <virtual_dir> <real_dir>\n"
                "                                          Add an opaque dir redirect\n"
                "  nm rule add [--uid <u> | --uid-range <a>-<b> | --uid-range all]\n"
                "              [--uid-excluded <c>-<d>] <vpath> <rpath>\n"
                "                                          Scope the rule to a UID set\n"
                "                                          (default: applies to all UIDs)\n"
                "  nm rule del <virtual_path>              Delete a specific rule\n"
                "  nm rule del [same UID options] <vpath>  Delete a rule matching the UID scope\n"
                "  nm rule list [--json]                   List all active rules\n"
                "  nm rule clear                           Clear all rules\n\n"
                "Module Commands:\n"
                "  nm module load <mod_path> [--targets=\"system vendor ...\"]\n"
                "                                          Inject every partition of a module\n"
                "                                          (default: all known partitions)\n"
                "  nm module unload <mod_path> [same --targets]\n"
                "                                          Remove all rules belonging to a module\n"
                "  nm module list [--json] [--only=<a,b,...>]\n"
                "                                          List modules with their injected file count\n\n"
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
            break;
        }
    }

do_exit:
    return exit_code;
}
