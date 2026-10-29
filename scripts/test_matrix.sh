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

    # Constant-bitrate output: lossless like the rest, and actually constant —
    # every frame carrying the same bitrate index, which is the whole point of
    # the mode and the thing a caller would notice if the search picked a
    # bitrate some frame does not fit in.
    local cbr_status="ok" cbr_out="$WORKDIR/out_cbr.mp3" pcm_cbr="$WORKDIR/pcm_cbr.f32"
    local reported=""
    if ! reported="$("$BIN" --ib -z "$src" 2>/dev/null)"; then
        cbr_status="--ib failed"
    elif ! "$BIN" --cbr -z "$src" "$cbr_out" >/dev/null 2>&1; then
        cbr_status="pack failed"
    elif ! ffmpeg -y -v error -i "$cbr_out" -f f32le "$pcm_cbr" 2>/dev/null; then
        cbr_status="output failed to decode"
    else
        local d_cbr; d_cbr="$(pcm_diff "$orig_pcm" "$pcm_cbr")"
        [ "$d_cbr" = "0" ] || cbr_status="$d_cbr PCM diffs"
        local seen; seen="$(frame_bitrates "$cbr_out")"
        if [ "$(echo "$seen" | wc -w | tr -d ' ')" != "1" ]; then
            cbr_status="bitrate not constant: $seen"
        elif [ -n "$reported" ] && [ "$seen" != "$reported" ]; then
            cbr_status="--ib said $reported but output is $seen"
        fi
    fi

    # CRC preservation: --keep-crc must stay lossless, must actually carry CRCs
    # when the source had them, and every CRC it writes must be right. The default
    # is checked too, since dropping the CRC is what every release so far did.
    local crc_status="ok" crc_out="$WORKDIR/out_crc.mp3" pcm_crc="$WORKDIR/pcm_crc.f32"
    local src_prot; src_prot="$(crc_report "$src" | cut -d' ' -f2)"
    if [ "$(echo "$d_noz $d_t1" | tr -d ' 0')" = "" ]; then
        local def_rep; def_rep="$(crc_report "$out_t1")"
        if [ "$(echo "$def_rep" | cut -d' ' -f2)" != "0" ]; then
            crc_status="default output kept a CRC ($def_rep)"
        elif ! "$BIN" -z --keep-crc "$src" "$crc_out" >/dev/null 2>&1; then
            crc_status="--keep-crc pack failed"
        elif ! ffmpeg -y -v error -i "$crc_out" -f f32le "$pcm_crc" 2>/dev/null; then
            crc_status="--keep-crc output failed to decode"
        else
            local d_crc; d_crc="$(pcm_diff "$orig_pcm" "$pcm_crc")"
            local rep; rep="$(crc_report "$crc_out")"
            local out_prot; out_prot="$(echo "$rep" | cut -d' ' -f2)"
            local out_bad; out_bad="$(echo "$rep" | cut -d' ' -f3)"
            if [ "$d_crc" != "0" ]; then
                crc_status="--keep-crc $d_crc PCM diffs"
            elif [ "$out_bad" != "0" ]; then
                crc_status="$out_bad frames with a wrong CRC"
            elif [ "$src_prot" != "0" ] && [ "$out_prot" = "0" ]; then
                crc_status="source had $src_prot CRC frames, output has none"
            elif [ "$src_prot" = "0" ] && [ "$out_prot" != "0" ]; then
                crc_status="source had no CRC, output invented $out_prot"
            fi
        fi
    fi

    # Xing fidelity: --preserve-xing must keep the frame and its tag the length they
    # arrived at, where the default is free to shrink them around the fields it knows.
    # Either way the LAME checksums must describe what was actually written.
    local xing_status="ok" xing_out="$WORKDIR/out_xing.mp3" pcm_xing="$WORKDIR/pcm_xing.f32"
    local src_x; src_x="$(xing_report "$src")"
    if [ "$(echo "$d_noz $d_t1" | tr -d ' 0')" = "" ] && [ "$(echo "$src_x" | cut -d' ' -f1)" != "0" ]; then
        local def_x; def_x="$(xing_report "$out_t1")"
        if [ "$(echo "$def_x" | cut -d' ' -f3)" = "bad" ]; then
            xing_status="default output has stale LAME checksums"
        elif ! "$BIN" -z --preserve-xing "$src" "$xing_out" >/dev/null 2>&1; then
            xing_status="--preserve-xing pack failed"
        elif ! ffmpeg -y -v error -i "$xing_out" -f f32le "$pcm_xing" 2>/dev/null; then
            xing_status="--preserve-xing output failed to decode"
        else
            local d_x; d_x="$(pcm_diff "$orig_pcm" "$pcm_xing")"
            local out_x; out_x="$(xing_report "$xing_out")"
            if [ "$d_x" != "0" ]; then
                xing_status="--preserve-xing $d_x PCM diffs"
            elif [ "$(echo "$out_x" | cut -d' ' -f3)" = "bad" ]; then
                xing_status="stale LAME checksums under --preserve-xing"
            elif [ "$(echo "$out_x" | cut -d' ' -f1,2)" != "$(echo "$src_x" | cut -d' ' -f1,2)" ]; then
                xing_status="frame/tag was [$src_x], output is [$out_x]"
            fi
        fi
    fi

    if [ "$d_noz" != "0" ] || [ "$d_t1" != "0" ] || [ "$d_tn" != "0" ] || [ "$det_ok" != "yes" ]; then
        FAIL=$((FAIL + 1))
        FAILURES+=("$label :: no-z diffs=$d_noz  t1 diffs=$d_t1  tN diffs=$d_tn  t1==tN:$det_ok")
        echo "FAIL  $label  (no-z=$d_noz t1=$d_t1 tN=$d_tn t1==tN:$det_ok)"
    elif [ "$cbr_status" != "ok" ]; then
        FAIL=$((FAIL + 1))
        FAILURES+=("$label :: cbr $cbr_status")
        echo "FAIL  $label  (cbr: $cbr_status)"
    elif [ "$crc_status" != "ok" ]; then
        FAIL=$((FAIL + 1))
        FAILURES+=("$label :: crc $crc_status")
        echo "FAIL  $label  (crc: $crc_status)"
    elif [ "$xing_status" != "ok" ]; then
        FAIL=$((FAIL + 1))
        FAILURES+=("$label :: xing $xing_status")
        echo "FAIL  $label  (xing: $xing_status)"
    else
        echo "ok    $label"
    fi
}

