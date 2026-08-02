#!/usr/bin/env bash
# Deep regression check for mp3packercpp.
#
# Where test_matrix.sh and test_random.sh go wide (many parameter combinations,
# one decoder), this goes deep on a representative corpus:
#
#   - cross-decoder losslessness: ffmpeg (mp3float), lame --decode and mpg123
#     must each decode the packed file to the same PCM they decode the source to.
#     One decoder agreeing proves less than it looks: a wrong Xing/LAME tag, for
#     instance, changes gapless trimming in ffmpeg but not in mpg123.
#   - decoded length: a frame silently dropped or added shows up here even when
#     the overlapping samples all match (this is what bug #10 did).
#   - structural validity via mp3val, over and above "it decodes".
#   - idempotence: packing an already-packed file must not change it further.
#   - thread determinism: -t 1 and -t N byte-identical.
#
# Usage: scripts/test_regression.sh [path/to/mp3packercpp]

set -uo pipefail

BIN="${1:-cmake-build-release/bin/mp3packercpp}"
if [ ! -x "$BIN" ]; then
    echo "error: binary not found or not executable: $BIN" >&2
    exit 1
fi
BIN="$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")"

WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT
NCPU="$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)"

TOTAL=0; FAIL=0
declare -a FAILURES=()

fail() { FAIL=$((FAIL+1)); FAILURES+=("$1"); echo "  FAIL: $1"; }

# Decode to raw s16le with a named decoder. Each decoder gets applied to both the
# source and the packed file, so any decoder-specific delay cancels out.
decode_raw() { # decode_raw DECODER IN OUT_RAW
    local dec="$1" in="$2" out="$3"
    case "$dec" in
        ffmpeg)
            ffmpeg -y -v error -i "$in" -f s16le -acodec pcm_s16le "$out" 2>/dev/null ;;
        lame)
            lame --quiet --decode "$in" "$out.wav" 2>/dev/null && \
            ffmpeg -y -v error -i "$out.wav" -f s16le -acodec pcm_s16le "$out" 2>/dev/null ;;
        mpg123)
            mpg123 -q -w "$out.wav" "$in" 2>/dev/null && \
            ffmpeg -y -v error -i "$out.wav" -f s16le -acodec pcm_s16le "$out" 2>/dev/null ;;
    esac
}

cmp_raw() { # cmp_raw A B -> "diffs len_a len_b"
    python3 -c "
import sys
a=open(sys.argv[1],'rb').read(); b=open(sys.argv[2],'rb').read()
n=min(len(a),len(b))//2
import struct
d=sum(1 for i in range(n) if a[2*i:2*i+2]!=b[2*i:2*i+2])
print(d, len(a)//2, len(b)//2)
" "$1" "$2"
}

check_file() { # check_file LABEL SRC
    local label="$1" src="$2"
    TOTAL=$((TOTAL+1))
    echo "== $label"

    local noz="$WORKDIR/o_noz.mp3" t1="$WORKDIR/o_t1.mp3" tn="$WORKDIR/o_tn.mp3"
    "$BIN" "$src" "$noz" >/dev/null 2>&1     || { fail "$label: no-z pack failed"; return; }
    "$BIN" -z -t 1 "$src" "$t1" >/dev/null 2>&1 || { fail "$label: -z -t1 pack failed"; return; }
    "$BIN" -z -t "$NCPU" "$src" "$tn" >/dev/null 2>&1 || { fail "$label: -z -tN pack failed"; return; }

    # thread determinism
    cmp -s "$t1" "$tn" || fail "$label: -t1 and -tN differ"

    # structural validity (ignore cosmetic tag/VBR-header notes)
    for f in "$noz" "$t1"; do
        local w
        w=$(mp3val "$f" 2>&1 | grep -E "WARNING|ERROR" \
            | grep -v "No supported tags" | grep -v "VBR detected" | wc -l | tr -d ' ')
        [ "$w" = "0" ] || fail "$label: mp3val reported $w issue(s) on $(basename "$f")"
    done

    # cross-decoder losslessness, source vs each output
    for dec in ffmpeg lame mpg123; do
        local ref="$WORKDIR/ref_$dec.raw"
        decode_raw "$dec" "$src" "$ref" || { fail "$label: $dec could not decode source"; continue; }
        for f in "$noz" "$t1"; do
            local got="$WORKDIR/got_$dec.raw"
            if ! decode_raw "$dec" "$f" "$got"; then
                fail "$label: $dec could not decode $(basename "$f")"; continue
            fi
            local r; r=$(cmp_raw "$ref" "$got")
            local d la lb; read -r d la lb <<<"$r"
            [ "$d" = "0" ] || fail "$label: $dec $(basename "$f") $d differing samples"
            [ "$la" = "$lb" ] || fail "$label: $dec $(basename "$f") length $la vs $lb samples"
        done
    done

    # idempotence: packing the packed file again must change nothing
    local again="$WORKDIR/o_again.mp3"
    "$BIN" -z "$t1" "$again" >/dev/null 2>&1 || { fail "$label: repack failed"; return; }
    cmp -s "$t1" "$again" || fail "$label: not idempotent (repack changed bytes)"
}

echo "== building corpus =="
for sr in 44100 48000 32000 22050 24000 16000 11025 12000 8000; do
    for ch in 1 2; do
        wav="$WORKDIR/s_${sr}_${ch}.wav"
        ffmpeg -y -v error -f lavfi -i "anoisesrc=color=pink:seed=11:duration=3:sample_rate=$sr" \
               -ar "$sr" -ac "$ch" "$wav" 2>/dev/null || continue
        mono=""; [ "$ch" = "1" ] && mono="-m m"
        for crc in "" "-c"; do
            tag="nocrc"; [ -n "$crc" ] && tag="crc"
            mp3="$WORKDIR/c_${sr}_${ch}_${tag}.mp3"
            lame $mono $crc -V 4 "$wav" "$mp3" >/dev/null 2>&1 || continue
            check_file "${sr}Hz/ch${ch}/${tag}" "$mp3"
        done
    done
done

# the long real-world track, if present
REAL="$(dirname "$0")/../build/bin/a.mp3"
[ -f "$REAL" ] && check_file "real-track-166s" "$REAL"

echo ""
echo "== summary: $((TOTAL-FAIL)) / $TOTAL files clean =="
if [ "$FAIL" -gt 0 ]; then
    echo ""
    printf '%s\n' "${FAILURES[@]}"
    exit 1
fi
