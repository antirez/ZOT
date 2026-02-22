#!/bin/bash
#
# download.sh -- Download classic CP/M 2.2 software
#
# Downloads famous CP/M programs from public archives into this directory.
# Each program gets its own subdirectory. Run cpmcon with any of them:
#
#   ../cpmcon wordstar/
#   ../cpmcon -A turbo-pascal/ -B mbasic/
#
# Sources:
#   github.com/skx/cpm-dist      -- Direct .COM files (primary)
#   github.com/MockbaTheBorg/RunCPM -- CP/M 2.2 standard utilities
#   deramp.com                    -- Microsoft language tools

set -e
cd "$(dirname "$0")"

SKX="https://raw.githubusercontent.com/skx/cpm-dist/master"

# --- Copyright warning ---

cat <<'NOTICE'

  ============================================================
  WARNING: COPYRIGHTED SOFTWARE
  ============================================================

  This script downloads classic CP/M software from public
  archives. These programs are copyrighted by their original
  authors and publishers:

    WordStar 3.3        MicroPro International
    MBASIC 5.29         Microsoft Corporation
    Turbo Pascal 3.00A  Borland International
    Zork I, II, III     Infocom / Activision
    Hitchhiker's Guide  Infocom / Activision
    FORTRAN-80          Microsoft Corporation
    CP/M 2.2 Utilities  Digital Research Inc.

  These downloads are intended for personal, non-commercial,
  educational, and historical preservation purposes only.
  Some of these may be considered abandonware but none have
  been officially released as freeware by their rights holders.

  ============================================================

NOTICE

read -p "  Do you accept responsibility for this download? [y/N] " answer
echo
if [ "$answer" != "y" ] && [ "$answer" != "Y" ]; then
    echo "  Aborted."
    exit 0
fi

# --- Helper ---

download() {
    local url="$1"
    local dest="$2"
    local dir
    dir=$(dirname "$dest")
    mkdir -p "$dir"
    if [ -f "$dest" ]; then
        echo "  [skip] $dest (already exists)"
    else
        echo "  [get]  $dest"
        curl -sL -o "$dest" "$url"
        if [ ! -s "$dest" ]; then
            echo "  [FAIL] $dest (empty or download failed)"
            rm -f "$dest"
            return 1
        fi
    fi
    return 0
}

# ============================================================
# CP/M 2.2 Standard Utilities (Digital Research)
# ============================================================

echo "=== CP/M 2.2 Utilities ==="
echo "  Source: RunCPM (github.com/MockbaTheBorg/RunCPM)"

RUNCPM_ZIP="https://raw.githubusercontent.com/MockbaTheBorg/RunCPM/master/DISK/A0.ZIP"
mkdir -p utils
if [ ! -f utils/PIP.COM ]; then
    echo "  [get]  A0.ZIP (standard utilities pack)"
    curl -sL -o /tmp/cpm_a0.zip "$RUNCPM_ZIP"
    # Extract .COM files from the A/0/ subdirectory in the ZIP
    cd utils
    unzip -ojq /tmp/cpm_a0.zip 'A/0/*.COM' 2>/dev/null || \
    unzip -ojq /tmp/cpm_a0.zip '*/0/*.COM' 2>/dev/null || \
    unzip -ojq /tmp/cpm_a0.zip '*.COM' 2>/dev/null || true
    cd ..
    rm -f /tmp/cpm_a0.zip
    echo "  Extracted to utils/"
else
    echo "  [skip] utils/ (already exists)"
fi
echo

# ============================================================
# WordStar 3.3 (MicroPro)
# ============================================================

echo "=== WordStar 3.3 ==="
echo "  The legendary word processor. Type WS to start."
download "$SKX/E/WS.COM"      "wordstar/WS.COM"
download "$SKX/E/WSOVLY1.OVR" "wordstar/WSOVLY1.OVR"
download "$SKX/E/WSMSGS.OVR"  "wordstar/WSMSGS.OVR"
echo

# ============================================================
# MBASIC 5.29 (Microsoft)
# ============================================================

