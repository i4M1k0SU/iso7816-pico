#!/usr/bin/env bash
# Hardware regression: build, flash and test the demo card with each of its three ATRs on a real PC/SC reader.
# Linux / macOS counterpart of hw_regression.ps1 (same steps and checks; see docs/tools.md).  Needs what flash.sh needs,
# python3 with pyscard (tools/sc_test.py) and pyserial (`pip install pyscard pyserial`), and pcscd on Linux.  $PYTHON
# overrides the interpreter.  Also runs from Git Bash on Windows with --port COMn (verified there; not on Linux/macOS).
#
# For each configuration (t0, t1, negotiable): build the demo with -DDEMO_ATR=<name> in build-atr-<name>/, flash it,
# wait for the USB console and check the boot self-test, run tools/sc_test.py with the matching protocol (negotiable:
# Any, then T0; retried once if it fails before any check or sees another ATR), then require every error counter of
# `status` to be 0 (and, for negotiable, the PPS to T=1 at F/D 372/4 in the log).  Finally the t0 build is flashed
# again.  Exit status 1 if anything failed.
#
#   tools/hw_regression.sh                      all three ATRs
#   tools/hw_regression.sh --only t1            one configuration (repeatable)
#   tools/hw_regression.sh --no-build           flash the existing UF2s only
#   tools/hw_regression.sh --reader ACS         reader name substring (default SCR33)
#   tools/hw_regression.sh --port /dev/ttyACM0  console port (default: the Raspberry Pi USB serial device)
set -uo pipefail

usage() {
    sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'
    exit 0
}

only=()
no_build=0
reader=SCR33
port=
while [ $# -gt 0 ]; do
    case "$1" in
        --only) [ $# -ge 2 ] || { echo "$1 needs t0, t1 or negotiable" >&2; exit 2; }; only+=("$2"); shift ;;
        --no-build|-n) no_build=1 ;;
        --reader) [ $# -ge 2 ] || { echo "$1 needs a name" >&2; exit 2; }; reader=$2; shift ;;
        --port) [ $# -ge 2 ] || { echo "$1 needs a device" >&2; exit 2; }; port=$2; shift ;;
        -h|--help) usage ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done
[ ${#only[@]} -gt 0 ] || only=(t0 t1 negotiable)
for o in "${only[@]}"; do
    case "$o" in t0|t1|negotiable) ;; *) echo "unknown configuration: $o" >&2; exit 2 ;; esac
done

here=$(cd "$(dirname "$0")" && pwd)
python=${PYTHON:-python3}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# Expected ATR (without TCK), and the runs: protocol:expected-protocol[:log line that must appear].
atr_of() {
    case "$1" in
        t0) echo '3B 08 50 69 63 6F 37 38 31 36' ;;
        t1) echo '3B 88 81 31 FE 45 50 69 63 6F 37 38 31 36' ;;
        negotiable) echo '3B 98 13 80 31 FE 45 50 69 63 6F 37 38 31 36' ;;
    esac
}
runs_of() {
    case "$1" in
        t0) printf '%s\n' 'T0:T=0' ;;
        t1) printf '%s\n' 'T1:T=1' ;;
        negotiable) printf '%s\n' 'Any:T=1:PPS: T=1 F/D 372/1 -> 372/4' 'T0:T=0' ;;
    esac
}

find_port() {
    if [ -n "$port" ]; then
        # COMn (Git Bash on Windows) is /dev/ttyS<n-1> there.
        case "$port" in
            COM*) [ -e "/dev/ttyS$(( ${port#COM} - 1 ))" ] && echo "$port" ;;
            *) [ -e "$port" ] && echo "$port" ;;
        esac
        return
    fi
    local p
    for p in /dev/serial/by-id/usb-Raspberry_Pi_*-if00 /dev/cu.usbmodem*; do
        [ -e "$p" ] && { echo "$p"; return; }
    done
}

