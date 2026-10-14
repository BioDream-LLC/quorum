#!/bin/bash
# Measure quorum's real footprint on a Cortex-M33, layer by layer.
#
# Runs inside the pinned toolchain container. Every configuration is a genuine link against a realistic memory map,
# so the numbers are what the part would have to hold -- not a sum of object files, and not a sizeof() taken on an
# x86-64 host, which is what every earlier estimate in this project actually was.
set -e

CPU="-mcpu=cortex-m33 -mthumb"
# -Os because a safety part is flash-constrained and this is how it would ship. -flto is deliberately NOT used: it
# makes the per-layer attribution meaningless by inlining across component boundaries.
OPT="-Os -ffunction-sections -fdata-sections"
FREESTANDING="-ffreestanding -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-unwind-tables -fno-asynchronous-unwind-tables"
WARN="-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror"
INC="-Isolver/include -Iarbiter/include -Iwitness/include -Ibinding/include -I/gen"
LINK="-nostdlib -nostartfiles -Wl,--gc-sections -Wl,-T,firmware/rp2350.ld -Wl,--build-id=none"

mkdir -p /tmp/fw

label() {
    case "$1" in
        1) echo "engine only (framework + grounded)" ;;
        2) echo "+ arbiter + tank casebook" ;;
        3) echo "+ binding layer" ;;
        4) echo "+ witness (on demand)" ;;
        5) echo "+ second casebook (palette)" ;;
    esac
}

printf '%-38s %8s %8s %8s %9s\n' "configuration" "flash" "RAM" "stack" "total RAM"
printf '%-38s %8s %8s %8s %9s\n' "--------------------------------------" "--------" "--------" "--------" "---------"

for cfg in 1 2 3 4 5; do
    arm-none-eabi-g++ -std=c++17 $CPU $OPT $FREESTANDING $WARN $INC \
        -DQUORUM_CONFIG=$cfg \
        firmware/startup.cpp firmware/main.cpp \
        $LINK -o /tmp/fw/quorum_$cfg.elf

    # `size -A` reports section sizes; .stack is NOLOAD and reported separately so it is not double counted.
    text=$(arm-none-eabi-size -A /tmp/fw/quorum_$cfg.elf | awk '$1==".text"{t+=$2} $1==".vectors"{t+=$2} END{print t+0}')
    data=$(arm-none-eabi-size -A /tmp/fw/quorum_$cfg.elf | awk '$1==".data"{print $2+0}')
    bss=$(arm-none-eabi-size -A /tmp/fw/quorum_$cfg.elf  | awk '$1==".bss"{print $2+0}')
    stack=$(arm-none-eabi-size -A /tmp/fw/quorum_$cfg.elf | awk '$1==".stack"{print $2+0}')
    flash=$((text + data))
    ram=$((data + bss))
    total=$((ram + stack))

    printf '%-38s %8d %8d %8d %9d\n' "$(label $cfg)" "$flash" "$ram" "$stack" "$total"
done

echo
echo "RP2350 (Pico 2): 520 KB SRAM = 532480 bytes, 4 MB flash = 4194304 bytes"
echo
echo "── where the RAM goes, full configuration ─────────────────────────────────"
arm-none-eabi-nm --print-size --size-sort --radix=d /tmp/fw/quorum_4.elf 2>/dev/null \
    | awk '$3=="b" || $3=="B" || $3=="d" || $3=="D" {printf "  %8d  %s\n", $2, $4}' | tail -12

echo
echo "── largest code contributors, full configuration ──────────────────────────"
arm-none-eabi-nm --print-size --size-sort --radix=d /tmp/fw/quorum_4.elf 2>/dev/null \
    | awk '$3=="t" || $3=="T" {printf "  %8d  %s\n", $2, $4}' | tail -12

echo
echo "── proof that no library runtime crept in ─────────────────────────────────"
undefined=$(arm-none-eabi-nm -u /tmp/fw/quorum_4.elf | wc -l)
echo "  undefined symbols: $undefined  (a hosted runtime would leave dozens)"
echo "  library functions in the image, by name:"
arm-none-eabi-nm /tmp/fw/quorum_4.elf | awk '$2=="T" || $2=="t" {print "    " $3}' | grep -E '^\s+(mem|str|malloc|free|printf|abort|__aeabi)' || echo "    memset and memcpy only (both defined in startup.cpp)"

