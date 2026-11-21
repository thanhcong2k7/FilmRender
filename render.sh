#!/usr/bin/env bash
set -euo pipefail

# --- Configuration ---
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MP3_DIR="${SCRIPT_DIR}/ncs"      # Always points to ~/Videos/phim/ncs
SPEED_FACTOR="3.7"               # Video speedup multiplier
OUTPUT_FILENAME="final_output.mp4"

# Temporary working files (stored in /tmp so folders stay clean)
TMP_VIDEO_LIST=$(mktemp /tmp/video_list.XXXXXX.txt)
TMP_AUDIO_LIST=$(mktemp /tmp/audio_list.XXXXXX.txt)
MERGED_AUDIO=$(mktemp /tmp/merged_audio.XXXXXX.mp3)

cleanup() {
  rm -f "$TMP_VIDEO_LIST" "$TMP_AUDIO_LIST" "$MERGED_AUDIO"
}
trap cleanup EXIT

# --- Prompt & Validate ---
echo "==================== Filename Sanitizer ===================="
VIDEO_DIR="${1:-}"
if [[ -z "$VIDEO_DIR" ]]; then
  read -r -p "Film folder (e.g., chitose or phim2): " VIDEO_DIR
fi
VIDEO_DIR="${VIDEO_DIR/#\~/$HOME}"

if [[ ! -d "$VIDEO_DIR" ]]; then
  echo "Error: Film directory '$VIDEO_DIR' not found."
  exit 1
fi

if [[ ! -d "$MP3_DIR" ]]; then
  echo "Error: MP3 directory '$MP3_DIR' not found."
  exit 1
fi

VIDEO_DIR_ABS=$(realpath "$VIDEO_DIR")
OUTPUT_VIDEO="${VIDEO_DIR_ABS}/${OUTPUT_FILENAME}"

# --- 1. Sanitize Video Filenames ---
while IFS= read -r -d '' filepath; do
  # Skip existing final_output.mp4 so it isn't renamed or re-processed
  if [[ "$(realpath "$filepath")" == "$OUTPUT_VIDEO" ]]; then
    continue
  fi

  filename=$(basename "$filepath")
  ext="${filename##*.}"
  base="${filename%.*}"

  # Keep only ASCII a-z, A-Z, 0-9
  clean_base=$(printf '%s' "$base" | LC_ALL=C tr -cd 'a-zA-Z0-9')
  [[ -z "$clean_base" ]] && clean_base="video"

  new_filepath="${VIDEO_DIR_ABS}/${clean_base}.${ext}"

  if [[ "$(realpath "$filepath")" != "$new_filepath" ]]; then
    counter=1
    while [[ -e "$new_filepath" ]]; do
      new_filepath="${VIDEO_DIR_ABS}/${clean_base}_${counter}.${ext}"
      ((counter++))
    done
    mv -v -- "$filepath" "$new_filepath"
  fi
done < <(find "$VIDEO_DIR_ABS" -maxdepth 1 -type f -iname "*.mp4" -print0)

# --- 2. Build Sorted Video Concat List ---
> "$TMP_VIDEO_LIST"
while IFS= read -r filepath; do
  abs_path=$(realpath "$filepath")
  [[ "$abs_path" == "$OUTPUT_VIDEO" ]] && continue

  escaped_path=${abs_path//\'/\'\\\'\'}
  printf "file '%s'\n" "$escaped_path" >> "$TMP_VIDEO_LIST"
done < <(find "$VIDEO_DIR_ABS" -maxdepth 1 -type f -iname "*.mp4" | LC_ALL=C sort -V)

VIDEO_COUNT=$(wc -l < "$TMP_VIDEO_LIST")
if [[ "$VIDEO_COUNT" -eq 0 ]]; then
  echo "Error: No source .mp4 files found in '$VIDEO_DIR'."
  exit 1
fi
echo "Queued $VIDEO_COUNT videos."

# --- 3. Build Randomized Audio Concat List & Merge ---
echo "==================== Media Renderer ===================="
> "$TMP_AUDIO_LIST"
while IFS= read -r filepath; do
  abs_path=$(realpath "$filepath")
  escaped_path=${abs_path//\'/\'\\\'\'}
  printf "file '%s'\n" "$escaped_path" >> "$TMP_AUDIO_LIST"
done < <(find "$MP3_DIR" -maxdepth 1 -type f -iname "*.mp3" | shuf)

AUDIO_COUNT=$(wc -l < "$TMP_AUDIO_LIST")
if [[ "$AUDIO_COUNT" -eq 0 ]]; then
  echo "Error: No .mp3 files found in '$MP3_DIR'."
  exit 1
fi

echo "Merging $AUDIO_COUNT randomized MP3 files..."
ffmpeg -y -hide_banner -loglevel error \
  -f concat -safe 0 -i "$TMP_AUDIO_LIST" \
  -vn -c:a copy "$MERGED_AUDIO"

# --- 4. Concatenate, Speed Up, and Mux ---
TS_MULT=$(LC_ALL=C awk "BEGIN {printf \"%.6f\", 1 / $SPEED_FACTOR}")

#It supposed to be able to re-arrange files here
echo "> CAT VIDEO LIST"
cat $TMP_VIDEO_LIST
read -r -p "Continue? " && [[ "$REPLY" == "yes" ]] && echo "Continuing..."

echo "Rendering ${OUTPUT_VIDEO} at ${SPEED_FACTOR}x speed..."
ffmpeg -y -hide_banner -loglevel warning -stats \
  -f concat -safe 0 -i "$TMP_VIDEO_LIST" \
  -i "$MERGED_AUDIO" \
  -map 0:v:0 -map 1:a:0 \
  -c:v copy -c:a copy \
  -bsf:v "setts=dts=DTS*${TS_MULT}:pts=PTS*${TS_MULT}" \
  -shortest "$OUTPUT_VIDEO"

echo "Done! Saved to: $OUTPUT_VIDEO"
