#!/usr/bin/env bash
# Randomized lossless-validation for mp3packercpp -z.
#
# Complements scripts/test_matrix.sh's fixed combinatorial grid: instead of a
# handful of representative points, this draws N random cases across the full
# parameter space (all 9 MPEG1/2/2.5 sample rates, random durations, random
# sine frequencies / noise seeds / click periods, random bitrates, all channel
# modes, CRC on/off) and runs the same lossless + thread-determinism checks as
# test_matrix.sh on each one.
#
# Usage: scripts/test_random.sh [path/to/mp3packercpp] [N] [seed]
#   N    number of random cases to generate (default 50)
#   seed RANDOM seed for reproducibility (default: current time)

set -uo pipefail

BIN="${1:-cmake-build-release/bin/mp3packercpp}"
N="${2:-50}"
SEED="${3:-$RANDOM}"

if [ ! -x "$BIN" ]; then
    echo "error: binary not found or not executable: $BIN" >&2
    exit 1
fi
BIN="$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")"

RANDOM=$SEED
echo "== seed: $SEED (rerun with the same seed to reproduce) =="

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

# all valid MPEG1/2/2.5 sample rates
SAMPLERATES=(44100 48000 32000 22050 24000 16000 11025 12000 8000)

# Both helpers assign into a caller-named variable via eval rather than
# echo+command-substitution: a `$(...)` capture forks a subshell, and $RANDOM's
# state advanced inside a subshell never propagates back to the parent — every
# such call would silently restart from the same state, breaking the seed's
# reproducibility. eval keeps everything in the one persistent shell. (No
# `local -n` nameref either: macOS ships bash 3.2, which predates it.)
rand_range() { # rand_range VARNAME MIN MAX
    local _rr_val=$(( $2 + RANDOM % ($3 - $2 + 1) ))
    eval "$1=\$_rr_val"
}

rand_choice() { # rand_choice VARNAME item1 item2 ...
    local _rc_var="$1"; shift
    local _rc_arr=("$@")
    local _rc_val="${_rc_arr[$((RANDOM % $#))]}"
    eval "$_rc_var=\$_rc_val"
}

# CBR bitrates valid for each MPEG version's table (kbps). Asking lame for a
# bitrate outside the chosen version's table doesn't error — it silently
# resamples and switches version to make the request satisfiable (e.g. -b 8 on
# a 44100Hz source comes back as an 8000Hz MPEG2.5 file), which would quietly
# defeat the point of choosing samplerate and bitrate independently.
MPEG1_BITRATES=(32 40 48 56 64 80 96 112 128 160 192 224 256 320)
MPEG2_BITRATES=(8 16 24 32 40 48 56 64 80 96 112 128 144 160)

mpeg_version_for_rate() { # 1, 2 or 25
    case "$1" in
        44100|48000|32000) echo 1 ;;
        22050|24000|16000) echo 2 ;;
        11025|12000|8000)  echo 25 ;;
    esac
}

gen_case() {
    local idx="$1"
    local samplerate version signal filter ch mono_flag channel_mode_flag bitrate_mode lame_br crc_mode lame_crc duration seed label wav mp3
    local id3_mode id3_flags

    rand_choice samplerate "${SAMPLERATES[@]}"
    version="$(mpeg_version_for_rate "$samplerate")"
    rand_choice signal sine clicks noise sweep silence
    rand_range duration 1 6
    seed="$RANDOM"

    case "$signal" in
        sine)    local sfreq; rand_range sfreq 60 12000
                 filter="sine=frequency=${sfreq}:duration=${duration}" ;;
        sweep)   local f0 f1
                 rand_range f0 60 3000
                 rand_range f1 3000 15000
                 filter="aevalsrc=exprs='sin(2*PI*(${f0}+(${f1}-${f0})*t/(2*${duration}))*t)':d=${duration}" ;;
        clicks)  local period; rand_range period 500 8000
                 filter="aevalsrc=exprs='if(eq(mod(n\,${period})\,0)\,1\,0)':d=${duration}" ;;
        noise)   local color; rand_choice color white pink brown
                 filter="anoisesrc=color=${color}:seed=${seed}:duration=${duration}" ;;
        silence) filter="anullsrc=duration=${duration}" ;;
    esac

    rand_choice ch 1 2
    mono_flag=""
    channel_mode_flag=""
    if [ "$ch" = "1" ]; then
        mono_flag="-m m"
    else
        rand_choice channel_mode_flag "-m s" "-m j" "-m f"
    fi

    rand_choice bitrate_mode cbr vbr
    if [ "$bitrate_mode" = "cbr" ]; then
        local br
        if [ "$version" = "1" ]; then
            rand_choice br "${MPEG1_BITRATES[@]}"
        else
            rand_choice br "${MPEG2_BITRATES[@]}"
        fi
        lame_br="-b $br --cbr"
    else
        local vq; rand_range vq 0 9
        lame_br="-V $vq"
    fi

    rand_choice crc_mode crc nocrc
    lame_crc=""
    [ "$crc_mode" = "crc" ] && lame_crc="-c"

    # tag coverage: none (lame's bare default, no tags at all), id3v2 only,
    # id3v1 only, or both (lame's default *with* tag content given, which
    # writes both unless told to restrict to one).
    rand_choice id3_mode none v2 v1 both
    case "$id3_mode" in
        none) id3_flags="" ;;
        v2)   id3_flags="--tt Test --ta Artist --id3v2-only" ;;
        v1)   id3_flags="--tt Test --ta Artist --id3v1-only" ;;
        both) id3_flags="--tt Test --ta Artist" ;;
    esac

    label="case${idx}: ${signal}/${samplerate}Hz(mpeg${version})/ch${ch}/${bitrate_mode}/${crc_mode}/id3-${id3_mode} (dur=${duration}s)"

    wav="$WORKDIR/case${idx}.wav"
    mp3="$WORKDIR/case${idx}.mp3"

    if ! ffmpeg -y -v error -f lavfi -i "${filter}:sample_rate=${samplerate}" -ar "$samplerate" -ac "$ch" "$wav" 2>/dev/null; then
        echo "SKIP  $label  (source generation failed)"
        return
    fi
    if ! lame $mono_flag $channel_mode_flag $lame_crc $lame_br $id3_flags "$wav" "$mp3" >/dev/null 2>&1; then
        echo "SKIP  $label  (lame encode failed)"
        return
    fi

    check_case "$label" "$mp3"
}

check_case() {
    local label="$1" src="$2"
    TOTAL=$((TOTAL + 1))

    local orig_pcm="$WORKDIR/orig.f32"
    if ! ffmpeg -y -v error -i "$src" -f f32le "$orig_pcm" 2>/dev/null; then
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
        FAILURES+=("$label :: no-z diffs=$d_noz  t1 diffs=$d_t1  tN diffs=$d_tn  t1==tN:$det_ok  src=$src")
        echo "FAIL  $label  (no-z=$d_noz t1=$d_t1 tN=$d_tn t1==tN:$det_ok)"
        # preserve the failing source for reproduction
        cp "$src" "/tmp/random_fail_case${TOTAL}.mp3"
    else
        echo "ok    $label"
    fi
}

for i in $(seq 1 "$N"); do
    gen_case "$i"
done

echo ""
echo "== summary: $((TOTAL - FAIL))/$TOTAL passed (seed=$SEED) =="
if [ "$FAIL" -gt 0 ]; then
    echo ""
    echo "failures (sources preserved as /tmp/random_fail_caseN.mp3):"
    for f in "${FAILURES[@]}"; do
        echo "  $f"
    done
    exit 1
fi
