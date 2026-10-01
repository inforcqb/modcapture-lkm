#!/system/bin/sh
#
# modcapture - on-device acceptance test.
#
#   adb push dist/modcapture.ko-android13-5.15/modcapture.ko /data/local/tmp/modcapture.ko
#   adb push tools/verify-modcapture.sh /data/local/tmp/
#   adb shell su -c "sh /data/local/tmp/verify-modcapture.sh"
#
# What it proves, in order:
#   0. nothing is left loaded from a previous run
#   1. the kprobe really armed on load_module (the module refuses to load if it cannot)
#   2. two real module loads produce two dumps, and BOTH are checked against what the
#      kernel was actually handed - see the note on the two load paths below
#   3. the file names are the timestamp form that was asked for
#   4. modcapture.log names the module that was captured
#   5. nothing in the kernel log complains, and rmmod leaves nothing behind
#
# THE TWO LOAD PATHS, AND WHY THE EXPECTATIONS DIFFER
# --------------------------------------------------
# (a) plain `insmod`  -> finit_module(2) -> the kernel reads the file off disk VERBATIM.
#     The dump must be byte-identical to the file.  On this device the load itself then
#     fails with "Unknown symbol filp_open (err -2)" - the device kernel does not export
#     filp_open/kernel_write to modules.  That is irrelevant here and is exactly why the
#     capture point is load_module(): the probe fires before symbol resolution.
# (b) `ksud insmod`   -> KernelSU rewrites the image BEFORE the syscall: it fills every
#     undefined symbol's st_value with the runtime kallsyms address and marks it absolute,
#     which is how it sidesteps the export table.  There is therefore no pristine copy of
#     that file inside the kernel to capture - what gets dumped is the image the kernel was
#     actually asked to load.  It must still be a valid module: same size, ELF intact,
#     `modinfo` still readable.
#
# The victim is a second load of modcapture.ko itself: the kernel answers -EEXIST, so no
# driver state on the device changes, and the load still reaches load_module.
#
# The kernel log is NOT cleared; only the lines that appeared during this run are examined.
#
# Overridable: DIR= KO= VICTIM=
set -u

DIR=${DIR:-/data/local/tmp}
KO=${KO:-$DIR/modcapture.ko}
VICTIM=${VICTIM:-$KO}
MODNAME=modcapture
TMP=${TMP:-$DIR/.modcapture-test}
LOG="$DIR/modcapture.log"

pass=0
fail=0
ok()  { echo "  [ok]   $*"; pass=$((pass + 1)); }
bad() { echo "  [FAIL] $*"; fail=$((fail + 1)); }
info() { echo "  ...    $*"; }

# Kernel log lines are sliced by their own "[ seconds.microseconds]" stamp rather than by
# line count: this device's log is chatty enough to wrap the ring buffer inside a single
# test run, and on a wrapped buffer a line offset silently points at a different line.
# T0 is the newest stamp seen before the test touched anything; everything after it is
# "this run".  Lines without a stamp (Call trace bodies) inherit the decision of the line
# above them, so a panic report is not cut in half.
dmesg_mark() {
	dmesg 2>/dev/null | awk '
		/^\[[ ]*[0-9]+\.[0-9]+\]/ { last = $0 }
		END {
			s = last
			sub(/^\[[ ]*/, "", s); sub(/\].*/, "", s)
			print s + 0
		}'
}
dmesg_since() {
	dmesg 2>/dev/null | awk -v t="$1" '
		/^\[[ ]*[0-9]+\.[0-9]+\]/ {
			s = $0; sub(/^\[[ ]*/, "", s); sub(/\].*/, "", s)
			keep = (s + 0 > t + 0)
			if (keep) print $0
			next
		}
		{ if (keep) print $0 }'
}
dmesg_new() { dmesg_since "$T0"; }
log_lines() { [ -f "$LOG" ] && wc -l < "$LOG" 2>/dev/null | tr -d ' ' || echo 0; }
# comm= of the capture that produced <basename>, taken from the module's own index log.
comm_of() {
	sed -n "s/.*comm=\([^ ]*\) .*file=$1\$/\1/p" "$LOG" 2>/dev/null | tail -1
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
for t in md5sum cmp; do
	command -v $t >/dev/null 2>&1 || { echo "  [FAIL] $t not available"; exit 1; }
done

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

T0=$(dmesg_mark)
info "kernel-log watermark: t=$T0"

# ---------------------------------------------------------------- 1. load
echo
echo "-- 1. load modcapture and check the probe armed"
if ! insmod "$KO" 2>"$TMP/insmod.err"; then
	info "plain insmod refused: $(head -1 "$TMP/insmod.err" 2>/dev/null)"
	if command -v ksud >/dev/null 2>&1; then
		info "retrying with 'ksud insmod' (this device does not export filp_open/kernel_write)"
		if ! ksud insmod "$KO" >"$TMP/insmod.err" 2>&1; then
			bad "loading $KO failed: $(head -3 "$TMP/insmod.err")"
			dmesg 2>/dev/null | tail -20 | sed 's/^/         /'
			exit 1
		fi
	else
		bad "cannot load $KO and there is no ksud to fall back to"
		dmesg 2>/dev/null | tail -20 | sed 's/^/         /'
		exit 1
	fi
fi
ok "loaded"

if grep -q "^$MODNAME " /proc/modules 2>/dev/null; then
	ok "present in /proc/modules"
else
	bad "not in /proc/modules"
fi

arm=$(dmesg_new | grep "$MODNAME:" | tail -1)
case "$arm" in
*"armed on load_module"*) ok "kernel log: ${arm#*] }" ;;
*) bad "no 'armed on load_module' line (last: ${arm:-<none>})" ;;
esac

