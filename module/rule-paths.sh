kind=$1; shift
case "$kind" in links|scan) ;; *) mod=$1; shift;; esac
for f do
    case "$kind" in links|scan)
        [ -L "$f" ] || continue
        mod=${f%%/*}
    ;; esac
    case "$f" in
        "$mod"/*) v="${f#"$mod"/}";;
        *) v="$f"; f="$mod/$f";;
    esac
    case "$v" in system/*)
        p="${v#system/}"; p="${p%%/*}"
        case "$p" in vendor|system_ext|product|odm|apex|oem|optics|prism|mi_ext|my_*)
            v="$p${v#system/$p}" ;;
        esac
    ;; esac
    if [ "$kind" = links ] || [ "$kind" = scan ]; then
        r=$(readlink -f "$f") || continue
        dir=false; [ -d "$f" ] && dir=true
        printf '@@S\t%s\t/%s\t%s\t%s\n' "${mod##*/}" "$v" "$r" "$dir"
        if [ "$kind" = scan ] && [ "$dir" = true ]; then
            find -L "$f" -mindepth 1 -exec sh "$0" links {} + || exit 1
        fi
    elif [ "$kind" = pair ]; then
        printf "/%s\0%s\0" "$v" "$f"
    elif [ "$kind" = del ]; then
        case "${f##*/}" in
            .replace) printf "/%s\0" "${v%/.replace}" ;;
            .remove)  printf "/%s\0" "${v%/.remove}"  ;;
            *)        printf "/%s\0" "$v"            ;;
        esac
    else
        if [ -d "$f" ]; then
            case "$(getfattr -n trusted.overlay.opaque "$f" 2>/dev/null)" in *"=\"y\""*) ;; *) continue;; esac
        elif [ "${f##*/}" = ".replace" ]; then
            f="${f%/.replace}"; v="${v%/.replace}"
        else
            printf "/%s\0" "${v%/.remove}"
            continue
        fi
        entries=$(find "$f" -mindepth 1 ! -name '.replace' ! -name '.remove' -print -quit 2>/dev/null) || exit 1
        if [ -n "$entries" ]; then
            if [ "$kind" = opaque ]; then printf "/%s\0%s\0" "$v" "$f"; fi
        elif [ "$kind" = path ]; then
            printf "/%s\0" "$v"
        fi
    fi
done