# "<frame0_bytes> <tag_bytes> <lame_crc>" for a file: how long the xing frame and
# its tag are, and whether the two checksums the LAME extension carries agree with
# a recomputation ("ok", "bad", or "none" when there is no extension). Those
# checksums cover the audio and the tag itself, both of which a repack rewrites.
xing_report() {
    python3 -c "
import sys
d=open(sys.argv[1],'rb').read()
i=0
if d[:3]==b'ID3':
    i=10+(((d[6]&0x7F)<<21)|((d[7]&0x7F)<<14)|((d[8]&0x7F)<<7)|(d[9]&0x7F))
while i<len(d)-4 and not (d[i]==0xFF and (d[i+1]&0xE0)==0xE0): i+=1
v1=[0,32,40,48,56,64,80,96,112,128,160,192,224,256,320,0]
v2=[0,8,16,24,32,40,48,56,64,80,96,112,128,144,160,0]
ver=(d[i+1]>>3)&3
tbl=v1 if ver==3 else v2
mult=144 if ver==3 else 72
sr=[44100,48000,32000][(d[i+2]>>2)&3]//(1 if ver==3 else (2 if ver==2 else 4))
size=mult*tbl[(d[i+2]>>4)&0xF]*1000//sr+((d[i+2]>>1)&1)
fr=d[i:i+size]
t=max(fr.find(b'Xing'), fr.find(b'Info'))
if t<0:
    print(0, 0, 'none'); raise SystemExit
end=len(d)
if d[end-128:end-125]==b'TAG': end-=128

def crc(buf):
    c=0
    for b in buf:
        c^=b
        for _ in range(8): c=(c>>1)^(0xA001 if c&1 else 0)
    return c

fl=int.from_bytes(fr[t+4:t+8],'big')
off=t+8
for m,n in ((1,4),(2,4),(4,100),(8,4)):
    if fl&m: off+=n
state='none'
if off+36<=size and 0x20<=fr[off]<=0x7E:
    sm=(fr[off+32]<<8)|fr[off+33]
    st=(fr[off+34]<<8)|fr[off+35]
    state='ok' if (sm==crc(d[i+size:end]) and st==crc(fr[:off+34])) else 'bad'
print(size, size-t, state)
" "$1"
}

