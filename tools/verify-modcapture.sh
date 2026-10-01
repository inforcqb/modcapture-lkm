#!/system/bin/sh
#
# modcapture - on-device acceptance test.
#
#   adb push modcapture.ko-android13-5.15/modcapture.ko tools/verify-modcapture.sh /data/local/tmp/
#   adb shell su -c "sh /data/local/tmp/verify-modcapture.sh"
#
# What it proves, in order:
#   0. nothing is left loaded from a previous run
#   1. the kprobe really armed on load_module (the module refuses to load if it cannot)
#   2. a real module load actually produces a dump
#   3. that dump is BYTE-IDENTICAL to the file that was loaded (md5)
#   4. the file name is the timestamp form that was asked for
#   5. modcapture.log names the module that was captured
#   6. nothing in the kernel log complains, and rmmod leaves nothing behind
#
# The victim load is deliberately a second `insmod` of modcapture.ko itself: the
# kernel rejects it with -EEXIST, so no driver state on the device changes -
# while still going through load_module(), which is all the capture needs.
# (The capture happens BEFORE validation, which is the point.)
#
# The kernel log is NOT cleared; only the lines that appeared during this run
# are examined, so a pre-existing warning cannot be attributed to this module.
#
# Overridable: DIR= KO= VICTIM=
set -u

DIR=${DIR:-/data/local/tmp}
KO=${KO:-$DIR/modcapture.ko}
VICTIM=${VICTIM:-$KO}
MODNAME=modcapture
TMP=${TMP:-$DIR/.modcapture-test}

pass=0
fail=0
ok()  { echo "  [ok]   $*"; pass=$((pass + 1)); }
bad() { echo "  [FAIL] $*"; fail=$((fail + 1)); }
info() { echo "  ...    $*"; }

dmesg_snapshot() { dmesg > "$TMP/$1" 2>/dev/null || : > "$TMP/$1"; }
# Only the lines added since the previous snapshot.  If the ring buffer wrapped
# (fewer lines now than before) the whole buffer is scanned and that is said out
# loud rather than silently mis-sliced.
dmesg_new() {
	n0=$(wc -l < "$TMP/dmesg.before" 2>/dev/null | tr -d ' ')
	n1=$(wc -l < "$TMP/dmesg.after" 2>/dev/null | tr -d ' ')
	n0=${n0:-0}
	n1=${n1:-0}
	if [ "$n1" -ge "$n0" ]; then
		tail -n +$((n0 + 1)) "$TMP/dmesg.after"
	else
		echo "  ...    (kernel log wrapped during the run; scanning the whole buffer)" >&2
		cat "$TMP/dmesg.after"
	fi
}

echo "== modcapture device verification =="
echo "   dir=$DIR  ko=$KO  victim=$VICTIM"

if [ ! -f "$KO" ]; then
	echo "  [FAIL] $KO not found - push the .ko built for THIS kernel first"
	exit 1
fi
if [ "$(id -u)" != "0" ]; then
	echo "  [FAIL] run as root: su -c 'sh $0'"
	exit 1
fi
if ! command -v md5sum >/dev/null 2>&1; then
	echo "  [FAIL] md5sum not available"
	exit 1
fi

rm -rf "$TMP"
mkdir -p "$TMP"

# ---------------------------------------------------------------- clean slate
echo
echo "-- 0. clean slate"
if grep -q "^$MODNAME " /proc/modules 2>/dev/null; then
	rmmod "$MODNAME" 2>/dev/null && info "removed a stale instance"
fi
if grep -q "^$MODNAME " /proc/modules 2>/dev/null; then
	bad "$MODNAME is already loaded and could not be unloaded"
	exit 1
fi
ok "no previous instance"

