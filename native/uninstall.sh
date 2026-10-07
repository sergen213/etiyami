#!/bin/sh
# The compound command is parsed before the helper can unlink this script.
{
    script=$0
    case $script in /*) ;; *) script=./$script ;; esac
    root=$(CDPATH= cd -P "$(dirname "$script")" && pwd -P)
    result=$?
    if [ "$result" -eq 0 ]; then
        "$root/.eti-yami-uninstall/yami-remove" --root "$root"
        result=$?
    else
        printf '%s\n' 'Cannot locate the ETI Yami installation.' >&2
    fi
    if [ -t 0 ]; then
        printf '\n%s' 'Press Enter to close this window: '
        IFS= read -r acknowledgement || :
    fi
    exit "$result"
}
