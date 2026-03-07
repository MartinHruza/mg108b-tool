#!/bin/bash
set -euo pipefail

API_BASE="https://api2.rongyuan.tech:3816"
DEV_ID=969

echo "Checking firmware for MG108B (dev_id=$DEV_ID)..."
resp=$(curl -s -X POST "$API_BASE/api/v2/get_fw_version" \
    -H "Content-Type: application/json" \
    -d "{\"dev_id\": $DEV_ID}")

file_path=$(echo "$resp" | jq -r '.data.file_path // empty')
version=$(echo "$resp" | jq -r '.data.version_str // empty')

if [ -z "$file_path" ]; then
    echo "Error: no firmware found. API response:"
    echo "$resp" | jq .
    exit 1
fi

echo "Latest firmware: $version"
echo "File path: $file_path"

outfile="mg108b_firmware_${version}.bin"
if [ -f "$outfile" ]; then
    echo "Already downloaded: $outfile"
    exit 0
fi

echo "Downloading..."
curl -s -o "$outfile" "$API_BASE/download/$file_path"
size=$(stat -c%s "$outfile")
echo "Saved $outfile ($size bytes)"

rawfile="mg108b_firmware_${version}.raw"
echo "Decompressing (raw deflate)..."
printf '\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\x00' | cat - "$outfile" | gzip -dc > "$rawfile" 2>/dev/null || true

if [ -f "$rawfile" ] && [ "$(stat -c%s "$rawfile")" -gt 0 ]; then
    rawsize=$(stat -c%s "$rawfile")
    echo "Saved $rawfile ($rawsize bytes)"
else
    rm -f "$rawfile"
    echo "Warning: decompression failed. The .bin file may not be raw-deflate compressed."
    echo "You may need to inspect and decompress it manually."
fi