# ---------------------------------------------------------------- 2. trigger
echo
echo "-- 2. trigger two loads of the victim"
ls -1 "$DIR"/*.ko 2>/dev/null | sort > "$TMP/before"
nlog0=$(log_lines)
info "$(grep -c . "$TMP/before" 2>/dev/null || echo 0) .ko file(s) in $DIR before"

# (a) finit_module straight off the file
insmod "$VICTIM" 2>"$TMP/v1.err"; rc1=$?
info "(a) insmod   -> rc=$rc1  $(head -1 "$TMP/v1.err" 2>/dev/null)"
sleep 1
# (b) KernelSU's loader
if command -v ksud >/dev/null 2>&1; then
	ksud insmod "$VICTIM" >"$TMP/v2.err" 2>&1; rc2=$?
	info "(b) ksud     -> rc=$rc2  $(grep -m1 -E 'load module failed|Loaded kernel module' "$TMP/v2.err" 2>/dev/null)"
fi
sleep 1
sync

ls -1 "$DIR"/*.ko 2>/dev/null | sort > "$TMP/after"
new=$(grep -vxFf "$TMP/before" "$TMP/after" 2>/dev/null)
nnew=$(printf '%s\n' "$new" | grep -c .)

if [ -z "$new" ]; then
	bad "no new .ko appeared in $DIR"
	dmesg_new | grep "$MODNAME" | sed 's/^/         /'
	echo "== $pass passed, $fail failed =="
	exit 1
fi
ok "$nnew new file(s)"

src_md5=$(md5sum "$VICTIM" | awk '{print $1}')
src_sz=$(wc -c < "$VICTIM" | tr -d ' ')

for f in $new; do
	b=$(basename "$f")
	dst_sz=$(wc -c < "$f" | tr -d ' ')
	comm=$(comm_of "$b")

	case "$comm" in
	"")
		bad "$b: no modcapture.log line for it (comm= unknown)"
		continue
		;;
	ksud)
		# KernelSU rewrote the symbol table before init_module: the dump is the image
		# the kernel was handed, not the file on disk.  What has to hold is that it is
		# still the same size and still a readable module.
		d=$(cmp -l "$VICTIM" "$f" 2>/dev/null | wc -l | tr -d ' ')
		if [ "$dst_sz" != "$src_sz" ]; then
			bad "$b: ksud-path dump changed size ($dst_sz vs $src_sz)"
		elif ! modinfo "$f" 2>/dev/null | grep -q "name:.*$MODNAME"; then
			bad "$b: ksud-path dump is no longer a readable module (modinfo fails)"
		else
			ok "$b: ksud-path image, same size ($dst_sz), still readable by modinfo"
			info "$b: differs from the file on disk in $d byte(s) - expected: ksud binds every undefined symbol to its kallsyms address before init_module(2)"
		fi
		;;
	*)
		if cmp -s "$VICTIM" "$f"; then
			ok "$b: byte-identical to $(basename "$VICTIM") (comm=$comm, md5 $src_md5, $dst_sz bytes)"
		else
			dst_md5=$(md5sum "$f" | awk '{print $1}')
			bad "$b: differs from $(basename "$VICTIM") on the plain-load path (comm=$comm): $dst_md5 vs $src_md5"
		fi
		;;
	esac

	if printf '%s' "$b" | grep -qE '^[0-9]{8}-[0-9]{6}-[0-9]{6}\.ko$'; then
		ok "$b matches YYYYMMDD-HHMMSS-uuuuuu.ko"
	else
		bad "$b does not match the expected timestamp form"
	fi
done

# ---------------------------------------------------------------- 5. index
echo
echo "-- 3. index log"
if [ -f "$LOG" ]; then
	nlog1=$(log_lines)
	info "$((nlog1 - nlog0)) line(s) added; tail:"
	tail -4 "$LOG" | sed 's/^/         /'
	if tail -4 "$LOG" | grep -q "$MODNAME"; then
		ok "modcapture.log names the captured module"
	else
		bad "modcapture.log does not mention $MODNAME"
	fi
else
	bad "$LOG missing (index=1 by default)"
fi

# ---------------------------------------------------------------- 6. dmesg
echo
echo "-- 4. kernel log"
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

unload=$(dmesg_new | grep "$MODNAME: unloaded" | tail -1)
case "$unload" in
*unloaded:*) ok "kernel log: ${unload#*] }" ;;
*) bad "no unload summary in the kernel log" ;;
esac

echo
echo "-- captures left in $DIR:"
ls -l "$DIR"/*.ko 2>/dev/null | sed 's/^/         /'

echo
echo "== $pass passed, $fail failed =="
[ "$fail" = "0" ]
