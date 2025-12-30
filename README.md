# ESP32-CAM → 라즈베리파이 사진 전송 시스템 정리

## 📁 프로젝트 구조

```
Camera/
├── CameraServerTest/
│   ├── Arduino/
│   │   ├── CameraServerTest/          # ESP32-CAM 카메라 모듈
│   │   │   ├── CameraServerTest.ino   # ESP32-CAM 메인 코드 (현재 오디오 스트리머 코드가 들어있음 - 수정 필요)
│   │   │   ├── board_config.h         # 카메라 보드 설정 (AI Thinker ESP32-CAM)
│   │   │   ├── camera_index.h         # 웹 인터페이스 HTML (압축)
│   │   │   ├── camera_pins.h          # 카메라 GPIO 핀 정의
│   │   │   ├── partitions.csv         # ESP32 플래시 파티션 스키마
│   │   │   └── ci.yml                 # CI/CD 빌드 설정
│   │   └── ESP32C3_AudioStream/       # ESP32-C3 오디오 스트리밍 (별도 프로젝트)
│   │       └── ESP32C3_AudioStream.ino
│   └── Raspberry/
│       └── rpi_server.py               # 라즈베리파이 수신 서버 (핵심)
```

---

## 🎯 시스템 개요

**ESP32-CAM으로 촬영한 사진을 라즈베리파이로 전송하는 시스템**

```
ESP32-CAM (카메라 모듈)
    ↓ [HTTP POST /upload]
라즈베리파이 서버 (rpi_server.py)
    ↓ [로컬 저장: ./esp32_photos/]
    ↓ [60초마다 자동 전송]
원격 서버 (118.42.62.78:6000)
```

---

## 1. 📷 ESP32-CAM 카메라 모듈 (CameraServerTest)

### 위치
`CameraServerTest/Arduino/CameraServerTest/`

### 역할
- 카메라로 사진 촬영
- WiFi를 통해 라즈베리파이 서버로 HTTP POST 전송
- 라즈베리파이 IP: `rpiServerIP` (코드에서 설정 필요)
- 라즈베리파이 포트: `rpiServerPort` (기본 5000)

### 주요 파일

#### `CameraServerTest.ino`
- **⚠️ 현재 상태**: 파일 내용이 ESP32-C3 블루투스 오디오 스트리머로 되어 있음
- **필요한 기능**: 
  - ESP32-CAM 카메라 초기화
  - 사진 촬영 (JPEG)
  - HTTP POST로 라즈베리파이 서버(`http://[라즈베리파이IP]:5000/upload`)로 전송
- **참고**: 실제 ESP32-CAM 코드로 교체 필요

#### `board_config.h`
- **기능**: 카메라 모델 선택 및 보드 설정
- **현재 설정**: `CAMERA_MODEL_AI_THINKER` (AI Thinker ESP32-CAM 모듈)
- **지원 보드**: 
  - ESP32 Wrover Kit
  - ESP-EYE
  - M5Stack 시리즈
  - AI Thinker (현재 활성화)
  - 기타 ESP32 카메라 보드들

#### `camera_pins.h`
- **기능**: 각 카메라 모델별 GPIO 핀 정의
- **포함 내용**:
  - PWDN, RESET, XCLK 핀
  - I2C 통신 핀 (SIOD, SIOC)
  - 데이터 핀 (Y2~Y9)
  - 동기화 핀 (VSYNC, HREF, PCLK)
  - LED 핀

#### `camera_index.h`
- **기능**: 웹 인터페이스 HTML 파일 (gzip 압축된 바이너리 데이터)
- **크기**: 약 6.7KB (압축 전)
- **용도**: ESP32-CAM 웹 서버의 프론트엔드 UI

#### `partitions.csv`
- **기능**: ESP32 플래시 메모리 파티션 스키마
- **파티션 구성**:
  - `nvs`: 20KB (비휘발성 저장소)
  - `otadata`: 8KB (OTA 데이터)
  - `app0`: 3.75MB (애플리케이션)
  - `fr`: 128KB (파일 시스템)
  - `coredump`: 64KB (코어 덤프)

