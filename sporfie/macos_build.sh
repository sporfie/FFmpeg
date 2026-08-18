# 1. Force the compiler to look at Homebrew FIRST for headers
export CPPFLAGS="-I/opt/homebrew/include -I/opt/homebrew/opt/freetype/include/freetype2 -I/opt/homebrew/opt/libpng/include/libpng16"

# 2. Force the linker to look at Homebrew FIRST for libraries
export LDFLAGS="-L/opt/homebrew/lib -L/opt/homebrew/opt/openssl@3/lib"

# 3. Make sure pkg-config doesn't even look at the GStreamer folder
export PKG_CONFIG_PATH="/opt/homebrew/lib/pkgconfig:/opt/homebrew/opt/openssl@3/lib/pkgconfig:/opt/homebrew/opt/libsrt/lib/pkgconfig"

./configure \
  --enable-libsrt \
  --enable-libfreetype \
  --enable-fontconfig \
  --enable-gpl \
  --enable-nonfree \
  --enable-filter=drawtext \
  --enable-libharfbuzz \
  --disable-bzlib \
  --enable-libx264 \
  --enable-libx265 \
  --enable-openssl \
  --enable-videotoolbox \
  --enable-audiotoolbox \
  --extra-cflags="$CPPFLAGS" \
  --extra-ldflags="$LDFLAGS"
  
make -j$(nproc)

install_name_tool -add_rpath /usr/lib /Users/guy/Development/Sporfie/ffmpeg_sporfie/ffmpeg
install_name_tool -add_rpath /usr/lib /Users/guy/Development/Sporfie/ffmpeg_sporfie/ffprobe
