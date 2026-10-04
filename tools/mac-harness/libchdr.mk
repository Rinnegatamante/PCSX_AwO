# Use the libchdr revision already pinned in .gitmodules/the gitlink.
# Scalar LZMA is portable on ARM64; GPU_NEON remains the native SIMD renderer.
LIBCHDR := deps/libchdr
LIBCHDR_LZMA := $(LIBCHDR)/deps/lzma-26.02
LIBCHDR_ZSTD := $(LIBCHDR)/deps/zstd-1.5.7
ifeq ($(wildcard $(LIBCHDR)/src/libchdr_chd.c),)
$(error libchdr is missing; run git submodule update --init deps/libchdr, or use HAVE_CHD=0)
endif
LIBCHDR_CFLAGS := -Iinclude -I$(LIBCHDR)/include -I$(LIBCHDR_LZMA)/include -DCHDR_SYSTEM_ZLIB
LIBCHDR_SOURCES := $(addprefix $(LIBCHDR)/src/libchdr_,\
  $(addsuffix .c,cdrom chd codec_cdfl codec_cdlz codec_cdzl codec_cdzs \
  codec_flac codec_huff codec_lzma codec_zlib codec_zstd flac huffman)) \
  $(LIBCHDR_LZMA)/src/LzmaDec.c $(LIBCHDR_ZSTD)/zstddeclib.c
LIBCHDR_EXTRA_OBJECTS := $(BUILD)/libchdr_bitstream.o
# Keep the upstream checkout pristine. Apply the two unsigned-shift fixes to
# a generated build-local copy; assertions fail if the pinned source changes.
$(BUILD)/libchdr_bitstream.c: $(LIBCHDR)/src/libchdr_bitstream.c scripts/patch_libchdr_bitstream.py
	@mkdir -p $(dir $@)
	python3 scripts/patch_libchdr_bitstream.py $< $@
$(BUILD)/libchdr_bitstream.o: $(BUILD)/libchdr_bitstream.c
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@
