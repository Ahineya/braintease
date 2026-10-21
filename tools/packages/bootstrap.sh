#!/bin/sh
# This script runs inside the guest beside a target-specific pkg-engine and a
# repo.pub obtained over SSH by the publisher, never through unauthenticated HTTP.
# V7 sh applies -e even to a false external test in an if condition.
# Check mutations explicitly instead, and use its supported backtick syntax.
set -u
here=`dirname "$0"`
test -n "$here" || {
    echo 'Bootstrap requires test and a resolvable script directory' >&2
    exit 1
}
url=http://192.168.100.179:8088
if test "$#" -gt 0; then url=$1; fi
mkdir -p /etc/pkg || exit 1
if test -f /etc/pkg/repo.pub; then
    cmp "$here/repo.pub" /etc/pkg/repo.pub >/dev/null 2>&1 || {
        echo 'Pinned repository key differs; inspect it before replacing /etc/pkg/repo.pub' >&2
        exit 1
    }
else
    cp "$here/repo.pub" /etc/pkg/repo.pub || exit 1
    chmod 644 /etc/pkg/repo.pub || exit 1
fi
if test ! -f /etc/pkg/repo.conf; then echo "url=$url" > /etc/pkg/repo.conf || exit 1; fi
"$here/pkg-engine" update || exit 1
"$here/pkg-engine" --adopt-seed add pkg || exit 1
"$here/pkg-engine" --adopt-seed add test || exit 1
echo
echo 'Installed pkg. For this shell: export PATH=/usr/bin:/bin'
