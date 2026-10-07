#!/bin/sh
#
# Fetch the music the MP3 encoder's quality gate is measured on.
#
# It is somebody else's recordings and is not committed: twelve clips from
# Wikimedia Commons, each public domain, CC0, CC BY or CC BY-SA, listed with
# licence, artist and URL in tools/corpus/MANIFEST. They are read and decoded
# here and never redistributed. `make test` never needs the network; the
# quality gate that uses them reports a missing corpus and says how to fetch
# it, and fails instead of skipping when GHOTI_CORPUS_REQUIRED=1.
#
# Only a prefix of each FLAC is fetched (MANIFEST gives its length), and the
# SHA-256 is of those bytes, so a changed upload or a truncated transfer fails
# here rather than moving a score. Commons answers a burst of requests with
# 429, so the requests are spaced and a 429 is waited out.
#
# Usage:  tools/corpus/fetch.sh
#
# Copyright 2026 by Corey Pennycuff

set -eu

root=$(cd "$(dirname "$0")/../.." && pwd)
dest="$root/third_party/mp3corpus"
mkdir -p "$dest"
ua='ghoti-audio-corpus/0.1 (https://github.com/Ghoti-io)'

grep -v '^#' "$root/tools/corpus/MANIFEST" | while IFS="	" read -r tag bytes sha licence artist url; do
  out="$dest/$tag.flac"
  if [ -s "$out" ] && [ "$(sha256sum "$out" | cut -d' ' -f1)" = "$sha" ]; then
    printf 'have    %s\n' "$tag"
    continue
  fi
  printf 'fetch   %s (%s, %s)\n' "$tag" "$licence" "$artist"
  tries=0
  until curl --fail --silent --show-error --location --user-agent "$ua" \
      --range "0-$((bytes - 1))" --output "$out.partial" "$url"; do
    tries=$((tries + 1))
    [ "$tries" -lt 8 ] || { echo "giving up on $tag" >&2; exit 1; }
    sleep 30
  done
  got=$(sha256sum "$out.partial" | cut -d' ' -f1)
  if [ "$got" != "$sha" ]; then
    rm -f "$out.partial"
    echo "$tag: sha256 $got, expected $sha" >&2
    exit 1
  fi
  mv "$out.partial" "$out"
  sleep 8
done
