############################################################################
# frameworks/ai/Makefile
#
# Licensed to the Apache Software Foundation (ASF) under one or more
# contributor license agreements.  See the NOTICE file distributed with
# this work for additional information regarding copyright ownership.  The
# ASF licenses this file to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance with the
# License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
# License for the specific language governing permissions and limitations
# under the License.
#
############################################################################

include $(APPDIR)/Make.defs

MODULE  = y
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/frameworks/multimedia/ai/utils
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/frameworks/multimedia/ai/fonts
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/frameworks/multimedia/ai/src/asr/plugin
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/frameworks/multimedia/ai/src/tts/plugin
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/vendor/xiaomi/miwear/apps/frameworks/include/media_session
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/vendor/xiaomi/miwear/apps/frameworks/include/data_proxy
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/vendor/xiaomi/miwear/common/pb
# CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/vendor/xiaomi/miwear/common/pb/include_sensor
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/netutils/libwebsockets
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/system/libarchive/libarchive/libarchive
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/external/json-c
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/external/json-c/json-c

ifneq ($(CONFIG_AI_TOOL),)
  MAINSRC   += ai_tool.c
  PROGNAME  += aitool
  PRIORITY  += $(CONFIG_AI_TOOL_PRIORITY)
  STACKSIZE += $(CONFIG_AI_TOOL_STACKSIZE)
endif

ifneq ($(CONFIG_AI_LVGL),)
  MAINSRC   += ai_lvgl.c
  PROGNAME  += ailvgl
  PRIORITY  += $(CONFIG_AI_LVGL_PRIORITY)
  STACKSIZE += $(CONFIG_AI_LVGL_STACKSIZE)
endif

ifneq ($(CONFIG_AI_MODULE),)
CSRCS += $(wildcard src/asr/*.c)
CSRCS += $(wildcard src/tts/*.c)
CSRCS += $(wildcard src/asr/plugin/*.c)
CSRCS += $(wildcard src/tts/plugin/*.c)
CSRCS += $(wildcard src/asr/volc/*.c)
CSRCS += $(wildcard src/asr/xiaoai/*.c)
CSRCS += $(wildcard src/tts/volc/*.c)
CSRCS += $(wildcard utils/*.c)
CSRCS += $(wildcard fonts/*.c)
endif

# ASRCS := $(wildcard $(ASRCS))
CSRCS := $(wildcard $(CSRCS))
# CXXSRCS := $(wildcard $(CXXSRCS))
MAINSRC := $(wildcard $(MAINSRC))
NOEXPORTSRCS = $(ASRCS)$(CSRCS)$(CXXSRCS)$(MAINSRC)

ifneq ($(NOEXPORTSRCS),)
BIN := $(APPDIR)/staging/libai.a
endif

EXPORT_FILES := include

include $(APPDIR)/Application.mk
