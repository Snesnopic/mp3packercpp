#!/usr/bin/env bash
# Broad lossless-validation matrix for mp3packercpp -z.
#
# Generates a corpus spanning MPEG version (via samplerate), channel mode,
# CBR/VBR, CRC on/off, and signal type (stationary / transient-heavy / noisy),
# then verifies that mp3packercpp:
#   - no-z output decodes to identical PCM as the source
#   - -z -t 1 output decodes to identical PCM as the source
#   - -z -t <max> output decodes to identical PCM as the source
#   - -z -t 1 and -z -t <max> outputs are byte-identical (algorithm must be
#     deterministic regardless of thread count)
#
# Usage: scripts/test_matrix.sh [path/to/mp3packercpp]

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

TOTAL=0
FAIL=0
declare -a FAILURES=()

pcm_diff() {
    python3 -c "
import struct, sys
a = open(sys.argv[1], 'rb').read()
b = open(sys.argv[2], 'rb').read()
n = min(len(a), len(b)) // 4
diffs = sum(1 for i in range(n) if abs(struct.unpack_from('f', a, i*4)[0] - struct.unpack_from('f', b, i*4)[0]) > 0.0001)
print(diffs)
" "$1" "$2"
}

check_case() {
    local label="$1" src="$2"
    TOTAL=$((TOTAL + 1))

    local orig_pcm="$WORKDIR/orig.f32"
    if ! ffmpeg -y -v error -i "$src" -f f32le "$orig_pcm"; then
        FAIL=$((FAIL + 1)); FAILURES+=("$label :: source failed to decode"); echo "FAIL  $label  (source failed to decode)"; return
    fi

    local out_noz="$WORKDIR/out_noz.mp3" out_t1="$WORKDIR/out_t1.mp3" out_tn="$WORKDIR/out_tn.mp3"
    if ! "$BIN" "$src" "$out_noz" >/dev/null 2>&1; then
        FAIL=$((FAIL + 1)); FAILURES+=("$label :: no-z pack failed"); echo "FAIL  $label  (no-z pack failed)"; return
    fi
    if ! "$BIN" -z -t 1 "$src" "$out_t1" >/dev/null 2>&1; then
        FAIL=$((FAIL + 1)); FAILURES+=("$label :: -z -t1 pack failed"); echo "FAIL  $label  (-z -t1 pack failed)"; return
    fi
    if ! "$BIN" -z -t "$NCPU" "$src" "$out_tn" >/dev/null 2>&1; then
        FAIL=$((FAIL + 1)); FAILURES+=("$label :: -z -tN pack failed"); echo "FAIL  $label  (-z -tN pack failed)"; return
    fi

    local pcm_noz="$WORKDIR/pcm_noz.f32" pcm_t1="$WORKDIR/pcm_t1.f32" pcm_tn="$WORKDIR/pcm_tn.f32"
    if ! ffmpeg -y -v error -i "$out_noz" -f f32le "$pcm_noz" 2>/dev/null; then
        FAIL=$((FAIL + 1)); FAILURES+=("$label :: no-z output failed to decode"); echo "FAIL  $label  (no-z output failed to decode)"; return
    fi
    if ! ffmpeg -y -v error -i "$out_t1" -f f32le "$pcm_t1" 2>/dev/null; then
        FAIL=$((FAIL + 1)); FAILURES+=("$label :: -t1 output failed to decode"); echo "FAIL  $label  (-t1 output failed to decode)"; return
    fi
    if ! ffmpeg -y -v error -i "$out_tn" -f f32le "$pcm_tn" 2>/dev/null; then
        FAIL=$((FAIL + 1)); FAILURES+=("$label :: -tN output failed to decode"); echo "FAIL  $label  (-tN output failed to decode)"; return
    fi

    local d_noz d_t1 d_tn
    d_noz="$(pcm_diff "$orig_pcm" "$pcm_noz")"
    d_t1="$(pcm_diff "$orig_pcm" "$pcm_t1")"
    d_tn="$(pcm_diff "$orig_pcm" "$pcm_tn")"

    local det_ok="yes"
    cmp -s "$out_t1" "$out_tn" || det_ok="no"

    if [ "$d_noz" != "0" ] || [ "$d_t1" != "0" ] || [ "$d_tn" != "0" ] || [ "$det_ok" != "yes" ]; then
        FAIL=$((FAIL + 1))
        FAILURES+=("$label :: no-z diffs=$d_noz  t1 diffs=$d_t1  tN diffs=$d_tn  t1==tN:$det_ok")
        echo "FAIL  $label  (no-z=$d_noz t1=$d_t1 tN=$d_tn t1==tN:$det_ok)"
    else
        echo "ok    $label"
    fi
}

echo "== generating corpus in $WORKDIR =="

signal_filter() {
    case "$1" in
        sine)   echo "sine=frequency=440:duration=4" ;;
        clicks) echo "aevalsrc=exprs='if(eq(mod(n\,2205)\,0)\,1\,0)':d=4" ;;
        noise)  echo "anoisesrc=color=white:duration=4" ;;
    esac
}

for signal in sine clicks noise; do
    filter="$(signal_filter "$signal")"
    for samplerate in 44100 22050 11025; do
        for channels_label in mono stereo; do
            [ "$channels_label" = "mono" ] && ch=1 || ch=2
            wav="$WORKDIR/${signal}_${samplerate}_${channels_label}.wav"
            ffmpeg -y -v error -f lavfi -i "${filter}:sample_rate=${samplerate}" -ar "$samplerate" -ac "$ch" "$wav"

            for bitrate_mode in cbr vbr; do
                [ "$bitrate_mode" = "cbr" ] && lame_br="-b 128 --cbr" || lame_br="-V 2"
                for crc_mode in crc nocrc; do
                    [ "$crc_mode" = "crc" ] && lame_crc="-c" || lame_crc=""
                    mono_flag=""
                    [ "$channels_label" = "mono" ] && mono_flag="-m m"

                    mp3="$WORKDIR/${signal}_${samplerate}_${channels_label}_${bitrate_mode}_${crc_mode}.mp3"
                    lame $mono_flag $lame_crc $lame_br "$wav" "$mp3" >/dev/null 2>&1

                    label="${signal}/${samplerate}Hz/${channels_label}/${bitrate_mode}/${crc_mode}"
                    check_case "$label" "$mp3"
                done
            done
        done
    done
done

echo ""
echo "== summary: $((TOTAL - FAIL))/$TOTAL passed =="
if [ "$FAIL" -gt 0 ]; then
    echo ""
    echo "failures:"
    for f in "${FAILURES[@]}"; do
        echo "  $f"
    done
    exit 1
fi
