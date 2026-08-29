# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Neil Rackett
#
# Find a TOS image, fetching EmuTOS once if there is not one already.
# Sourced, not run: it sets $TOS for the caller.
#
# The ROM is not committed. EmuTOS is GPLv2, so shipping the binary
# would oblige this repo to offer the corresponding source to anyone
# who receives it, indefinitely, which is a lot of obligation for a
# 256K convenience file. It would also sit in git history forever and
# go stale at the next EmuTOS release. Downloading it once into an
# ignored directory costs a few seconds and none of that.
#
# Order: $TOS if you set it, then the cache, then the network. $TOS
# stays deliberately overridable because EmuTOS always reports itself
# as TOS 2.06, so anything that depends on a real TOS version has to be
# pointed at a real ROM.

EMUTOS_VERSION=1.4
EMUTOS_ROM=etos256us.img
EMUTOS_SHA256=f1fe68360db345551231791edc6a7769e825edadf3632f59b10f3b2100ed75b3
EMUTOS_URL="https://sourceforge.net/projects/emutos/files/emutos/${EMUTOS_VERSION}/emutos-256k-${EMUTOS_VERSION}.zip/download"

TOS_CACHE=${TOS_CACHE:-build/tos}

tos_checksum_ok()
{
    [ -f "$1" ] || return 1
    command -v shasum >/dev/null || return 0   # cannot check, assume good
    [ "$(shasum -a 256 "$1" | cut -d' ' -f1)" = "$EMUTOS_SHA256" ]
}

tos_fetch()
{
    # No `local`: this file is sourced by the Makefile under /bin/sh,
    # which is dash on many systems and has no such keyword.
    zip="$TOS_CACHE/emutos.zip"

    mkdir -p "$TOS_CACHE" || return 1

    echo "fetching EmuTOS ${EMUTOS_VERSION} (once) into ${TOS_CACHE}/..." >&2

    if ! curl -fsSL --retry 2 -o "$zip" "$EMUTOS_URL"; then
        echo "download failed" >&2
        rm -f "$zip"
        return 1
    fi

    # Just the US ROM: the archive carries twenty language variants and
    # the docs, none of which the tests use.
    if ! unzip -o -j -q "$zip" "*/${EMUTOS_ROM}" -d "$TOS_CACHE"; then
        echo "could not extract ${EMUTOS_ROM}" >&2
        rm -f "$zip"
        return 1
    fi

    rm -f "$zip"

    if ! tos_checksum_ok "$TOS_CACHE/$EMUTOS_ROM"; then
        echo "checksum mismatch on ${EMUTOS_ROM}; refusing it" >&2
        rm -f "$TOS_CACHE/$EMUTOS_ROM"
        return 1
    fi

    return 0
}

# Sets TOS, or returns non-zero with a message already printed.
tos_find()
{
    if [ -n "${TOS:-}" ]; then
        if [ -f "$TOS" ]; then
            return 0
        fi
        echo "\$TOS is set to '$TOS' but there is no file there" >&2
        return 1
    fi

    if [ -f "$TOS_CACHE/$EMUTOS_ROM" ]; then
        TOS=$TOS_CACHE/$EMUTOS_ROM
        return 0
    fi

    if tos_fetch; then
        TOS=$TOS_CACHE/$EMUTOS_ROM
        return 0
    fi

    echo "" >&2
    echo "No TOS image, and EmuTOS could not be fetched." >&2
    echo "Either connect to the network and try again, or point \$TOS" >&2
    echo "at a ROM you already have:" >&2
    echo "" >&2
    echo "    TOS=/path/to/tos.img make test" >&2
    echo "" >&2
    echo "EmuTOS downloads: https://emutos.sourceforge.io/" >&2
    return 1
}
