#https://www.youtube.com/playlist?list=PLOVZwvNm10lXTKB9buafMb4G_yt3Ddr3c
read -r -p "Playlist URL: " Y_URL
#https://www.youtube.com/playlist?list=PLOVZwvNm10lXTKB9buafMb4G_yt3Ddr3c
yt-dlp -f "bestvideo[vcodec^=vp9][height<=1080][ext=mp4]/bestvideo[vcodec^=vp9][ext=mp4]" \
  --merge-output-format mp4 \
  --no-overwrites \
  -o "%(playlist_index)02d - %(title)s [%(id)s].%(ext)s" \
  "$Y_URL"