#### `ci.yml`
- **기능**: CI/CD 파이프라인 설정
- **지원 보드**: ESP32, ESP32-S2, ESP32-S3
- **옵션**: PSRAM 활성화/비활성화, 파티션 스키마, 플래시 모드

---

## 2. 🎤 ESP32-C3 오디오 스트리머 (ESP32C3_AudioStream)

### 위치
`CameraServerTest/Arduino/ESP32C3_AudioStream/ESP32C3_AudioStream.ino`

### 기능
ESP32-C3 보드에서 GPIO 2번 핀의 ADC 입력을 블루투스로 스트리밍

### 주요 특징
- **입력**: GPIO 2번 핀 (ADC)
- **출력**: 블루투스 A2DP 스트리밍
- **샘플링 레이트**: 16kHz
- **비트 깊이**: 16비트
- **버퍼 크기**: 512 샘플
- **ADC 해상도**: 12비트 (0-4095)
- **블루투스 장치명**: "ESP32-C3-Audio"

### 동작 방식
1. GPIO 2번 핀에서 ADC로 오디오 신호 읽기
2. 12비트 ADC 값을 16비트 오디오 샘플로 변환
3. 버퍼에 저장 (512 샘플)
4. 버퍼가 가득 차면 블루투스로 전송
5. PC/스마트폰에서 "ESP32-C3-Audio" 장치로 페어링하여 수신

### 사용 방법
1. Arduino IDE에서 ESP32 보드 매니저 설치
2. 보드: ESP32C3 Dev Module 선택
3. 업로드 후 시리얼 모니터 확인
4. PC에서 "ESP32-C3-Audio" 블루투스 장치 찾아 페어링

---

## 2. 🌐 라즈베리파이 수신 서버 (rpi_server.py) ⭐ 핵심

### 위치
`CameraServerTest/Raspberry/rpi_server.py`

### 역할
ESP32-CAM에서 촬영한 사진을 수신하여 저장하고, 원격 서버로 자동 전송하는 중계 서버

### 주요 기능
1. **사진 수신**: ESP32-CAM에서 HTTP POST로 전송된 사진 수신
2. **로컬 저장**: `./esp32_photos/` 디렉토리에 JPEG 파일로 저장
3. **원격 전송**: 60초마다 새로운 사진을 원격 서버로 자동 전송
4. **중복 방지**: 전송한 파일은 `sent_files.json`에 기록하여 중복 전송 방지

### 주요 특징

### 수신 엔드포인트

#### `POST /upload` - 사진 수신
- **요청 형식**: multipart/form-data
- **파일 필드명**: `file`
- **응답 형식**: JSON
  ```json
  {
    "success": true,
    "filename": "photo_20240101_120000.jpg",
    "size": 123456,
    "path": "/path/to/esp32_photos/photo_20240101_120000.jpg"
  }
  ```
- **저장 위치**: 기본 `./esp32_photos` (명령줄 옵션으로 변경 가능)
- **파일명**: 원본 파일명 + 타임스탬프로 고유 파일명 생성

### 원격 서버 전송
- **대상 서버**: `http://118.42.62.78:6000/upload`
- **전송 주기**: 60초마다 자동으로 새 파일 검사 및 전송
- **중복 방지**: `sent_files.json`에 전송 완료 파일명 기록
- **백그라운드 실행**: 별도 스레드에서 주기적 실행
- **에러 처리**: 연결 실패 시 다음 주기에 재시도

### API 엔드포인트

| 메서드 | 경로 | 설명 |
|--------|------|------|
| `GET /` | 상태 페이지 | 웹 인터페이스 (통계, 설정 정보) |
| `POST /upload` | 파일 업로드 | ESP32-CAM에서 사진 전송 |
| `GET /list` | 파일 목록 | 수신된 파일 목록 (JSON) |
| `GET /health` | 서버 상태 | 서버 상태 확인 (JSON) |

### 웹 인터페이스 (`GET /`)
- 수신된 파일 수 (이번 세션)
- 저장된 파일 수 (전체)
- 원격 전송 완료 수
- 원격 서버 정보
- 저장 위치 표시
- 10초마다 자동 새로고침

### 사용 방법

#### 기본 실행
```bash
python3 rpi_server.py
```