wait_port() {
    local i p
    for i in $(seq 60); do
        p=$(find_port)
        [ -n "$p" ] && { echo "$p"; return 0; }
        sleep 0.5
    done
    return 1
}

# read_console <port> <text to send> <read seconds>: the log since the last read plus the command's answer.
read_console() {
    timeout $(( ${3%.*} + 15 )) "$python" -c '
import sys, time, serial
# Open with DTR low and raise it afterwards: the firmware writes its log only to a terminal it saw connect (DTR high),
# and pyserial flushes the input buffer while opening, which would swallow the first lines.
p = serial.Serial()
p.port, p.baudrate, p.timeout, p.dtr = sys.argv[1], 115200, 0.1, False
p.open()
time.sleep(0.1)
p.dtr = True
time.sleep(0.6)
out = p.read(p.in_waiting or 1)
if sys.argv[2]:
    p.write(sys.argv[2].encode() + b"\r")
end = time.time() + float(sys.argv[3])
while time.time() < end:
    out += p.read(p.in_waiting or 1)
p.dtr = False
p.close()
sys.stdout.write(out.decode("ascii", "replace").replace("\r", ""))
' "$1" "$2" "$3" 2>&1
}

install_config() {
    local args=(--build-dir "build-atr-$1" "-DDEMO_ATR=$1")
    [ "$no_build" = 1 ] && args+=(--no-build)
    if ! timeout 900 "$here/flash.sh" "${args[@]}" > "$tmp/flash.txt" 2>&1; then
        grep -v 'Loading into Flash' "$tmp/flash.txt" | tail -n 8 >&2
        return 1
    fi
}

# Prints the console port; fails if the console does not come back or the self-test is not clean.
test_boot() {
    local com line i
    com=$(wait_port) || { echo "console port did not come back within 30 s" >&2; return 1; }
    # The device node may still be the old one for a moment (or already exist before the firmware listens): read
    # until the boot log shows up.
    for i in $(seq 10); do
        sleep 1
        line=$(read_console "$com" '' 1.5 | grep -m 1 selftest) && break
    done
    case "$line" in
        *'selftest(loopback OK)'*'parity error: detected'*) echo "$com" ;;
        *) echo "self-test: ${line:-no selftest line in the boot log}" >&2; return 1 ;;
    esac
}

rows=()
failed=0
row_format='%-10s %-12s %-6s %-7s %-13s %-6s %8s  %s'
add_row() {
    rows+=("$(printf "$row_format" "$@")")
    [ "$3" = PASS ] || failed=$((failed + 1))
}

