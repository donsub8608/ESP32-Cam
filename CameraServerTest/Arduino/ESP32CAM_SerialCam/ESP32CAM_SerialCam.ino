/**
 * ESP32-CAM Serial Camera
 * 
 * MDBT53(nRF52833)과 시리얼 통신을 통해 사진을 촬영하고 전송하는 펌웨어
 * 
 * 통신 프로토콜:
 *   명령 수신: "CAP\n"  → 사진 촬영 후 전송
 *   명령 수신: "STATUS\n" → 상태 확인
 *   
 *   응답 형식 (사진 전송):
 *     1. "IMG:<length>\n"  (헤더 - length는 JPEG 크기)
 *     2. <JPEG 바이너리 데이터>
 *     3. "END:<checksum>\n" (종료 - checksum은 XOR 체크섬)
 *   
 *   에러 응답:
 *     "ERR:<message>\n"
 * 
 * 하드웨어 연결:
 *   ESP32-CAM TX (GPIO1) → MDBT53 RX
 *   ESP32-CAM RX (GPIO3) → MDBT53 TX
 *   GND ↔ GND
 *   
 * 주의: ESP32-CAM의 GPIO1, GPIO3는 기본 Serial 포트입니다.
 *       프로그래밍 시에는 USB-TTL을 분리해야 합니다.
 */

#include "esp_camera.h"
#include "FS.h"
#include "SD_MMC.h"

// ===========================
// 카메라 모델 선택 (AI-Thinker ESP32-CAM)
// ===========================
#define CAMERA_MODEL_AI_THINKER

// 카메라 핀 정의
#define PWDN_GPIO_NUM  32
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM  0
#define SIOD_GPIO_NUM  26
#define SIOC_GPIO_NUM  27
#define Y9_GPIO_NUM    35
#define Y8_GPIO_NUM    34
#define Y7_GPIO_NUM    39
#define Y6_GPIO_NUM    36
#define Y5_GPIO_NUM    21
#define Y4_GPIO_NUM    19
#define Y3_GPIO_NUM    18
#define Y2_GPIO_NUM    5
#define VSYNC_GPIO_NUM 25
#define HREF_GPIO_NUM  23
#define PCLK_GPIO_NUM  22
#define LED_GPIO_NUM   4

// ===========================
// 시리얼 통신 설정
// ===========================
#define SERIAL_BAUD_RATE 115200
#define SERIAL_TIMEOUT_MS 5000

// 프로토콜 정의
#define CMD_CAPTURE    "CAP"
#define CMD_STATUS     "STATUS"
#define CMD_LED_ON     "LED_ON"
#define CMD_LED_OFF    "LED_OFF"
#define CMD_SET_QUALITY "QUALITY"
#define CMD_SET_SIZE    "SIZE"

#define RESP_IMG       "IMG:"
#define RESP_END       "END:"
#define RESP_OK        "OK:"
#define RESP_ERR       "ERR:"

// LED PWM 설정
#define LED_LEDC_FREQ 5000
#define LED_LEDC_RESOLUTION 8

// ===========================
// 전역 변수
// ===========================
String inputBuffer = "";
bool stringComplete = false;
int jpegQuality = 12;  // 1-63, 낮을수록 고품질

// 프레임 크기 옵션
// FRAMESIZE_QVGA (320x240)
// FRAMESIZE_VGA (640x480)
// FRAMESIZE_SVGA (800x600)
// FRAMESIZE_XGA (1024x768)
// FRAMESIZE_SXGA (1280x1024)
// FRAMESIZE_UXGA (1600x1200)
framesize_t frameSize = FRAMESIZE_SVGA;

// ===========================
// 함수 프로토타입
// ===========================
void processCommand(String cmd);
bool captureAndSend();
uint8_t calculateChecksum(uint8_t* data, size_t len);
void sendStatus();
void setLED(bool on);

// ===========================
// Setup 함수
// ===========================
void setup() {
  // 시리얼 초기화
  Serial.begin(SERIAL_BAUD_RATE);
  Serial.setTimeout(SERIAL_TIMEOUT_MS);
  
  delay(1000);  // 안정화 대기
  
  Serial.println("\n=== ESP32-CAM Serial Camera ===");
  Serial.println("Initializing...");
  
  // PSRAM 확인
  if (psramFound()) {
    Serial.println("OK:PSRAM found");
  } else {
    Serial.println("ERR:PSRAM not found");
  }

  // --- 카메라 초기화 설정 ---
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.jpeg_quality = jpegQuality;
  config.fb_count = 1;

  // PSRAM 사용 가능 시 설정
  if (psramFound()) {
    config.frame_size = frameSize;
    config.jpeg_quality = jpegQuality;
    config.fb_count = 2;
    config.grab_mode = CAMERA_GRAB_LATEST;
  } else {
    config.frame_size = FRAMESIZE_VGA;  // PSRAM 없으면 낮은 해상도
    config.fb_location = CAMERA_FB_IN_DRAM;
  }

  // 카메라 초기화
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("ERR:Camera init failed 0x%x\n", err);
    delay(1000);
    ESP.restart();
  }
  Serial.println("OK:Camera initialized");

  // 센서 설정
  sensor_t *s = esp_camera_sensor_get();
  if (s->id.PID == OV3660_PID) {
    s->set_vflip(s, 1);
    s->set_brightness(s, 1);
    s->set_saturation(s, -2);
  }

  // LED 초기화
  ledcAttach(LED_GPIO_NUM, LED_LEDC_FREQ, LED_LEDC_RESOLUTION);
  ledcWrite(LED_GPIO_NUM, 0);  // 초기 OFF
  Serial.println("OK:LED configured");

  Serial.println("OK:Ready");
  Serial.println("Commands: CAP, STATUS, LED_ON, LED_OFF, QUALITY:<1-63>, SIZE:<0-10>");
}

