#!/bin/bash
PCFIX="$HOME/.local/share/e2sar-0.3.2-pkgconfig"
mkdir -p "$PCFIX"

for PCDIR in /usr/local/lib64/pkgconfig /usr/local/lib/pkgconfig; do
  grep -l '/__w/E2SAR/E2SAR/package/usr/local' \
    "$PCDIR"/*.pc 2>/dev/null |
  while read -r PCFILE; do
    cp "$PCFILE" "$PCFIX/"
  done
done

sed -i \
  's|/__w/E2SAR/E2SAR/package/usr/local|/usr/local|g' \
  "$PCFIX"/*.pc
