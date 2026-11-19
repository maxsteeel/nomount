#!/bin/sh
set -eu

GKI_ROOT=$(pwd)
REPO_URL="https://github.com/maxsteeel/nomount"
REPO_DIR="$GKI_ROOT/NoMount"

display_usage() {
    echo "Usage: $0 [--cleanup | <commit-or-tag>]"
    echo "  --cleanup:             Cleans up previous modifications made by the script (doesn't revert in-kernel patches)."
    echo "  <commit-or-tag>:       Sets up or updates NoMount to specified tag or commit."
    echo "  -h, --help:            Displays this usage information."
    echo "  (no args):             Sets up or updates NoMount environment to the latest tagged version."
}

initialize_variables() {
    if [ -d "$GKI_ROOT/fs" ]; then
         FS_DIR="$GKI_ROOT/fs"
    elif [ -d "$GKI_ROOT/common/fs" ]; then
         FS_DIR="$GKI_ROOT/common/fs"
    else
         echo '[ERROR] "fs/" directory not found. Are you at the root of the kernel tree?'
         exit 127
    fi

    FS_MAKEFILE="$FS_DIR/Makefile"
    FS_KCONFIG="$FS_DIR/Kconfig"
}

perform_cleanup() {
    echo "[+] Cleaning up NoMount..."

    if [ -L "$FS_DIR/nomount" ]; then
        rm "$FS_DIR/nomount"
        echo "[-] Symlink removed."
    fi

    if grep -q "nomount" "$FS_MAKEFILE"; then
        sed -i '/nomount/d' "$FS_MAKEFILE"
        echo "[-] Makefile reverted."
    fi

    if grep -q "fs/nomount/Kconfig" "$FS_KCONFIG"; then
        sed -i '/fs\/nomount\/Kconfig/d' "$FS_KCONFIG"
        echo "[-] Kconfig reverted."
    fi

    if [ -d "$REPO_DIR" ]; then
        rm -rf "$REPO_DIR"
        echo "[-] NoMount directory deleted."
    fi

    echo "[+] Cleanup complete."
}

setup_nomount() {
    echo "[+] Setting up NoMount..."

    if [ ! -d "$REPO_DIR" ]; then
        git clone "$REPO_URL" "$REPO_DIR"
        echo "[+] Repository cloned."
    fi

    cd "$REPO_DIR"

    git reset --hard HEAD --quiet
    git fetch --all --tags --quiet
    if [ -z "${1-}" ]; then
        latest_tag=$(git tag --list 'v*' --sort=-v:refname | head -n 1)
        if [ -n "$latest_tag" ]; then
            git checkout "$latest_tag" --quiet
            echo "[-] Checked out latest tag: $latest_tag"
        else
            git checkout dev --quiet 2>/dev/null || git checkout master --quiet 2>/dev/null
            git pull --quiet
            echo "[-] No version tags found; checked out and updated default branch."
        fi
    else
        git checkout "$1" --quiet
        echo "[-] Checked out specific target: $1"
    fi

    cd "$FS_DIR"
    ln -sfn "$(realpath --relative-to="$FS_DIR" "$REPO_DIR/kernel/src")" "nomount"
    echo "[+] Symlink created (fs/nomount -> kernel/src)."

    if ! grep -q "nomount" "$FS_MAKEFILE"; then
        printf "\nobj-\$(CONFIG_NOMOUNT) += nomount/\n" >> "$FS_MAKEFILE"
        echo "[+] Modified fs/Makefile."
    fi

    if grep -q 'source "fs/nomount/Kconfig"' "$FS_KCONFIG"; then
        echo "[-] Kconfig already modified."
    else
        awk '
            /^endmenu/ { last_match = NR }
            { lines[NR] = $0 }
            END {
                for (i = 1; i <= NR; i++) {
                    if (i == last_match) print "source \"fs/nomount/Kconfig\""
                    print lines[i]
                }
            }
        ' "$FS_KCONFIG" > "$FS_KCONFIG.tmp" && mv "$FS_KCONFIG.tmp" "$FS_KCONFIG"
        echo "[+] Modified fs/Kconfig."
    fi

    echo '[+] NoMount Setup Done!'
}

if [ "$#" -eq 0 ]; then
    initialize_variables
    setup_nomount
elif [ "$1" = "-h" ] || [ "$1" = "--help" ]; then
    display_usage
elif [ "$1" = "--cleanup" ]; then
    initialize_variables
    perform_cleanup
else
    initialize_variables
    setup_nomount "$@"
fi