# "<frames> <protected> <bad>" for a file: how many frames carry a CRC, and how
# many of those disagree with a CRC recomputed here. The recomputation is
# deliberately independent of the packer's own, so a shared misunderstanding of
# the polynomial or the covered bytes cannot pass unnoticed.
crc_report() {
    python3 -c "
import sys
d=open(sys.argv[1],'rb').read()
i=0
if d[:3]==b'ID3':
    i=10+(((d[6]&0x7F)<<21)|((d[7]&0x7F)<<14)|((d[8]&0x7F)<<7)|(d[9]&0x7F))
v1=[0,32,40,48,56,64,80,96,112,128,160,192,224,256,320,0]
v2=[0,8,16,24,32,40,48,56,64,80,96,112,128,144,160,0]
rates=[44100,48000,32000]

def crc16(payload):
    crc=0xFFFF
    for byte in payload:
        value=byte<<8
        for _ in range(8):
            value=(value<<1)&0x1FFFF
            crc=(crc<<1)&0x1FFFF
            if (value^crc)&0x10000: crc^=0x8005
    return crc&0xFFFF

frames=protected=bad=0
while i < len(d)-4:
    if d[i]==0xFF and (d[i+1]&0xE0)==0xE0:
        ver=(d[i+1]>>3)&3
        if ver==1: i+=1; continue
        tbl = v1 if ver==3 else v2
        mult = 144 if ver==3 else 72
        sr = rates[(d[i+2]>>2)&3] // (1 if ver==3 else (2 if ver==2 else 4))
        br = tbl[(d[i+2]>>4)&0xF]; pad=(d[i+2]>>1)&1
        if br==0 or sr==0: i+=1; continue
        frames+=1
        mono = ((d[i+3]>>6)&3)==3
        si = (17 if mono else 32) if ver==3 else (9 if mono else 17)
        if not (d[i+1]&1):
            protected+=1
            stored=(d[i+4]<<8)|d[i+5]
            if crc16(bytes([d[i+2],d[i+3]])+d[i+6:i+6+si])!=stored: bad+=1
        i += mult*br*1000//sr + pad
    else:
        i += 1
print(frames, protected, bad)
" "$1"
}

# How many frames the file is, if every frame's declared length lands exactly on
# the next one's sync word from the first to the end; 0 if the chain breaks.
chain_length() {
    python3 -c "
import sys
d=open(sys.argv[1],'rb').read()
i=0
if d[:3]==b'ID3':
    i=10+(((d[6]&0x7F)<<21)|((d[7]&0x7F)<<14)|((d[8]&0x7F)<<7)|(d[9]&0x7F))
end=len(d)
if end>=128 and d[end-128:end-125]==b'TAG': end-=128
v1=[0,32,40,48,56,64,80,96,112,128,160,192,224,256,320,0]
v2=[0,8,16,24,32,40,48,56,64,80,96,112,128,144,160,0]
n=0
while i+4<=end:
    if d[i]!=0xFF or (d[i+1]&0xE0)!=0xE0: print(0); raise SystemExit
    ver=(d[i+1]>>3)&3; lay=(d[i+1]>>1)&3; bi=(d[i+2]>>4)&0xF; ri=(d[i+2]>>2)&3
    if ver==1 or lay!=1 or bi in (0,15) or ri==3: print(0); raise SystemExit
    br=(v1 if ver==3 else v2)[bi]
    rate=[44100,48000,32000][ri]//(1 if ver==3 else (2 if ver==2 else 4))
    i+=(144 if ver==3 else 72)*br*1000//rate+((d[i+2]>>1)&1)
    n+=1
print(n if i==end else 0)
" "$1"
}

