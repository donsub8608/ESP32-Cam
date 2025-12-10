# MDBT53 Camera Controller CMake Configuration
# 
# 이 프로젝트는 nRF Connect SDK를 사용합니다.
# 
# 빌드 방법:
#   1. nRF Connect SDK 설치
#   2. 프로젝트 디렉토리에서:
#      west build -b nrf52833dk_nrf52833
#   3. 플래시:
#      west flash
#
# 커스텀 보드 사용 시:
#   boards/ 디렉토리에 보드 정의 파일 추가 후
#   west build -b your_custom_board

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(mdbt53_camera_controller)

target_sources(app PRIVATE
    src/main.c
)