#### 옵션 지정
```bash
python3 rpi_server.py --port 5000 --dir /home/user/esp32_photos
```

#### 시스템 서비스로 등록
```bash
sudo nano /etc/systemd/system/esp32cam.service
```

서비스 파일 내용:
```ini
[Unit]
Description=ESP32-CAM Photo Receiver
After=network.target

[Service]
ExecStart=/usr/bin/python3 /home/donsub/rpi_server.py
WorkingDirectory=/home/donsub
Restart=always
User=donsub

[Install]
WantedBy=multi-user.target
```

서비스 활성화:
```bash
sudo systemctl enable esp32cam
sudo systemctl start esp32cam
```

### 필요한 라이브러리
- `flask` - 웹 서버 프레임워크
- `requests` - HTTP 요청 (원격 서버 전송)

설치:
```bash
pip3 install flask requests
```

### 웹 인터페이스 기능
- 수신된 파일 수 표시
- 저장된 파일 수 표시
- 원격 전송 완료 수 표시
- 원격 서버 정보 표시
- API 엔드포인트 목록
- 10초마다 자동 새로고침

---

## 3. 📊 시스템 동작 흐름

### 전체 데이터 흐름

```
┌─────────────────┐
│   ESP32-CAM     │
│  (카메라 모듈)   │
└────────┬────────┘
         │ 1. 사진 촬영 (JPEG)
         │ 2. HTTP POST 요청
         ↓
┌─────────────────────────────┐
│  라즈베리파이 서버            │
│  (rpi_server.py)            │
│  - 포트: 5000                │
│  - 엔드포인트: /upload       │
└────────┬────────────────────┘
         │ 3. 파일 수신 및 저장
         ↓
┌─────────────────┐
│ ./esp32_photos/ │
│  (로컬 저장)     │
└────────┬────────┘
         │ 4. 60초마다 자동 검사
         │ 5. 새 파일만 전송
         ↓
┌─────────────────────────────┐
│   원격 서버                  │
│   118.42.62.78:6000         │
│   /upload                    │
└─────────────────────────────┘
```

### 상세 동작 과정

1. **ESP32-CAM 촬영 및 전송**
   - 카메라로 사진 촬영 (JPEG 형식)
   - WiFi를 통해 라즈베리파이 IP로 HTTP POST 요청
   - 요청 URL: `http://[라즈베리파이IP]:5000/upload`
   - multipart/form-data 형식으로 파일 전송

2. **라즈베리파이 수신 및 저장**
   - Flask 서버가 `/upload` 엔드포인트에서 파일 수신
   - 타임스탬프를 포함한 고유 파일명 생성
   - `./esp32_photos/` 디렉토리에 저장
   - JSON 응답으로 수신 확인

3. **원격 서버 자동 전송**
   - 백그라운드 스레드가 60초마다 실행
   - `./esp32_photos/` 디렉토리에서 새 파일 검색
   - `sent_files.json`과 비교하여 미전송 파일만 전송
   - 전송 성공 시 기록에 추가

### 네트워크 구성
- **ESP32-CAM**: WiFi 연결 (같은 네트워크에 연결 필요)
- **라즈베리파이**: WiFi/이더넷 연결 (로컬 네트워크)
- **원격 서버**: 인터넷 연결 (공인 IP: 118.42.62.78)

---

## 4. ⚙️ 설정 및 구성

### ESP32-CAM 설정 (구현 필요)

#### 하드웨어
- **보드**: AI Thinker ESP32-CAM 모듈
- **카메라 모델**: OV2640 (board_config.h에서 설정)
- **파티션 스키마**: 커스텀 파티션 (3.75MB 앱 공간)
- **PSRAM**: 활성화 권장 (고해상도 촬영)

#### 소프트웨어 설정 (코드에 포함 필요)
```cpp
// 라즈베리파이 서버 정보
const char* rpiServerIP = "192.168.1.100";  // 라즈베리파이 IP
const int rpiServerPort = 5000;
const char* rpiServerPath = "/upload";

// WiFi 설정
const char* ssid = "WiFi_SSID";
const char* password = "WiFi_PASSWORD";
```

### 라즈베리파이 서버 설정

