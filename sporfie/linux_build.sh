# NVIDIA prerequisites for scale_cuda / overlay_cuda (and cuvid/nvenc/nvdec):
#
#  - nv-codec-headers >= 11.1.5.3. Ubuntu's libffmpeg-nvenc-dev ships 11.1.5.1, which configure
#    rejects, so install the headers ourselves. No sudo and no CUDA toolkit needed - they are
#    headers plus a .pc file, and ${HOME}/.local/lib/pkgconfig comes before /usr/lib on
#    pkg-config's path, so this build sees them instead of the distribution's:
#        git clone --depth 1 -b n11.1.5.3 https://github.com/FFmpeg/nv-codec-headers /tmp/nv-codec-headers
#        make -C /tmp/nv-codec-headers install PREFIX="${HOME}/.local"
#    Same tag as NVIDIA_HEADERS_VERSION in Dockerfile.nvidia - keep the two in step so this build
#    and the container build have the same NVENC/CUDA feature set.
#
#  - clang, to compile the filters' CUDA kernels to PTX (--enable-cuda-llvm). We use clang rather
#    than nvcc because --enable-cuda-nvcc is on FFmpeg's nonfree list and would force
#    --enable-nonfree, making the result unredistributable. clang keeps this build GPL.
#        sudo apt-get install clang
#
# A GPU is not needed to build; the kernels ship as PTX and are JIT-compiled at runtime.

export PKG_CONFIG_PATH="${HOME}/.local/lib/pkgconfig:${PKG_CONFIG_PATH}"

make distclean

./configure --prefix=/opt/ffmpeg-sporfie --extra-version=0ubuntu0.22.04.1-sporfie --toolchain=hardened --arch=amd64 --enable-gpl --disable-stripping --enable-gnutls --enable-ladspa --enable-libaom --enable-libass --enable-libbluray --enable-libbs2b --enable-libcaca --enable-libcdio --enable-libcodec2 --enable-libdav1d --enable-libflite --enable-libfontconfig --enable-libfreetype --enable-libfribidi --enable-libgme --enable-libgsm --enable-libjack --enable-libmp3lame --enable-libmysofa --enable-libopenjpeg --enable-libopenmpt --enable-libopus --enable-libpulse --enable-librabbitmq --enable-librubberband --enable-libshine --enable-libsnappy --enable-libsoxr --enable-libspeex --enable-libsrt --enable-libssh --enable-libtheora --enable-libtwolame --enable-libvidstab --enable-libvorbis --enable-libvpx --enable-libwebp --enable-libx265 --enable-libxml2 --enable-libxvid --enable-libzimg --enable-libzmq --enable-libzvbi --enable-lv2 --enable-omx --enable-openal --enable-opencl --enable-opengl --enable-sdl2 --enable-pocketsphinx --enable-librsvg --enable-libmfx --enable-libdc1394 --enable-libdrm --enable-libiec61883 --enable-chromaprint --enable-libx264 --enable-shared --enable-ffnvcodec --enable-cuda --enable-cuda-llvm --enable-cuvid --enable-nvdec --enable-nvenc

rm -f libavutil/ffversion.h
make -j$(nproc)

# Check what we just built, before installing over the previous version. The explicit
# LD_LIBRARY_PATH matters: with the previous version installed, ./ffmpeg resolves libavfilter and
# friends from /opt/ffmpeg-sporfie/lib - same sonames - so without it these checks would report on
# the libraries already installed rather than the ones just built.
build_libs=
for lib in libavfilter libavcodec libavutil libavformat libavdevice libswscale libswresample libpostproc; do
    build_libs="${build_libs}${PWD}/${lib}:"
done
for filter in scale_cuda overlay_cuda imgwatch; do
    LD_LIBRARY_PATH="${build_libs}" ./ffmpeg -hide_banner -filters | grep -qE "\b${filter}\b" \
        || { echo "${filter} missing from this build - not installing" >&2; exit 1; }
done

sudo make install

echo '/opt/ffmpeg-sporfie/lib' | sudo tee /etc/ld.so.conf.d/ffmpeg-sporfie.conf
sudo ldconfig