// ===========================
// Loop 함수
// ===========================
void loop() {
  // 시리얼 데이터 수신
  while (Serial.available()) {
    char inChar = (char)Serial.read();
    
    if (inChar == '\n' || inChar == '\r') {
      if (inputBuffer.length() > 0) {
        stringComplete = true;
      }
    } else {
      inputBuffer += inChar;
    }
  }

  // 명령 처리
  if (stringComplete) {
    inputBuffer.trim();
    processCommand(inputBuffer);
    inputBuffer = "";
    stringComplete = false;
  }
  
  delay(10);
}

// ===========================
// 명령 처리 함수
// ===========================
void processCommand(String cmd) {
  cmd.toUpperCase();
  
  if (cmd == CMD_CAPTURE) {
    // 사진 촬영 및 전송
    captureAndSend();
  }
  else if (cmd == CMD_STATUS) {
    // 상태 확인
    sendStatus();
  }
  else if (cmd == CMD_LED_ON) {
    setLED(true);
    Serial.println("OK:LED ON");
  }
  else if (cmd == CMD_LED_OFF) {
    setLED(false);
    Serial.println("OK:LED OFF");
  }
  else if (cmd.startsWith("QUALITY:")) {
    // JPEG 품질 설정 (1-63, 낮을수록 고품질)
    int quality = cmd.substring(8).toInt();
    if (quality >= 1 && quality <= 63) {
      jpegQuality = quality;
      sensor_t *s = esp_camera_sensor_get();
      s->set_quality(s, jpegQuality);
      Serial.printf("OK:Quality set to %d\n", jpegQuality);
    } else {
      Serial.println("ERR:Invalid quality (1-63)");
    }
  }
  else if (cmd.startsWith("SIZE:")) {
    // 프레임 크기 설정 (0-10)
    int size = cmd.substring(5).toInt();
    if (size >= 0 && size <= 10) {
      frameSize = (framesize_t)size;
      sensor_t *s = esp_camera_sensor_get();
      s->set_framesize(s, frameSize);
      Serial.printf("OK:Frame size set to %d\n", size);
    } else {
      Serial.println("ERR:Invalid size (0-10)");
    }
  }
  else {
    Serial.println("ERR:Unknown command");
  }
}

// ===========================
// 사진 촬영 및 전송
// ===========================
bool captureAndSend() {
  // LED 켜기 (촬영 전 조명)
  ledcWrite(LED_GPIO_NUM, 255);
  delay(100);  // 짧은 안정화 시간
  
  // 카메라 캡처
  camera_fb_t *fb = esp_camera_fb_get();
  
  // LED 끄기
  ledcWrite(LED_GPIO_NUM, 0);
  
  if (!fb) {
    Serial.println("ERR:Capture failed");
    return false;
  }
  
  // 체크섬 계산
  uint8_t checksum = calculateChecksum(fb->buf, fb->len);
  
  // 헤더 전송: "IMG:<size>\n"
  Serial.printf("IMG:%u\n", fb->len);
  Serial.flush();
  
  // 수신 측 준비 시간
  delay(10);
  
  // JPEG 바이너리 데이터 전송
  // 큰 데이터는 청크로 나누어 전송
  const size_t chunkSize = 1024;
  size_t offset = 0;
  
  while (offset < fb->len) {
    size_t toSend = min(chunkSize, fb->len - offset);
    Serial.write(fb->buf + offset, toSend);
    Serial.flush();
    offset += toSend;
    
    // 플로우 컨트롤: 너무 빠른 전송 방지
    delayMicroseconds(100);
  }
  
  // 종료 마커 전송: "\nEND:<checksum>\n"
  Serial.printf("\nEND:%02X\n", checksum);
  Serial.flush();
  
  // 프레임버퍼 반환
  esp_camera_fb_return(fb);
  
  return true;
}

// ===========================
// XOR 체크섬 계산
// ===========================
uint8_t calculateChecksum(uint8_t* data, size_t len) {
  uint8_t checksum = 0;
  for (size_t i = 0; i < len; i++) {
    checksum ^= data[i];
  }
  return checksum;
}

// ===========================
// 상태 정보 전송
// ===========================
void sendStatus() {
  Serial.println("OK:ESP32-CAM Status");
  Serial.printf("OK:PSRAM: %s\n", psramFound() ? "Yes" : "No");
  Serial.printf("OK:Free heap: %d bytes\n", ESP.getFreeHeap());
  if (psramFound()) {
    Serial.printf("OK:Free PSRAM: %d bytes\n", ESP.getFreePsram());
  }
  Serial.printf("OK:Quality: %d\n", jpegQuality);
  Serial.printf("OK:Frame size: %d\n", frameSize);
}

// ===========================
// LED 제어
// ===========================
void setLED(bool on) {
  ledcWrite(LED_GPIO_NUM, on ? 128 : 0);  // 50% 밝기
}

