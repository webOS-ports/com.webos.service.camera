#!/bin/sh
# Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
#
# SPDX-License-Identifier: Apache-2.0
#
# On-target stress and abuse harness for com.webos.service.camera2. Exercises
# the luna API the way a hostile or crashing client would: open/close churn,
# preview cycling, concurrent clients, and malformed payloads that used to
# crash the service. The service surviving with sane error replies is the
# pass criterion.
#
#   camera-stress.sh [rounds]        (default 20)
#
# Watch the service with: journalctl -f -u com.webos.service.camera or
# ls-monitor. The script prints a summary and exits non-zero if the service
# stopped answering.

ROUNDS=${1:-20}
SVC=com.webos.service.camera2
LUNA="luna-send -n 1 -a com.webos.app.test.stress"
FAILED=0

say() { echo "[camera-stress] $*"; }

alive() {
    # A getCameraList that answers (even with an error) counts as alive.
    OUT=$($LUNA "luna://$SVC/getCameraList" '{}' 2>&1)
    case "$OUT" in
    *returnValue*) return 0 ;;
    *) return 1 ;;
    esac
}

check_alive() {
    if ! alive; then
        say "FAIL: service stopped answering after: $1"
        FAILED=1
    fi
}

first_camera_id() {
    $LUNA "luna://$SVC/getCameraList" '{}' 2>/dev/null |
        sed -n 's/.*"id":"\(camera[0-9]*\)".*/\1/p' | head -n1
}

say "starting: $ROUNDS rounds against $SVC"
if ! alive; then
    say "FAIL: service not answering at start"
    exit 1
fi

CAM=$(first_camera_id)
say "camera under test: ${CAM:-<none>}"

round=0
while [ "$round" -lt "$ROUNDS" ]; do
    round=$((round + 1))
    say "round $round/$ROUNDS"

    # --- malformed / hostile payloads (regression corpus) ------------------
    for payload in \
        '{"id":"camera1","params":{}}' \
        '{"id":"camera1","params":5}' \
        '{"id":"camera1","params":[null,42,{}]}' \
        '{"id":"camera999"}' \
        '{"id":"camera-2"}' \
        '{"id":"camera1abc"}' \
        '{"id":""}' \
        '{"handle":-1}' \
        '{"handle":0}' \
        '{"handle":2147483647}' \
        '{}' \
        '{"id":123}' \
        'null' \
        '[]'; do
        $LUNA "luna://$SVC/getProperties" "$payload" >/dev/null 2>&1
        $LUNA "luna://$SVC/getFormat" "$payload" >/dev/null 2>&1
        $LUNA "luna://$SVC/setProperties" "$payload" >/dev/null 2>&1
        $LUNA "luna://$SVC/getSolutions" "$payload" >/dev/null 2>&1
    done
    check_alive "malformed payload sweep"

    # windowId injection attempts must be rejected, not concatenated.
    for wid in \
        '_Window_Id_1",\"appId\":\"evil' \
        '_Window_Id_' \
        '_Window_Id_1 OR 1' \
        'totally_bogus'; do
        $LUNA "luna://$SVC/startPreview" \
            "{\"handle\":1,\"windowId\":\"$wid\",\"disPlayMode\":\"Display\"}" >/dev/null 2>&1
    done
    check_alive "windowId injection sweep"

    # Handle brute-force probes (must all be clean errors).
    for h in 1 7 42 555 9999 123456; do
        $LUNA "luna://$SVC/close" "{\"handle\":$h}" >/dev/null 2>&1
        $LUNA "luna://$SVC/stopPreview" "{\"handle\":$h}" >/dev/null 2>&1
    done
    check_alive "handle probe sweep"

    if [ -n "$CAM" ]; then
        # --- open/close churn ---------------------------------------------
        i=0
        while [ $i -lt 5 ]; do
            i=$((i + 1))
            H=$($LUNA "luna://$SVC/open" "{\"id\":\"$CAM\",\"mode\":\"secondary\"}" 2>/dev/null |
                sed -n 's/.*"handle":\([0-9]*\).*/\1/p' | head -n1)
            if [ -n "$H" ]; then
                $LUNA "luna://$SVC/getProperties" "{\"handle\":$H}" >/dev/null 2>&1
                $LUNA "luna://$SVC/getFormat" "{\"handle\":$H}" >/dev/null 2>&1
                $LUNA "luna://$SVC/close" "{\"handle\":$H}" >/dev/null 2>&1
            fi
        done
        check_alive "open/close churn"

        # --- double close / stale handle reuse -----------------------------
        H=$($LUNA "luna://$SVC/open" "{\"id\":\"$CAM\",\"mode\":\"secondary\"}" 2>/dev/null |
            sed -n 's/.*"handle":\([0-9]*\).*/\1/p' | head -n1)
        if [ -n "$H" ]; then
            $LUNA "luna://$SVC/close" "{\"handle\":$H}" >/dev/null 2>&1
            $LUNA "luna://$SVC/close" "{\"handle\":$H}" >/dev/null 2>&1
            $LUNA "luna://$SVC/startPreview" \
                "{\"handle\":$H,\"windowId\":\"_Window_Id_1\",\"disPlayMode\":\"Display\"}" \
                >/dev/null 2>&1
        fi
        check_alive "double close"

        # --- concurrent clients --------------------------------------------
        for n in 1 2 3; do
            (
                j=0
                while [ $j -lt 3 ]; do
                    j=$((j + 1))
                    HH=$(luna-send -n 1 -a "com.webos.app.stress$n" \
                        "luna://$SVC/open" "{\"id\":\"$CAM\",\"mode\":\"secondary\"}" 2>/dev/null |
                        sed -n 's/.*"handle":\([0-9]*\).*/\1/p' | head -n1)
                    [ -n "$HH" ] && luna-send -n 1 -a "com.webos.app.stress$n" \
                        "luna://$SVC/close" "{\"handle\":$HH}" >/dev/null 2>&1
                done
            ) &
        done
        wait
        check_alive "concurrent clients"
    fi

    [ "$FAILED" -ne 0 ] && break
done

if [ "$FAILED" -eq 0 ]; then
    say "PASS: service survived $round rounds"
    exit 0
else
    say "FAILED in round $round"
    exit 1
fi
