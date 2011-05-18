
LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_ARM_MODE := arm
ifeq ($(BUILD_WITH_NEON),1)
LOCAL_ARM_NEON := true
endif

LOCAL_MODULE := liblibplaylist_plugin

LOCAL_CFLAGS += \
    -std=c99 \
    -DHAVE_CONFIG_H \
    -DMODULE_STRING=\"libplaylist\" \
    -DMODULE_NAME=libplaylist

LOCAL_C_INCLUDES += \
    $(VLCROOT)/compat \
    $(VLCROOT) \
    $(VLCROOT)/include

LOCAL_SRC_FILES := \
    asx.c \
    b4s.c \
    dvb.c \
    gvp.c \
    ifo.c \
    itml.c \
    m3u.c \
    playlist.c \
    pls.c \
    podcast.c \
    qtl.c \
    ram.c \
    sgimb.c \
    shoutcast.c \
    wpl.c \
    xspf.c \
    zpl.c

include $(BUILD_STATIC_LIBRARY)

