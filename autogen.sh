#!/bin/sh
set -e

if test -n "$AUTOMAKE"; then
    : # don't override an explicit user request
elif automake-1.11 --version >/dev/null 2>/dev/null && \
     aclocal-1.11 --version >/dev/null 2>/dev/null; then
    # If we have automake-1.11, use it. This is the oldest version (=> least
    # likely to introduce undeclared dependencies) that will give us
    # --enable-silent-rules support.
    AUTOMAKE=automake-1.11
    export AUTOMAKE
    ACLOCAL=aclocal-1.11
    export ACLOCAL
fi

autoreconf -i -f

#Check if submodules should be enabled
enable_submodules=true
for arg in $*; do
    case $arg in
        --disable-submodules)
            enable_submodules=false
            ;;
        *)
            ;;
    esac
done

if test $enable_submodules = true; then
    # Fetch submodules if needed
    if test ! -f lib/ext/wocky/autogen.sh; then
        echo "+ Setting up submodules"
        git submodule init
    fi
    git submodule update

    # launch Wocky's autogen.sh
    cd lib/ext/wocky
    sh autogen.sh --no-configure
    cd ../../..
fi

# Honor NOCONFIGURE for compatibility with gnome-autogen.sh
if test x"$NOCONFIGURE" = x; then
    run_configure=true
    for arg in $*; do
        case $arg in
            --no-configure)
                run_configure=false
                ;;
            *)
                ;;
        esac
    done
else
    run_configure=false
fi

if test $run_configure = true; then
    ./configure "$@"
fi
