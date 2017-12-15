FFMPEG_LIBS=    libavdevice                        \
                libavformat                        \
                libavfilter                        \
                libavcodec                         \
                libswresample                      \
                libswscale                         \
                libavutil                          \

CONFIG_DIR= /home/lee/ffmpeg_build/lib/pkgconfig

CFLAGS := -Wall -Og -g -ggdb
CFLAGS += $(shell export PKG_CONFIG_PATH=$(CONFIG_DIR); pkg-config --cflags $(FFMPEG_LIBS)) $(CFLAGS)
CFLAGS += $(shell freetype-config --cflags) $(CFLAGS)
CFLAGS += $(shell sdl2-config --cflags) $(CFLAGS)

LDLIBS := $(shell export PKG_CONFIG_PATH=$(CONFIG_DIR); pkg-config --libs $(FFMPEG_LIBS)) $(LDLIBS)
LDLIBS += $(shell freetype-config --libs) $(LDLIBS)
LDLIBS += $(shell sdl2-config --libs) $(LDLIBS)

EXAMPLES= tutorial01 \
          tutorial02 \
          tutorial03 \
          tutorial04 \
          tutorial05 \
          tutorial06 \
           
OBJS=$(addsuffix .o,$(EXAMPLES))

all: $(OBJS) $(EXAMPLES)

clean: 
	$(RM) $(EXAMPLES) $(OBJS)