echo "=== MBASIC 5.29 (Microsoft BASIC) ==="
echo "  Microsoft BASIC interpreter. Type MBASIC to start."
download "$SKX/B/MBASIC.COM" "mbasic/MBASIC.COM"
echo

# ============================================================
# Turbo Pascal 3.00A (Borland)
# ============================================================

echo "=== Turbo Pascal 3.00A ==="
echo "  Borland's revolutionary IDE+compiler. Type TURBO to start."
download "$SKX/P/TURBO.COM"    "turbo-pascal/TURBO.COM"
download "$SKX/P/TURBO.OVR"    "turbo-pascal/TURBO.OVR"
download "$SKX/P/TURBO.MSG"    "turbo-pascal/TURBO.MSG"
# Terminal installation utility
download "$SKX/P/TINST.COM"    "turbo-pascal/TINST.COM" 2>/dev/null || true
download "$SKX/P/TINST.DTA"    "turbo-pascal/TINST.DTA" 2>/dev/null || true
download "$SKX/P/TINST.MSG"    "turbo-pascal/TINST.MSG" 2>/dev/null || true
echo

# ============================================================
# Zork I, II, III + Hitchhiker's Guide (Infocom)
# ============================================================

echo "=== Infocom Text Adventures ==="
echo "  Interactive fiction classics. Type ZORK1, ZORK2, ZORK3, or HHGG."

download "$SKX/G/ZORK1.COM"    "games/ZORK1.COM"
download "$SKX/G/ZORK1.DAT"    "games/ZORK1.DAT"

download "$SKX/G/ZORK2.COM"    "games/ZORK2.COM"
download "$SKX/G/ZORK2.DAT"    "games/ZORK2.DAT"

download "$SKX/G/ZORK3.COM"    "games/ZORK3.COM"
download "$SKX/G/ZORK3.DAT"    "games/ZORK3.DAT"

download "$SKX/G/HITCH.COM"    "games/HHGG.COM"
download "$SKX/G/HITCHHIK.DAT" "games/HITCHHIK.DAT"
echo

# ============================================================
# Microsoft FORTRAN-80 (v3.44)
# ============================================================

echo "=== Microsoft FORTRAN-80 ==="
echo "  F80 compiler + M80 assembler + L80 linker."

DERAMP="https://deramp.com/downloads/microsoft/CPM%20Software/FORTRAN-80%20(F80%20v3.44)"
download "$DERAMP/F80.COM"     "fortran/F80.COM"
download "$DERAMP/L80.COM"     "fortran/L80.COM"
download "$DERAMP/M80.COM"     "fortran/M80.COM"
download "$DERAMP/FORLIB.REL"  "fortran/FORLIB.REL"
echo

# ============================================================
# BBC BASIC (Z80) -- R.T. Russell (freely distributed)
# ============================================================

echo "=== BBC BASIC (Z80) ==="
echo "  R.T. Russell's BBC BASIC for CP/M. Type BBCBASIC to start."
download "$SKX/B/BBCBASIC.COM" "bbcbasic/BBCBASIC.COM" 2>/dev/null || true
echo

# ============================================================
# Summary
# ============================================================

echo "============================================================"
echo "Download complete! Directory layout:"
echo
echo "  utils/          CP/M utilities (STAT, PIP, DDT, ED, ASM...)"
echo "  wordstar/       WordStar 3.3 word processor"
echo "  mbasic/         Microsoft BASIC 5.29"
echo "  turbo-pascal/   Borland Turbo Pascal 3.00A"
echo "  games/          Zork I/II/III + Hitchhiker's Guide"
echo "  fortran/        Microsoft FORTRAN-80 compiler"
echo "  bbcbasic/       BBC BASIC (Z80)"
echo
echo "Quick start:"
echo "  cd .."
echo "  ./cpmcon cpm-software/mbasic/"
echo "  A>MBASIC"
echo
echo "Mount multiple drives:"
echo "  ./cpmcon -A cpm-software/utils/ -B cpm-software/games/"
echo "  A>B:"
echo "  B>ZORK1"
echo "============================================================"