#### 기본 설정
- **포트**: 5000 (명령줄 옵션으로 변경 가능)
- **저장 디렉토리**: `./esp32_photos` (명령줄 옵션으로 변경 가능)
- **호스트**: `0.0.0.0` (모든 인터페이스에서 수신)

#### 원격 서버 설정 (코드 내 하드코딩)
```python
REMOTE_SERVER_IP = "118.42.62.78"
REMOTE_SERVER_PORT = 6000
REMOTE_UPLOAD_URL = f"http://{REMOTE_SERVER_IP}:{REMOTE_SERVER_PORT}/upload"
UPLOAD_INTERVAL_SECONDS = 60  # 1분
```

#### 필요한 Python 라이브러리
```bash
pip3 install flask requests
```

### 네트워크 요구사항
- ESP32-CAM과 라즈베리파이가 같은 WiFi 네트워크에 연결되어야 함
- 라즈베리파이의 IP 주소를 ESP32-CAM 코드에 설정 필요
- 원격 서버는 인터넷을 통해 접근 가능해야 함

---

## 5. 🚀 사용 방법

### 라즈베리파이 서버 실행

#### 기본 실행
```bash
cd CameraServerTest/Raspberry
python3 rpi_server.py
```

#### 옵션 지정
```bash
python3 rpi_server.py --port 5000 --dir /home/user/esp32_photos
```

#### 시스템 서비스로 등록 (백그라운드 실행)
```bash
# 서비스 파일 생성
sudo nano /etc/systemd/system/esp32cam.service
```

서비스 파일 내용:
```ini
[Unit]
Description=ESP32-CAM Photo Receiver
After=network.target

[Service]
ExecStart=/usr/bin/python3 /home/donsub/rpi_server.py
WorkingDirectory=/home/donsub
Restart=always
User=donsub

[Install]
WantedBy=multi-user.target
```

서비스 활성화 및 시작:
```bash
sudo systemctl enable esp32cam
sudo systemctl start esp32cam
sudo systemctl status esp32cam  # 상태 확인
```

### ESP32-CAM 설정

#### 필요한 작업
1. `CameraServerTest.ino` 파일을 실제 ESP32-CAM 코드로 교체
2. 라즈베리파이 IP 주소 설정
3. WiFi 자격 증명 설정
4. Arduino IDE에서 업로드

#### 예상 코드 구조 (구현 필요)
```cpp
#include "esp_camera.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include "board_config.h"

// WiFi 및 서버 설정
const char* ssid = "WiFi_SSID";
const char* password = "WiFi_PASSWORD";
const char* rpiServerIP = "192.168.1.100";  // 라즈베리파이 IP
const int rpiServerPort = 5000;

void captureAndSend() {
  // 카메라로 사진 촬영
  camera_fb_t *fb = esp_camera_fb_get();
  
  // HTTP POST로 라즈베리파이 서버에 전송
  HTTPClient http;
  http.begin("http://" + String(rpiServerIP) + ":" + String(rpiServerPort) + "/upload");
  http.addHeader("Content-Type", "multipart/form-data");
  
  // 파일 전송
  http.POST(fb->buf, fb->len);
  
  esp_camera_fb_return(fb);
}
```

---

## 6. 🔍 주의사항 및 개선 필요 사항

### ⚠️ 현재 문제점

1. **CameraServerTest.ino 파일 불일치** (중요!)
   - 파일명은 ESP32-CAM 카메라 서버를 의미하지만
   - 실제 내용은 ESP32-C3 블루투스 오디오 스트리머 코드
   - **실제 ESP32-CAM 사진 촬영 및 전송 코드가 누락됨**
   - 라즈베리파이 서버는 준비되어 있으나, ESP32-CAM 코드가 없어 동작 불가

2. **하드코딩된 설정값**
   - 원격 서버 IP: `118.42.62.78` (코드에 직접 작성)
   - WiFi 자격 증명: ESP32-CAM 코드에 직접 작성 필요
   - 라즈베리파이 IP: ESP32-CAM 코드에 설정 필요

### ✅ 권장 개선 사항

1. **ESP32-CAM 코드 구현**
   - `CameraServerTest.ino`를 실제 ESP32-CAM 코드로 교체
   - 카메라 초기화, 사진 촬영, HTTP POST 전송 기능 구현

