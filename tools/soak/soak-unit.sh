#!/bin/sh
# The soak's monitor on the unit (NFR-014). POSIX sh for the device's BusyBox (1.20) ash: no Python there.
# It samples /proc/<pid> of the running app every --interval seconds into <out>/monitor.csv (the format
# tools/soak/soak-run.py reads: t_s,rss_kb,hwm_kb,threads,fds,ticks,autosave_mtime) and prints one SOAK summary line
# every --summary seconds (also appended to <out>/soak.txt): VmRSS / VmHWM, threads, open fds, and the app's last
# LOAD line (xruns, overruns, p95, max). It never signals anything but the PID it started or was given.
#
#   tools/soak/soak-unit.sh --out DIR [--interval 10] [--summary 600] [--hours H] [--data DIR] -- APP [ARGS...]
#       starts APP (its stdout + stderr → DIR/log.txt), monitors it until it exits (the soak script ends with `quit`);
#       --hours: SIGTERM to that PID after H hours if it is still running (then SIGKILL 30 s later)
#   tools/soak/soak-unit.sh --out DIR --pid PID --log FILE [...]
#       monitors an app started elsewhere (its log file given for the LOAD lines)
#
# The run on the unit (no human): generate the script on the desktop
#     tools/soak/soak-run.py script --out soak --hours 24 --seed 1 --gap-ms 1500,4500
# copy soak/script.txt and this file to the unit, start the app with input_script=<path to script.txt> (settings file
# or <PREFIX>_INPUT_SCRIPT, see app/SettingsFile.h) through this monitor, e.g.
#     <PREFIX>_INPUT_SCRIPT=/mnt/usb/soak/script.txt sh soak-unit.sh --out /mnt/usb/soak/run -- /path/to/app
# then, back on the desktop:
#     tools/soak/soak-run.py analyze --log run/log.txt --monitor run/monitor.csv --manifest soak/manifest.json --unit
# --gap-ms 1500,4500 keeps a 24 h script near 340 k steps (≈ 8 MB of text, parsed once at start-up).
set -u
out="" interval=10 summary=600 hours="" pid="" log="" data=""
while [ $# -gt 0 ]; do
  case "$1" in
    --out) out=$2; shift 2 ;;
    --interval) interval=$2; shift 2 ;;
    --summary) summary=$2; shift 2 ;;
    --hours) hours=$2; shift 2 ;;
    --pid) pid=$2; shift 2 ;;
    --log) log=$2; shift 2 ;;
    --data) data=$2; shift 2 ;;
    --) shift; break ;;
    *) echo "soak-unit: unknown argument $1" >&2; exit 2 ;;
  esac
done
[ -n "$out" ] || { echo "soak-unit: --out DIR is required" >&2; exit 2; }
mkdir -p "$out" || exit 2
started=0
if [ -z "$pid" ]; then
  [ $# -gt 0 ] || { echo "soak-unit: give -- APP [ARGS...] or --pid PID --log FILE" >&2; exit 2; }
  log="$out/log.txt"
  "$@" > "$log" 2>&1 &
  pid=$!
  started=1
fi
[ -d "/proc/$pid" ] || { echo "soak-unit: no process $pid" >&2; exit 2; }
autosave_dir=""
[ -n "$data" ] && autosave_dir="$data/System/autosave"

mon="$out/monitor.csv"
echo "t_s,rss_kb,hwm_kb,threads,fds,ticks,autosave_mtime" > "$mon"
t0=$(date +%s)
next_summary=$((t0 + summary))
deadline=""
[ -n "$hours" ] && deadline=$(awk -v t="$t0" -v h="$hours" 'BEGIN { printf "%d", t + h * 3600 }')
base_rss=""
echo "SOAK monitor pid=$pid interval=${interval}s summary=${summary}s out=$out" | tee -a "$out/soak.txt"

sample() { # one CSV row; returns 1 when the process is gone
  status="/proc/$pid/status"
  [ -r "$status" ] || return 1
  rss=0 hwm=0 thr=0
  while read -r key value rest; do
    case "$key" in
      VmRSS:) rss=$value ;;
      VmHWM:) hwm=$value ;;
      Threads:) thr=$value ;;
    esac
  done < "$status"
  fds=$(ls "/proc/$pid/fd" 2>/dev/null | wc -l)
  ticks=0
  if read -r statline < "/proc/$pid/stat" 2>/dev/null; then
    # fields after the ")" of the command name: state is field 3, utime 14, stime 15
    rest=${statline##*) }
    set -- $rest
    [ $# -ge 13 ] && ticks=$(( ${12} + ${13} ))
  fi
  am=""
  if [ -n "$autosave_dir" ] && [ -d "$autosave_dir" ]; then
    newest=$(ls -t "$autosave_dir"/*.json 2>/dev/null | head -n 1)
    [ -n "$newest" ] && am=$(stat -c %Y "$newest" 2>/dev/null || date -r "$newest" +%s 2>/dev/null || echo "")
  fi
  now=$(date +%s)
  echo "$now,$rss,$hwm,$thr,$fds,$ticks,$am" >> "$mon"
  [ -n "$base_rss" ] || { [ $((now - t0)) -ge 600 ] && base_rss=$rss; }
  return 0
}

summary_line() {
  now=$(date +%s)
  el=$((now - t0))
  load=""
  [ -n "$log" ] && [ -r "$log" ] && load=$(tail -n 4000 "$log" | grep " LOAD cycles/block" | tail -n 1 | sed 's/^.* LOAD cycles\/block //')
  d=""
  [ -n "$base_rss" ] && d=" d=$(( (rss - base_rss) / 1024 ))MB"
  printf 'SOAK t=%02d:%02d:%02d rss=%dMB%s hwm=%dMB fds=%s thr=%s %s\n' $((el / 3600)) $((el % 3600 / 60)) $((el % 60)) \
    $((rss / 1024)) "$d" $((hwm / 1024)) "$fds" "$thr" "${load:-(no LOAD line yet)}" | tee -a "$out/soak.txt"
}

while sample; do
  now=$(date +%s)
  if [ "$now" -ge "$next_summary" ]; then
    summary_line
    next_summary=$((next_summary + summary))
  fi
  if [ -n "$deadline" ] && [ "$now" -ge "$deadline" ]; then
    echo "SOAK deadline: SIGTERM to $pid" | tee -a "$out/soak.txt"
    kill -TERM "$pid" 2>/dev/null
    i=0
    while [ -d "/proc/$pid" ] && [ $i -lt 30 ]; do sleep 1; i=$((i + 1)); done
    [ -d "/proc/$pid" ] && kill -KILL "$pid" 2>/dev/null
    break
  fi
  sleep "$interval"
done
code=""
if [ "$started" = 1 ]; then
  wait "$pid"
  code=$?
fi
echo "SOAK END pid=$pid exit=${code:-unknown} samples=$(($(wc -l < "$mon") - 1)) log=$log" | tee -a "$out/soak.txt"
[ -z "$code" ] || exit "$code"
