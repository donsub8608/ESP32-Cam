# MDBT53 ↔ ESP32-CAM 시리얼 통신 프로토콜

## 시스템 개요

```
┌─────────────────┐         UART          ┌─────────────────┐
│     MDBT53      │ ←──────────────────→  │   ESP32-CAM     │
│   (nRF52833)    │    115200 baud        │                 │
│                 │    8N1                │                 │
├─────────────────┤                       ├─────────────────┤
│   SD 카드       │                       │     카메라      │
│   저장소        │                       │     OV2640      │
└─────────────────┘                       └─────────────────┘
```

## 하드웨어 연결

### UART 연결
| MDBT53 | ESP32-CAM | 설명 |
|--------|-----------|------|
| P0.06 (TX) | GPIO3 (RX) | MDBT53 → ESP32-CAM |
| P0.08 (RX) | GPIO1 (TX) | ESP32-CAM → MDBT53 |
| GND | GND | 공통 그라운드 |

### MDBT53 SD 카드 (SPI)
| MDBT53 | SD 카드 모듈 |
|--------|-------------|
| P0.28 | MOSI |
| P0.29 | MISO |
| P0.30 | SCK |
| P0.31 | CS |
| VCC | VCC (3.3V) |
| GND | GND |

## 통신 프로토콜

### 1. 사진 촬영 명령

**요청 (MDBT53 → ESP32-CAM):**
```
CAP\n
```

**응답 (ESP32-CAM → MDBT53):**
```
IMG:<size>\n
<JPEG 바이너리 데이터>
\nEND:<checksum>\n
```

- `<size>`: JPEG 데이터 바이트 크기 (10진수)
- `<JPEG 바이너리>`: 실제 JPEG 이미지 데이터
- `<checksum>`: XOR 체크섬 (2자리 16진수)

**예시:**
```
IMG:45678
[45678 bytes of JPEG data]
END:A5
```

### 2. 상태 확인

**요청:**
```
STATUS\n
```

**응답:**
```
OK:ESP32-CAM Status
OK:PSRAM: Yes
OK:Free heap: 123456 bytes
OK:Quality: 12
OK:Frame size: 5
```

### 3. LED 제어

**켜기:**
```
LED_ON\n
→ OK:LED ON
```

**끄기:**
```
LED_OFF\n
→ OK:LED OFF
```

### 4. 품질/해상도 설정

**JPEG 품질 (1-63, 낮을수록 고품질):**
```
QUALITY:10\n
→ OK:Quality set to 10
```

**프레임 크기 (0-10):**
```
SIZE:5\n
→ OK:Frame size set to 5
```

프레임 크기 옵션:
| 값 | 해상도 |
|----|--------|
| 0 | QQVGA (160x120) |
| 3 | QVGA (320x240) |
| 5 | VGA (640x480) |
| 6 | SVGA (800x600) |
| 8 | XGA (1024x768) |
| 9 | SXGA (1280x1024) |
| 10 | UXGA (1600x1200) |

### 5. 에러 응답

```
ERR:<message>\n
```

예시:
- `ERR:Capture failed`
- `ERR:Unknown command`
- `ERR:Invalid quality (1-63)`

## 데이터 흐름

### 사진 촬영 시퀀스

```
MDBT53                          ESP32-CAM
  │                                 │
  │──── "CAP\n" ───────────────────→│
  │                                 │ (LED ON)
  │                                 │ (캡처)
  │                                 │ (LED OFF)
  │←─── "IMG:45678\n" ─────────────│
  │                                 │
  │←─── [JPEG 데이터 45678 bytes] ─│
  │                                 │
  │←─── "\nEND:A5\n" ─────────────│
  │                                 │
  │ (체크섬 확인)                   │
  │ (SD 카드 저장)                  │
  │                                 │
```

## 수신 측 구현 가이드

### 1. 헤더 파싱
```c
// "IMG:<size>\n" 형식 파싱
char *img_ptr = strstr(buffer, "IMG:");
if (img_ptr) {
    int size = atoi(img_ptr + 4);
    // JPEG 수신 모드로 전환
}
```

### 2. 바이너리 수신
```c
// size 바이트만큼 수신
while (received < expected_size) {
    int len = uart_read(buf, sizeof(buf));
    memcpy(jpeg_buffer + received, buf, len);
    received += len;
}
```

### 3. 체크섬 검증
```c
uint8_t calculate_checksum(uint8_t *data, size_t len) {
    uint8_t checksum = 0;
    for (size_t i = 0; i < len; i++) {
        checksum ^= data[i];
    }
    return checksum;
}
```

## 주의사항

1. **보드레이트**: 반드시 양쪽 모두 115200bps로 설정
2. **플로우 컨트롤**: 없음 (None) - 소프트웨어로 처리
3. **버퍼 크기**: JPEG는 최대 512KB까지 될 수 있음
4. **타임아웃**: 수신 타임아웃 30초 권장
5. **전원**: ESP32-CAM은 5V, MDBT53는 3.3V - 레벨 시프터 불필요 (ESP32 GPIO는 3.3V 호환)

## 트러블슈팅

| 문제 | 해결책 |
|------|--------|
| 응답 없음 | TX/RX 선 확인, 보드레이트 확인 |
| 데이터 깨짐 | GND 연결 확인, 짧은 케이블 사용 |
| 체크섬 오류 | 재전송 요청 또는 무시하고 저장 |
| 메모리 부족 | 해상도 낮추기, 품질 값 높이기 |