2. **설정 파일 분리**
   - WiFi 자격 증명을 별도 헤더 파일로 분리
   - 라즈베리파이 IP를 설정 파일로 관리

3. **에러 처리 강화**
   - 네트워크 연결 실패 시 재시도 로직
   - 사진 촬영 실패 시 처리

4. **보안 강화**
   - WiFi 자격 증명 암호화
   - 서버 인증 추가 (선택사항)

---

## 7. 📝 파일별 상세 정보

### 핵심 파일

#### `rpi_server.py` ⭐
- **역할**: 라즈베리파이 수신 서버 (핵심 컴포넌트)
- **기능**: 
  - ESP32-CAM에서 전송된 사진 수신
  - 로컬 저장
  - 원격 서버로 자동 전송
- **상태**: ✅ 완성됨, 동작 가능

#### `CameraServerTest.ino` ⚠️
- **역할**: ESP32-CAM 메인 코드 (예상)
- **현재 상태**: 오디오 스트리머 코드가 들어있음
- **필요한 기능**:
  - 카메라 초기화
  - 사진 촬영
  - HTTP POST로 라즈베리파이 서버에 전송
- **상태**: ❌ 구현 필요

#### `board_config.h`
- **역할**: 카메라 보드 설정
- **현재 설정**: `CAMERA_MODEL_AI_THINKER`
- **상태**: ✅ 설정 완료

#### `camera_pins.h`
- **역할**: 카메라 GPIO 핀 정의
- **상태**: ✅ AI Thinker 보드 핀 정의 완료

#### `camera_index.h`
- **역할**: 웹 인터페이스 HTML (압축)
- **용도**: ESP32-CAM 웹 서버 UI (선택사항)
- **상태**: ✅ 포함됨

#### `partitions.csv`
- **역할**: ESP32 플래시 메모리 파티션
- **상태**: ✅ 커스텀 파티션 설정 완료

### 부가 프로젝트

#### `ESP32C3_AudioStream.ino`
- **역할**: ESP32-C3 오디오 스트리밍 (별도 프로젝트)
- **기능**: GPIO 2번 핀 ADC → WiFi 웹 스트리밍
- **상태**: ✅ 완성됨 (카메라 시스템과 무관)

---

## 8. 📋 체크리스트

### 라즈베리파이 서버 설정 ✅
- [x] Python 3 설치
- [x] Flask, requests 라이브러리 설치
- [x] 서버 실행 또는 systemd 서비스 등록
- [x] 라즈베리파이 IP 주소 확인

### ESP32-CAM 설정 ⚠️
- [ ] `CameraServerTest.ino` 파일을 실제 카메라 코드로 교체
- [ ] 라즈베리파이 IP 주소 설정
- [ ] WiFi SSID/비밀번호 설정
- [ ] Arduino IDE에서 보드 설정 (ESP32 Dev Module)
- [ ] 파티션 스키마 선택 (커스텀)
- [ ] 코드 업로드 및 테스트

### 네트워크 확인
- [ ] ESP32-CAM과 라즈베리파이가 같은 WiFi 네트워크에 연결
- [ ] 라즈베리파이 서버가 정상 실행 중인지 확인 (`http://[라즈베리파이IP]:5000`)
- [ ] 원격 서버 접근 가능 여부 확인

---

## 📌 요약

### 시스템 목적
**ESP32-CAM으로 촬영한 사진을 라즈베리파이로 전송하고, 라즈베리파이가 원격 서버로 자동 전송하는 시스템**

### 현재 상태
- ✅ **라즈베리파이 서버**: 완성되어 동작 가능
- ❌ **ESP32-CAM 코드**: 구현 필요 (현재 오디오 스트리머 코드가 들어있음)
- ✅ **하드웨어 설정 파일**: 준비 완료 (board_config.h, camera_pins.h 등)

### 다음 단계
1. `CameraServerTest.ino` 파일을 실제 ESP32-CAM 사진 촬영 및 전송 코드로 교체
2. 라즈베리파이 IP 주소를 ESP32-CAM 코드에 설정
3. WiFi 자격 증명 설정
4. 테스트 및 디버깅