# The distinct bitrates the frames of a file carry, as a space-separated list.
frame_bitrates() {
    python3 -c "
import sys
d=open(sys.argv[1],'rb').read()
i=0
if d[:3]==b'ID3':
    i=10+(((d[6]&0x7F)<<21)|((d[7]&0x7F)<<14)|((d[8]&0x7F)<<7)|(d[9]&0x7F))
v1=[0,32,40,48,56,64,80,96,112,128,160,192,224,256,320,0]
v2=[0,8,16,24,32,40,48,56,64,80,96,112,128,144,160,0]
rates=[44100,48000,32000]
seen=[]
while i < len(d)-4:
    if d[i]==0xFF and (d[i+1]&0xE0)==0xE0:
        ver=(d[i+1]>>3)&3
        if ver==1: i+=1; continue
        tbl = v1 if ver==3 else v2
        mult = 144 if ver==3 else 72
        sr = rates[(d[i+2]>>2)&3] // (1 if ver==3 else (2 if ver==2 else 4))
        br = tbl[(d[i+2]>>4)&0xF]; pad=(d[i+2]>>1)&1
        if br==0 or sr==0: i+=1; continue
        if br not in seen: seen.append(br)
        i += mult*br*1000//sr + pad
    else:
        i += 1
print(' '.join(str(b) for b in sorted(seen)))
" "$1"
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

# Second corpus: encoded through ffmpeg's libmp3lame rather than the lame CLI, and
# without pinning the stereo mode.
#
# Everything above passes lame an explicit -m, which fixes one stereo mode for the
# whole file. Left to itself the encoder decides per frame, so a file can open in
# plain L/R and switch to joint/MS a frame or two in. That is the only way to get a
# file whose frames do not all share frame 0's mode — and it is exactly what the
# packer got wrong (writing frame 0's mode into every output header, leaving mid/side
# data labelled as L/R: structurally valid, audio destroyed).
#
# Whether the encoder switches at all depends on how correlated the channels are, so
# the content matters as much as the settings: identical channels and noise push it
# into MS, two unrelated tones keep it in plain stereo.
echo "== ffmpeg-encoded corpus (per-frame stereo mode decisions) =="

ff_content() { # ff_content NAME SAMPLERATE -> ffmpeg input args
    local name="$1" sr="$2"
    case "$name" in
        # both channels the same: encoder goes to MS, side channel vanishes
        dup-sine)   echo "-f lavfi -i sine=frequency=440:duration=5:sample_rate=$sr -ac 2" ;;
        # unrelated tones per channel: encoder tends to stay in plain L/R
        lr-sines)   echo "-f lavfi -i sine=frequency=440:duration=5:sample_rate=$sr -f lavfi -i sine=frequency=660:duration=5:sample_rate=$sr -filter_complex [0][1]amerge=inputs=2[a] -map [a]" ;;
        pink-noise) echo "-f lavfi -i anoisesrc=d=5:c=pink:r=$sr:a=0.5:seed=1 -ac 2" ;;
        mono-noise) echo "-f lavfi -i anoisesrc=d=5:c=white:r=$sr:a=0.5:seed=2 -ac 1" ;;
    esac
}

for content in dup-sine lr-sines pink-noise mono-noise; do
    for samplerate in 48000 44100 22050; do
        for rate_opt in "-b:a 192k" "-b:a 320k" "-q:a 2"; do
            mp3="$WORKDIR/ff_${content}_${samplerate}_$(echo "$rate_opt" | tr -d ' :').mp3"
            label="ffmpeg/${content}/${samplerate}Hz/${rate_opt}"
            # A silent skip here would hide this whole corpus — the one that covers
            # per-frame stereo mode switching — so an encode that fails is reported
            # as a failure, not passed over. Needs an ffmpeg built with libmp3lame.
            # shellcheck disable=SC2046,SC2086
            if ! ffmpeg -y -v error $(ff_content "$content" "$samplerate") \
                        -c:a libmp3lame $rate_opt "$mp3" 2>/dev/null; then
                TOTAL=$((TOTAL + 1)); FAIL=$((FAIL + 1))
                FAILURES+=("$label :: ffmpeg/libmp3lame could not encode this case")
                echo "FAIL  $label  (ffmpeg/libmp3lame encode failed)"
                continue
            fi
            check_case "$label" "$mp3"
        done
    done
done