dmesg_snapshot dmesg.before
ls -1 "$DIR"/*.ko 2>/dev/null | sort > "$TMP/before"
info "$(grep -c . "$TMP/before" 2>/dev/null || echo 0) .ko file(s) in $DIR before"

# ---------------------------------------------------------------- 1. load
echo
echo "-- 1. load modcapture and check the probe armed"
if ! insmod "$KO" 2>"$TMP/insmod.err"; then
	bad "insmod $KO failed: $(cat "$TMP/insmod.err")"
	echo "       (a refusal here is by design when load_module is absent - see dmesg)"
	dmesg 2>/dev/null | tail -20 | sed 's/^/         /'
	exit 1
fi
ok "insmod"

if grep -q "^$MODNAME " /proc/modules 2>/dev/null; then
	ok "present in /proc/modules"
else
	bad "not in /proc/modules"
fi

dmesg_snapshot dmesg.after
arm=$(dmesg_new | grep "$MODNAME:" | tail -1)
case "$arm" in
*"armed on load_module"*) ok "kernel log: ${arm#*] }" ;;
*) bad "no 'armed on load_module' line (last: ${arm:-<none>})" ;;
esac

# ---------------------------------------------------------------- 2. trigger
echo
echo "-- 2. trigger a module load"
insmod "$VICTIM" 2>"$TMP/victim.err"
info "insmod $VICTIM -> $(head -1 "$TMP/victim.err" 2>/dev/null)   (a failure here is expected)"
sleep 1
sync

ls -1 "$DIR"/*.ko 2>/dev/null | sort > "$TMP/after"
new=$(grep -vxFf "$TMP/before" "$TMP/after" 2>/dev/null)

if [ -z "$new" ]; then
	bad "no new .ko appeared in $DIR"
	dmesg_snapshot dmesg.after
	dmesg_new | grep "$MODNAME" | sed 's/^/         /'
	echo "== $pass passed, $fail failed =="
	exit 1
fi
ok "$(printf '%s\n' "$new" | grep -c .) new file(s)"

for f in $new; do
	b=$(basename "$f")

	# 3. byte-identical?
	src_md5=$(md5sum "$VICTIM" | awk '{print $1}')
	dst_md5=$(md5sum "$f" | awk '{print $1}')
	dst_sz=$(wc -c < "$f" | tr -d ' ')
	if [ "$src_md5" = "$dst_md5" ]; then
		ok "$b is byte-identical to $(basename "$VICTIM") (md5 $src_md5, $dst_sz bytes)"
	else
		bad "$b differs from $(basename "$VICTIM"): $dst_md5 vs $src_md5"
	fi

	# 4. timestamp naming
	if printf '%s' "$b" | grep -qE '^[0-9]{8}-[0-9]{6}-[0-9]{6}\.ko$'; then
		ok "$b matches YYYYMMDD-HHMMSS-uuuuuu.ko"
	else
		bad "$b does not match the expected timestamp form"
	fi
done

# ---------------------------------------------------------------- 5. index
echo
echo "-- 3. index log"
if [ -f "$DIR/modcapture.log" ]; then
	info "tail:"
	tail -5 "$DIR/modcapture.log" | sed 's/^/         /'
	if tail -5 "$DIR/modcapture.log" | grep -q "$MODNAME"; then
		ok "modcapture.log names the captured module"
	else
		bad "modcapture.log does not mention $MODNAME"
	fi
else
	bad "$DIR/modcapture.log missing (index=1 by default)"
fi

# ---------------------------------------------------------------- 6. dmesg
echo
echo "-- 4. kernel log"
dmesg_snapshot dmesg.after
newlog="$TMP/dmesg.new"
dmesg_new > "$newlog" 2>/dev/null

complaints=$(grep -iE 'BUG:|WARNING:|CFI failure|Oops|Unable to handle|scheduling while atomic|Call trace' "$newlog" 2>/dev/null)
if [ -z "$complaints" ]; then
	ok "no BUG/WARNING/CFI failure/Oops/Call trace since the test started"
else
	bad "kernel complaints since the test started:"
	printf '%s\n' "$complaints" | sed 's/^/         /'
fi
info "modcapture lines:"
grep "$MODNAME" "$newlog" 2>/dev/null | sed 's/^/         /'

# ---------------------------------------------------------------- 7. unload
echo
echo "-- 5. unload"
if rmmod "$MODNAME" 2>"$TMP/rmmod.err"; then
	ok "rmmod"
else
	bad "rmmod failed: $(cat "$TMP/rmmod.err")"
fi
if grep -q "^$MODNAME " /proc/modules 2>/dev/null; then
	bad "still in /proc/modules after rmmod"
else
	ok "gone from /proc/modules"
fi

dmesg_snapshot dmesg.after
unload=$(dmesg_new | grep "$MODNAME: unloaded" | tail -1)
case "$unload" in
*unloaded:*) ok "kernel log: ${unload#*] }" ;;
*) bad "no unload summary in the kernel log" ;;
esac

echo
echo "== $pass passed, $fail failed =="
[ "$fail" = "0" ]
