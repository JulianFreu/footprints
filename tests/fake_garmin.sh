#!/bin/sh
# A stand-in for garmin_sync.py, so tests/test_garmin.c can exercise the
# spawning, the line protocol and the exit codes without a Garmin account.
#
# src/garmin.c builds the same argument list either way, so what arrives here is
# $1 = login|sync, $2 = session directory, $3 = output directory. The test
# chooses a behaviour through $3, which is the one argument it controls.

case "$1" in
login)
    read -r email
    read -r password
    read -r mfa
    if [ -z "$email" ] || [ -z "$password" ]; then
        echo "error an email address and a password are needed"
        exit 1
    fi
    # Stands in for Garmin asking for a code: the first attempt has none to
    # give, the retry does.
    if [ "$password" = "needs-mfa" ] && [ -z "$mfa" ]; then
        echo "mfa-required"
        exit 2
    fi
    echo "signed in as $email"
    exit 0
    ;;
sync)
    case "$3" in
    *SLOW*)
        # Long enough that the test's cancel lands mid-import rather than after
        # it. Killed by SIGTERM, so it never gets to the line below.
        echo "total 1000"
        sleep 30
        echo "imported 1000"
        exit 0
        ;;
    *NOAUTH*)
        echo "error not logged in: no token files found"
        exit 3
        ;;
    esac
    echo "total 3"
    echo "progress 1"
    echo "progress 2"
    echo "error activity 99: 404 not found"
    echo "progress 3"
    echo "imported 2"
    exit 0
    ;;
esac

echo "error unknown command: $1"
exit 1