# Streams that change channel count partway through. No encoder emits one, but
# concatenating two files does, and the packer used to size every frame's side
# info from frame 0's channel mode — so a stereo frame in a mono stream came out
# eight bytes shorter than its own header declared and every frame after it lost
# sync. Same shape as the per-frame stereo mode bug (#12), one field over; found
# by the fuzzer. The check is structural: frames in must equal frames out, with
# each frame's declared length landing exactly on the next one's sync word.
echo ""
echo "== concatenated streams (channel count changes mid-file) =="
for rate in 44100 22050 11025; do
    for order in "mono stereo" "stereo mono" "mono stereo mono"; do
        parts=""
        idx=0
        for mode in $order; do
            src="$WORKDIR/cat_${rate}_${idx}.mp3"
            ch=$([ "$mode" = "mono" ] && echo 1 || echo 2)
            # No tags and no Xing frame on the parts: ffmpeg writes an ID3v2 header
            # at the start of each, and one landing in the middle of the joined file
            # would break the chain before the packer ever saw it — the test would
            # then be failing on its own fixture.
            ffmpeg -y -v error -f lavfi -i "sine=frequency=$((300 + idx * 220)):duration=1:sample_rate=$rate" \
                   -ac "$ch" -c:a libmp3lame -b:a 96k -write_xing 0 -id3v2_version 0 \
                   -map_metadata -1 "$src" 2>/dev/null || continue
            parts="$parts $src"
            idx=$((idx + 1))
        done
        joined="$WORKDIR/joined_${rate}_$(echo "$order" | tr ' ' '-').mp3"
        cat $parts > "$joined" 2>/dev/null || continue
        TOTAL=$((TOTAL + 1))
        label="concat/${rate}Hz/$(echo "$order" | tr ' ' '+')"
        before="$(chain_length "$joined")"
        ok=yes
        if [ "$before" = "0" ]; then ok="the joined fixture is not an unbroken chain"; fi
        [ "$ok" = "yes" ] || { FAIL=$((FAIL + 1)); FAILURES+=("$label :: $ok"); echo "FAIL  $label  ($ok)"; continue; }
        for flags in "" "-z"; do
            out="$WORKDIR/joined_out.mp3"
            if ! $BIN $flags "$joined" "$out" >/dev/null 2>&1; then ok="pack failed$flags"; break; fi
            after="$(chain_length "$out")"
            if [ "$after" = "0" ]; then ok="output is not an unbroken chain ($flags)"; break; fi
            if [ "$after" != "$before" ]; then ok="$before frames in, $after out ($flags)"; break; fi
        done
        if [ "$ok" = "yes" ]; then
            echo "ok    $label  ($before frames, chain intact)"
        else
            FAIL=$((FAIL + 1)); FAILURES+=("$label :: $ok"); echo "FAIL  $label  ($ok)"
        fi
    done
done

# A protected xing frame whose LAME extension carries a vendor other than "LAME".
# The default path trims a CRC input's tag after the fields it knows how to size,
# and it used to recognise the extension by the literal "LAME" magic — so a "Lavc"
# or "Lavf" block was cut off, taking the encoder delay and padding with it, and
# decoders stopped trimming them (1296-2016 extra samples). ffmpeg reads gapless
# from all three vendors, which is what makes this observable. No encoder in our
# corpus writes both a CRC and a non-lame vendor, so the fixture patches one in.
echo ""
echo "== xing extension with a non-lame vendor (crc input, trimmed tag) =="
for vendor in Lavc Lavf; do
    src="$WORKDIR/vendor_src.mp3"
    patched="$WORKDIR/vendor_${vendor}.mp3"
    ffmpeg -y -v error -f lavfi -i "anoisesrc=duration=3:color=pink:sample_rate=44100" -ac 2 "$WORKDIR/vendor.wav" 2>/dev/null
    lame --quiet -V2 -p "$WORKDIR/vendor.wav" "$src"
    python3 -c "
import sys
d=bytearray(open(sys.argv[1],'rb').read())
i=max(d.find(b'Xing',0,64), d.find(b'Info',0,64))
fl=int.from_bytes(d[i+4:i+8],'big'); off=i+8
for m,n in ((1,4),(2,4),(4,100),(8,4)):
    if fl&m: off+=n
d[off:off+4]=sys.argv[3].encode()
open(sys.argv[2],'wb').write(bytes(d))
" "$src" "$patched" "$vendor"
    want="$(ffmpeg -v error -i "$patched" -f s16le - 2>/dev/null | wc -c | tr -d ' ')"
    for flags in "" "-z"; do
        TOTAL=$((TOTAL + 1))
        label="vendor/$vendor/${flags:-no-z}"
        out="$WORKDIR/vendor_out.mp3"
        status=ok
        if ! $BIN $flags "$patched" "$out" >/dev/null 2>&1; then
            status="pack failed"
        else
            got="$(ffmpeg -v error -i "$out" -f s16le - 2>/dev/null | wc -c | tr -d ' ')"
            ext="$(xing_report "$out" | cut -d' ' -f3)"
            if [ "$ext" != "ok" ]; then status="extension $ext after repack"
            elif [ "$got" != "$want" ]; then status="decoded $got bytes, source $want"
            fi
        fi
        if [ "$status" = "ok" ]; then echo "ok    $label"
        else FAIL=$((FAIL + 1)); FAILURES+=("$label :: $status"); echo "FAIL  $label  ($status)"; fi
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