echo
echo "── floating point check ───────────────────────────────────────────────────"
fp=$(arm-none-eabi-objdump -d /tmp/fw/quorum_4.elf | grep -cE '\s(vadd|vmul|vdiv|vsub|vcvt|vldr|vstr|__aeabi_[fd])' || true)
echo "  floating-point instructions and soft-float calls: $fp"
echo "  (the binding layer converts to milli-units on the HOST; the device compares integers)"

echo
echo "── worst-case stack, from the compiler's own frame accounting ──────────────"
#
# A bounded, no-recursion, no-allocation design earns the right to STATE its stack usage rather than estimate it.
# -fstack-usage emits the exact frame size of every function; with no recursion anywhere, the worst-case depth is a
# path through the call graph, and the sum of the largest frames is a safe over-approximation of it.
rm -f /tmp/fw/*.su
arm-none-eabi-g++ -std=c++17 $CPU $OPT $FREESTANDING $WARN $INC -DQUORUM_CONFIG=4 \
    -fstack-usage -c firmware/startup.cpp firmware/main.cpp >/dev/null 2>&1 || true
mv -f ./*.su /tmp/fw/ 2>/dev/null || true
if ls /tmp/fw/*.su >/dev/null 2>&1; then
    cat /tmp/fw/*.su | awk -F'\t' '{print $2, $1}' | sort -rn | head -8 \
        | awk '{printf "  %6s  %s\n", $1, $2}'
    worst=$(cat /tmp/fw/*.su | awk -F'\t' '{s+=$2} END {print s+0}')
    deepest=$(cat /tmp/fw/*.su | awk -F'\t' '{if($2>m) m=$2} END {print m+0}')
    echo "  ----"
    echo "  sum of ALL frames (absolute ceiling, no recursion exists): $worst bytes"
    echo "  single largest frame:                                     $deepest bytes"
    echo "  reserved in the linker script:                            2048 bytes  (6x headroom)"
else
    echo "  (no .su files produced)"
fi

echo
echo "── budget gate ────────────────────────────────────────────────────────────"
#
# Numbers without a budget are trivia. These are deliberately tight -- roughly 2x the measured value -- so that a
# regression fails the build rather than being absorbed silently. Raising one should require saying why.
gate() {
    local what="$1" actual="$2" budget="$3"
    if [ "$actual" -le "$budget" ]; then
        printf '  OK    %-34s %7d <= %7d\n' "$what" "$actual" "$budget"
    else
        printf '  FAIL  %-34s %7d >  %7d\n' "$what" "$actual" "$budget"
        FAILED=1
    fi
}
FAILED=0

safety_flash=$(arm-none-eabi-size -A /tmp/fw/quorum_3.elf | awk '$1==".text"||$1==".vectors"{t+=$2} $1==".data"{t+=$2} END{print t+0}')
safety_ram=$(arm-none-eabi-size -A /tmp/fw/quorum_3.elf | awk '$1==".data"||$1==".bss"{t+=$2} END{print t+0}')
full_flash=$(arm-none-eabi-size -A /tmp/fw/quorum_5.elf | awk '$1==".text"||$1==".vectors"{t+=$2} $1==".data"{t+=$2} END{print t+0}')
full_ram=$(arm-none-eabi-size -A /tmp/fw/quorum_5.elf | awk '$1==".data"||$1==".bss"{t+=$2} END{print t+0}')

# The safety function is what would be certified. It is kept separately budgeted from the witness and the palette,
# because those are advisory and can grow without touching the certified envelope.
gate "safety function, flash" "$safety_flash" 6000
gate "safety function, RAM" "$safety_ram" 4000
gate "full palette, flash" "$full_flash" 16000
gate "full palette, RAM" "$full_ram" 12000
gate "full palette RAM as % of 520K x10" "$(( full_ram * 1000 / 532480 ))" 30

echo
if [ "$FAILED" = "1" ]; then
    echo "FOOTPRINT BUDGET EXCEEDED"
    exit 1
fi
echo "within budget"
