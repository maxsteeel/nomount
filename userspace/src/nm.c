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

    if (argc < 2) {
        print_str("nm <command>\n");
        goto do_exit;
    }

    struct nm_ipc_payload *ipc = (void *)sys6(SYS_MMAP, 0, 4096, 3, 0x22, -1, 0);
    if ((long)ipc < 0) { exit_code = 2; goto do_exit; }

    char cmd = argv[1][0];
    unsigned int target_uid = 0;
    const char *p_args[64];
    int p_count = 0;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--uid") == 0 && i + 1 < argc) {
            const char *s = argv[++i];
            while (*s) target_uid = (target_uid << 3) + (target_uid << 1) + (*s++ - '0');
        } else if (p_count < 64) {
            p_args[p_count++] = argv[i];
        }
    }

    if (cmd == 'a' || cmd == 'd' || cmd == 'w') {
        int step = 1 + (cmd == 'a');
        if (p_count < step) { exit_code = 0; goto cleanup; }

        char *cwd_buf = (char *)sp - 12288;
        const char *cwd = (sys3(SYS_GETCWD, (long)cwd_buf, PATH_MAX, 0) > 0) ? cwd_buf : "/";
        int target_cmd = (cmd == 'd') ? NM_CMD_DEL_RULE : NM_CMD_ADD_RULE_BATCH;
        exit_code = 0;
        ipc->cmd = target_cmd;
        ipc->data_size = 0;
        char *cursor = ipc->buffer;

        for (int i = 0; i + step - 1 < p_count; i += step) {
            char *v_resolved = (char *)sp - 8196;
            char *v_end = resolve_path(v_resolved, cwd, p_args[i]);
            int v_len = v_end - v_resolved;
            if (!v_len) { exit_code = 3; continue; }

            int r_len = 0;
            char *r_resolved = (char *)sp - 4096;
            if (cmd == 'a') {
                char *r_end = resolve_path(r_resolved, cwd, p_args[i+1]);
                r_len = r_end - r_resolved;
                if (!r_len) { exit_code = 3; continue; }
            }

            int header_size = (target_cmd == NM_CMD_ADD_RULE_BATCH) ? 12 : 6;
            if ((cursor - ipc->buffer) + header_size + v_len + r_len > 3900) {
                ipc->data_size = cursor - ipc->buffer;
                exit_code |= (nm_trigger_ipc(ipc) < 0);
                cursor = ipc->buffer;
                ipc->cmd = target_cmd;
            }

            if (target_cmd == NM_CMD_ADD_RULE_BATCH) {
                *(unsigned int*)cursor = (cmd == 'w') ? 4 : 0;
                *(unsigned int*)(cursor + 4) = target_uid;
                *(unsigned short*)(cursor + 8) = v_len;
                *(unsigned short*)(cursor + 10) = r_len;
                memcpy(cursor + 12, v_resolved, v_len);
                if (r_len > 0) memcpy(cursor + 12 + v_len, r_resolved, r_len);
                cursor += 12 + v_len + r_len;
            } else { /* DEL_RULE */
                *(unsigned int*)cursor = target_uid;
                *(unsigned short*)(cursor + 4) = v_len;
                memcpy(cursor + 6, v_resolved, v_len);
                cursor += 6 + v_len;
            }
        }

        if (cursor > ipc->buffer) {
            ipc->data_size = cursor - ipc->buffer;
            exit_code |= (nm_trigger_ipc(ipc) < 0);
        }
        goto cleanup;

    } else if (cmd == 'b' || cmd == 'u') {
        if (p_count < 1) goto cleanup;
        unsigned int uid = 0; const char *s = p_args[0];
        while (*s) uid = (uid << 3) + (uid << 1) + (*s++ - '0');
        ipc->cmd = (cmd == 'b') ? NM_CMD_ADD_UID : NM_CMD_DEL_UID;
        ipc->target_uid = uid;
        exit_code = (nm_trigger_ipc(ipc) < 0);
        goto cleanup;

    } else if (cmd == 'c') {
        ipc->cmd = NM_CMD_CLEAR_ALL;
        exit_code = (nm_trigger_ipc(ipc) < 0);
        goto cleanup;

    } else if (cmd == 'v') {
        ipc->cmd = NM_CMD_GET_VERSION;
        if (nm_trigger_ipc(ipc) == 0) {
            unsigned int v = ipc->arg1; 
            char v_str[4] = {0};
            unsigned char tens = ((v << 7) + (v << 6) + (v << 3) + (v << 2) + v) >> 11;
            v = v - ((tens << 3) + (tens << 1));
            v_str[0] = tens + '0'; v_str[1] = v + '0'; v_str[2] = '\n';
            print_str(v_str);
            exit_code = 0;
        }
        goto cleanup;

    } else if (cmd == 'l') {
        int is_json = 0, is_uids = 0;
        for (int i = 0; i < p_count; i++) {
            if (p_args[i][0] == 'j') is_json = 1;
            if (p_args[i][0] == 'u') is_uids = 1;
        }
        if (is_uids) is_json = 1;
        if (is_json) print_str("[\n");
        int offset = 2;

        ipc->cmd = is_uids ? NM_CMD_GET_UIDS : NM_CMD_GET_LIST;
        ipc->arg1 = 0;
        while (1) {
            if (nm_trigger_ipc(ipc) < 0) break;
            if (ipc->data_size == 0) break;

            char *data = ipc->buffer;
            int pos = 0;
            while (pos < ipc->data_size) {
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
                    int is_whiteout    = (flags & 4);
                    int is_virtual_dir = (flags & 2); 

                    if (is_json) {
                        print_str((const char *)",\n  {\n    \"virtual\": \"" + offset); offset = 0;
                        print_strn(v, vlen);
                        if (is_whiteout) print_str("\",\n    \"whiteout\": true");
                        else if (is_virtual_dir) print_str("\",\n    \"virtual_dir\": true");
                        else { print_str("\",\n    \"real\": \""); print_strn(r, rlen); print_str("\""); }
                        if (uid != 0) { print_str(",\n    \"uid\": "); print_uint(uid); }
                        print_str("\n  }");
                    } else {
                        print_strn(v, vlen);
                        if (is_whiteout) print_str(" (whiteout)");
                        else if (is_virtual_dir) print_str(" (virtual dir)");
                        else { print_str(" -> "); print_strn(r, rlen); }
                        if (uid != 0) { print_str(" [UID: "); print_uint(uid); print_str("]"); }
                        print_str("\n");
                    }
                }
            }
            ipc->cmd = is_uids ? NM_CMD_GET_UIDS : NM_CMD_GET_LIST;
        }

        if (is_json) print_str("\n]\n");
        exit_code = 0;
    }

cleanup:
    sys3(SYS_MUNMAP, (long)ipc, 4096, 0);
do_exit:
    sys1(SYS_EXIT, exit_code);
    __builtin_unreachable();
}
