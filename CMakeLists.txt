# ##############################################################################
# frameworks/multimedia/ai/CMakeLists.txt
#
# Licensed to the Apache Software Foundation (ASF) under one or more contributor
# license agreements.  See the NOTICE file distributed with this work for
# additional information regarding copyright ownership.  The ASF licenses this
# file to you under the Apache License, Version 2.0 (the "License"); you may not
# use this file except in compliance with the License.  You may obtain a copy of
# the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
# License for the specific language governing permissions and limitations under
# the License.
#
# ##############################################################################

if(CONFIG_AI_MODULE)

  # ############################################################################
  # Include Directory
  # ############################################################################

  set(INCDIR ${CMAKE_CURRENT_LIST_DIR}/utils ${CMAKE_CURRENT_LIST_DIR}/include ${CMAKE_CURRENT_LIST_DIR}/src/asr/plugin ${CMAKE_CURRENT_LIST_DIR}/src/tts/plugin)
  list(APPEND INCDIR ${NUTTX_APPS_DIR}/vendor/xiaomi/miwear/apps/frameworks/include/media_session)
  list(APPEND INCDIR ${NUTTX_APPS_DIR}/vendor/xiaomi/miwear/apps/frameworks/include/data_proxy)
  list(APPEND INCDIR ${NUTTX_APPS_DIR}/vendor/xiaomi/miwear/common/pb)
  list(APPEND INCDIR ${NUTTX_APPS_DIR}/netutils/libwebsockets)
  list(APPEND INCDIR ${NUTTX_APPS_DIR}/system/libarchive/libarchive/libarchive)
  list(APPEND INCDIR ${NUTTX_APPS_DIR}/external/json-c)
  list(APPEND INCDIR ${NUTTX_APPS_DIR}/external/json-c/json-c)

  # ############################################################################
  # Sources
  # ############################################################################

  set(CSRCS
      ${CMAKE_CURRENT_SOURCE_DIR}/src/asr/ai_asr.c
      ${CMAKE_CURRENT_SOURCE_DIR}/src/asr/plugin/ai_voice_plugin.c
      ${CMAKE_CURRENT_SOURCE_DIR}/src/asr/xiaoai/ai_xiaoai.c
      ${CMAKE_CURRENT_SOURCE_DIR}/src/asr/volc/ai_volc.c
      ${CMAKE_CURRENT_SOURCE_DIR}/utils/ai_ring_buffer.c
      ${CMAKE_CURRENT_SOURCE_DIR}/src/tts/ai_tts.c
      ${CMAKE_CURRENT_SOURCE_DIR}/src/tts/plugin/ai_tts_plugin.c
      ${CMAKE_CURRENT_SOURCE_DIR}/src/tts/volc/ai_volc_tts.c
  )
  message("cmake src list:${CSRCS}")

  if(CONFIG_AI_TOOL)
    nuttx_add_application(
      NAME
      aitool
      SRCS
      ai_tool.c
      STACKSIZE
      ${CONFIG_AI_TOOL_STACKSIZE}
      PRIORITY
      ${CONFIG_AI_TOOL_PRIORITY}
      INCLUDE_DIRECTORIES
      ${INCDIR})
  endif()

  # ############################################################################
  # Library Configuration
  # ############################################################################

  nuttx_add_library(libai STATIC)
  target_sources(libai PRIVATE ${CSRCS})
  target_include_directories(libai PRIVATE ${INCDIR})

  set_property(
    TARGET nuttx
    APPEND
    PROPERTY NUTTX_INCLUDE_DIRECTORIES ${CMAKE_CURRENT_LIST_DIR}/include)

endif()

