#!/bin/bash
# Builds the ffmpeg and ffprobe that KAVL.app carries: LGPL only (no --enable-gpl, no nonfree), with just what KAVL runs:
# camera and microphone (AVFoundation), the test pattern, the usual video files in, AV1 (SVT-AV1) and Opus out, IVF / Ogg / Matroska.
# FFmpeg's own libraries are linked in statically; libSvtAv1Enc and libdav1d stay dylibs and are bundled next to it (tools/bundle_app.sh).
# Usage: tools/build_ffmpeg.sh /path/to/ffmpeg-8.0-source   (a clean, unconfigured tree from ffmpeg.org)
# Output: build-ffmpeg/out/bin/{ffmpeg,ffprobe}
set -euo pipefail
SRC="${1:?usage: tools/build_ffmpeg.sh /path/to/ffmpeg-source}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
B="$ROOT/build-ffmpeg"
BREW="$(brew --prefix)"
mkdir -p "$B/obj" "$B/out"
# work on a copy; SVT-AV1 4 renamed enable_adaptive_quantization to aq_mode (FFmpeg 8.0 still uses the old name)
rsync -a --delete --exclude .git "$SRC/" "$B/src/"
SRC="$B/src"
if grep -q "uint8_t aq_mode" "$BREW/include/svt-av1/EbSvtAv1Enc.h"; then
  sed -i '' 's/param->enable_adaptive_quantization = 0;/param->aq_mode = 0;/' "$SRC/libavcodec/libsvtav1.c"
fi
cd "$B/obj"
export PKG_CONFIG_PATH="$BREW/lib/pkgconfig"
DEC=wrapped_avframe,pcm_f32be,pcm_f64le,pcm_s24be,pcm_s32be,h264,hevc,vp8,vp9,av1,libdav1d,mpeg4,mpeg2video,mpeg1video,mjpeg,prores,h263,theora,png,rawvideo,aac,aac_latm,mp3,mp3float,mp2,opus,vorbis,flac,alac,ac3,eac3,pcm_s16le,pcm_s16be,pcm_s24le,pcm_s32le,pcm_f32le,pcm_u8
DEMUX=mov,matroska,ivf,ogg,avi,mpegts,mpegps,flv,mp3,aac,wav,flac,h264,hevc,obu,image2,avfoundation
"$SRC/configure" --prefix="$B/out" \
  --disable-shared --enable-static --disable-debug --disable-doc --disable-ffplay --disable-network --disable-autodetect \
  --disable-everything --enable-pthreads --enable-zlib \
  --enable-avfoundation --enable-videotoolbox --enable-libsvtav1 --enable-libopus --enable-libdav1d \
  --enable-indev=avfoundation,lavfi \
  --enable-protocol=file,pipe \
  --enable-demuxer=$DEMUX \
  --enable-muxer=ivf,ogg,matroska,null,rawvideo \
  --enable-decoder=$DEC \
  --enable-encoder=libsvtav1,libopus,rawvideo,wrapped_avframe \
  --enable-parser=h264,hevc,vp8,vp9,av1,mpeg4video,mpegvideo,mjpeg,aac,aac_latm,mpegaudio,opus,vorbis,flac,ac3 \
  --enable-bsfs \
  --enable-hwaccel=h264_videotoolbox,hevc_videotoolbox,vp9_videotoolbox,prores_videotoolbox,mpeg4_videotoolbox,mpeg2_videotoolbox \
  --enable-filter=scale,pad,fps,format,aformat,aresample,null,anull,testsrc2,sine,setpts,asetpts,copy,acopy,crop,transpose,hflip,vflip,rotate,yadif,bwdif \
  --extra-cflags="-I$BREW/include" --extra-ldflags="-L$BREW/lib" --pkg-config-flags=--static > "$B/configure.log" 2>&1 \
  || { tail -20 "$B/configure.log"; exit 1; }
grep -q "#define CONFIG_GPL 0" config.h || { echo "GPL parts got enabled"; exit 1; }
grep -q "#define CONFIG_NONFREE 0" config.h || { echo "nonfree parts got enabled"; exit 1; }
nice -n 19 make -j3 > "$B/make.log" 2>&1 || { grep -E "error" "$B/make.log" | head -20; exit 1; }
make install > /dev/null
"$B/out/bin/ffmpeg" -hide_banner -L | head -3
echo "built: $B/out/bin/ffmpeg"
