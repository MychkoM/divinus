#!/bin/sh
# divinus keepalive (runs under setsid from S96divinus; survives SSH)
LOGF=/tmp/divinus.log
while :; do
    if ! pidof divinus >/dev/null; then
        cd /opt/divinus && ./divinus > $LOGF 2>&1
    fi
    SZ=$(wc -c < $LOGF 2>/dev/null || echo 0)
    [ "$SZ" -gt 150000 ] && tail -c 40000 $LOGF > $LOGF.r && mv $LOGF.r $LOGF
    sleep 3
done