started=$(date +%s)
last=
for name in t0 t1 negotiable; do
    case " ${only[*]} " in *" $name "*) ;; *) continue ;; esac
    echo "=== $name: build + flash (build-atr-$name) ==="
    t=$(date +%s)
    if ! install_config "$name"; then
        add_row "$name" - FAIL '' '' '' $(( $(date +%s) - t )) 'build/flash failed'
        continue
    fi
    last=$name
    if ! com=$(test_boot); then
        add_row "$name" - FAIL '' '' '' $(( $(date +%s) - t )) 'boot check failed'
        continue
    fi
    echo "flashed in $(( $(date +%s) - t )) s; console $com"
    expect_atr=$(atr_of "$name")
    prev_rx=0; prev_tx=0
    mapfile -t runs < <(runs_of "$name")
    for run in "${runs[@]}"; do
        proto=${run%%:*}; rest=${run#*:}; expect=${rest%%:*}; logline=
        [ "$rest" != "$expect" ] && logline=${rest#*:}
        t=$(date +%s)
        note=
        probe_log=
        if [ -n "$logline" ]; then
            # The suite logs more than the console's log ring holds: look for the line after a one-APDU session.
            for attempt in 1 2; do
                "$python" "$here/sc_test.py" --protocol "$proto" --reader "$reader" --apdu '80 10 00 00 00' \
                    > "$tmp/sc.txt" 2>&1
                grep -q "^ATR *: *$expect_atr" "$tmp/sc.txt" && break
                sleep 3
            done
            probe_log=$(read_console "$com" '' 1)
        fi
        for attempt in 1 2; do
            "$python" "$here/sc_test.py" --protocol "$proto" --reader "$reader" > "$tmp/sc.txt" 2>&1
            got_atr=$(sed -n 's/^ATR *: *\([0-9A-F ]*[0-9A-F]\) *(\(.*\))$/\1/p' "$tmp/sc.txt")
            got_proto=$(sed -n 's/^ATR *: .*(\(.*\))$/\1/p' "$tmp/sc.txt")
            passes=$(grep -c '^\[PASS\]' "$tmp/sc.txt")
            fails=$(grep -c '^\[FAIL\]' "$tmp/sc.txt")
            grep -q 'all checks passed' "$tmp/sc.txt" && break
            case "$got_atr" in "$expect_atr"*) [ "$passes" -gt 0 ] && break ;; esac
            [ $attempt = 1 ] && { note='retried; '; sleep 3; }
        done
        ok=1
        grep -q 'all checks passed' "$tmp/sc.txt" || { ok=0; note+="$(grep -m 1 'FAIL\|failed' "$tmp/sc.txt"); "; }
        case "$got_atr" in "$expect_atr"*) ;; *) ok=0; note+="ATR $got_atr; " ;; esac
        [ "$got_proto" = "$expect" ] || { ok=0; note+="protocol $got_proto; "; }
        status=$(read_console "$com" status 2.5)
        chars=$(echo "$status" | sed -n 's/^rx\/tx chars: *\([0-9]*\) *\/ *\([0-9]*\).*/\1 \2/p')
        # "errors     : rx parity 0, rx overrun 0, tx echo 0, tx timeout 0" -> "0 0 0 0"
        errs=$(echo "$status" | sed -n '/^errors *: /{s/[^0-9]\{1,\}/ /g;s/^ //;p;}')
        reps=$(echo "$status" | sed -n 's/^repetition *: *\([0-9]*\).*/\1/p')
        if [ -z "$chars" ] || [ -z "$errs" ] || [ -z "$reps" ]; then
            ok=0; note+='no status output; '; total=?; chars_d=?
        else
            total=$(( reps $(printf ' + %s' $errs) ))
            [ "$total" = 0 ] || { ok=0; note+="errors $errs, repetition $reps; "; }
            set -- $chars
            chars_d="$(( $1 - prev_rx )) / $(( $2 - prev_tx ))"; prev_rx=$1; prev_tx=$2
        fi
        if [ -n "$logline" ]; then
            if printf '%s\n%s\n' "$probe_log" "$status" | grep -qF "$logline"; then
                note+="$logline"
            else
                ok=0; note+="no '$logline'"
            fi
        fi
        result=PASS; [ $ok = 1 ] || result=FAIL
        add_row "$name" "$proto -> $got_proto" $result "$passes/$(( passes + fails ))" "$chars_d" "$total" \
            $(( $(date +%s) - t )) "$note"
        echo "${rows[${#rows[@]}-1]}"
    done
done

if [ "$last" != t0 ]; then
    echo "=== restoring the default (t0) demo ==="
    if install_config t0 && test_boot > /dev/null; then
        echo "board is running the t0 demo again"
    else
        add_row t0 restore FAIL '' '' '' 0 'restore failed'
    fi
fi

echo
printf "$row_format\n" ATR Protocol Result Checks 'Chars rx/tx' Errors 'Time [s]' Note
for r in "${rows[@]}"; do echo "$r"; done
if [ "$failed" -gt 0 ] || [ ${#rows[@]} -eq 0 ]; then
    echo "hardware regression FAILED ($failed of ${#rows[@]} rows, $(( $(date +%s) - started )) s)"
    exit 1
fi
echo "hardware regression passed (${#rows[@]} runs, $(( $(date +%s) - started )) s)"
