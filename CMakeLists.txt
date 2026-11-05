cmake_minimum_required(VERSION 3.10)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

project(VitaFin C)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "VitaFin")
set(VITA_TITLEID  "VFIN00001")
set(VITA_VERSION  "01.00")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -O2 -std=gnu11")
# Large heap: curl + openssl + thumbnails + AvPlayer buffers.
set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d ATTRIBUTE2=12")

add_executable(${PROJECT_NAME}
  src/main.c
  src/ui.c
  src/input.c
  src/http.c
  src/json.c
  src/jellyfin.c
  src/images.c
  src/player.c
  src/config.c
  src/ime.c
  src/screens.c
  src/media.c
  src/font.c
  src/log.c
  src/sfx.c
  src/subs.c
)

target_link_libraries(${PROJECT_NAME}
  vita2d
  curl ssl crypto zstd bz2
  freetype png jpeg z m
  pthread
  SceVideodec_stub
  SceAudiodec_stub
  SceAudio_stub
  SceAppMgr_stub
  SceAppUtil_stub
  SceCommonDialog_stub
  SceCtrl_stub
  SceDisplay_stub
  SceGxm_stub
  SceNet_stub
  SceNetCtl_stub
  ScePgf_stub
  ScePvf_stub
  ScePower_stub
  SceRtc_stub
  SceSysmodule_stub
  SceTouch_stub
  SceLibKernel_stub
)

vita_create_self(${PROJECT_NAME}.self ${PROJECT_NAME} UNSAFE)
vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} ${PROJECT_NAME}.self
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE sce_sys/icon0.png sce_sys/icon0.png
  FILE sce_sys/pic0.png sce_sys/pic0.png
  FILE sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
  FILE sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
  FILE sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
)
