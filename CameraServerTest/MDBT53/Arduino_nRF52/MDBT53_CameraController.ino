/**
 * MDBT53 Camera Controller - Arduino nRF52 버전
 * 
 * 개발 환경: Adafruit nRF52 Arduino (Arduino IDE 또는 PlatformIO)
 * 보드 선택: "Adafruit Feather nRF52840 Express" 또는 유사 보드
 * 
 * 라이브러리 필요:
 *   - SdFat (by Bill Greiman)
 * 
 * 하드웨어 연결:
 *   nRF52 TX (Pin 1) → ESP32-CAM RX (GPIO3)
 *   nRF52 RX (Pin 0) → ESP32-CAM TX (GPIO1)
 *   GND ↔ GND
 *   
 *   SD 카드 (SPI):
 *   nRF52 MOSI → SD MOSI
 *   nRF52 MISO → SD MISO
 *   nRF52 SCK  → SD SCK
 *   nRF52 A5   → SD CS (또는 원하는 핀)
 */

#include <SPI.h>
#include <SdFat.h>

// ===========================
// 핀 설정
// ===========================
#define SD_CS_PIN       A5      // SD 카드 CS 핀
#define BUTTON_PIN      7       // 수동 촬영 버튼 (선택사항)
#define LED_PIN         LED_RED // 상태 LED

// ===========================
// 설정
// ===========================
#define SERIAL_BAUD     115200
#define ESP32_SERIAL    Serial1  // 하드웨어 시리얼1 사용
#define DEBUG_SERIAL    Serial   // USB 시리얼 (디버그)

#define JPEG_MAX_SIZE   (512UL * 1024UL)  // 최대 512KB
#define RX_TIMEOUT_MS   30000             // 수신 타임아웃 30초
#define CAPTURE_INTERVAL_MS 60000         // 60초마다 자동 촬영

// 프로토콜 정의
#define CMD_CAPTURE     "CAP\n"
#define CMD_STATUS      "STATUS\n"
#define RESP_IMG        "IMG:"
#define RESP_END        "END:"
#define RESP_OK         "OK:"
#define RESP_ERR        "ERR:"

// ===========================
// 전역 변수
// ===========================
SdFat sd;
FatFile file;

// JPEG 수신 버퍼
uint8_t* jpegBuffer = nullptr;
size_t jpegLength = 0;
size_t expectedLength = 0;

// 파일 카운터
uint32_t fileCounter = 0;

// 타이밍
unsigned long lastCaptureTime = 0;

// ===========================
// 함수 프로토타입
// ===========================
bool initSDCard();
bool requestCapture();
bool receiveJPEG();
bool saveJPEGToSD();
uint8_t calculateChecksum(uint8_t* data, size_t len);
void findNextFileNumber();

// ===========================
// Setup
// ===========================
void setup() {
  // 핀 설정
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);  // LED OFF (active low)
  
#ifdef BUTTON_PIN
  pinMode(BUTTON_PIN, INPUT_PULLUP);
#endif

  // 디버그 시리얼
  DEBUG_SERIAL.begin(SERIAL_BAUD);
  delay(1000);
  DEBUG_SERIAL.println("\n=== MDBT53 Camera Controller ===");
  DEBUG_SERIAL.println("Initializing...");

  // ESP32-CAM 시리얼
  ESP32_SERIAL.begin(SERIAL_BAUD);
  DEBUG_SERIAL.println("ESP32 Serial initialized");

  // JPEG 버퍼 할당
  jpegBuffer = (uint8_t*)malloc(JPEG_MAX_SIZE);
  if (!jpegBuffer) {
    DEBUG_SERIAL.println("ERROR: Failed to allocate JPEG buffer!");
    while (1) {
      digitalWrite(LED_PIN, LOW);
      delay(100);
      digitalWrite(LED_PIN, HIGH);
      delay(100);
    }
  }
  DEBUG_SERIAL.printf("JPEG buffer allocated: %lu bytes\n", JPEG_MAX_SIZE);

  // SD 카드 초기화
  if (!initSDCard()) {
    DEBUG_SERIAL.println("WARNING: SD card not available");
  }

  // ESP32-CAM 부팅 대기
  DEBUG_SERIAL.println("Waiting for ESP32-CAM...");
  delay(3000);

  // ESP32-CAM 상태 확인
  ESP32_SERIAL.print(CMD_STATUS);
  delay(500);
  
  DEBUG_SERIAL.println("ESP32-CAM Response:");
  while (ESP32_SERIAL.available()) {
    char c = ESP32_SERIAL.read();
    DEBUG_SERIAL.write(c);
  }
  DEBUG_SERIAL.println();

  DEBUG_SERIAL.println("System Ready!");
  DEBUG_SERIAL.println("Commands: 'c' = capture, 's' = status");
  
  lastCaptureTime = millis();
}

// ===========================
// Loop
// ===========================
void loop() {
  // 디버그 시리얼 명령 처리
  if (DEBUG_SERIAL.available()) {
    char cmd = DEBUG_SERIAL.read();
    
    if (cmd == 'c' || cmd == 'C') {
      DEBUG_SERIAL.println("\n--- Manual Capture ---");
      captureAndSave();
    }
    else if (cmd == 's' || cmd == 'S') {
      DEBUG_SERIAL.println("\n--- Status Request ---");
      ESP32_SERIAL.print(CMD_STATUS);
      delay(500);
      while (ESP32_SERIAL.available()) {
        DEBUG_SERIAL.write(ESP32_SERIAL.read());
      }
    }
  }

#ifdef BUTTON_PIN
  // 버튼 눌림 감지
  if (digitalRead(BUTTON_PIN) == LOW) {
    delay(50);  // 디바운스
    if (digitalRead(BUTTON_PIN) == LOW) {
      DEBUG_SERIAL.println("\n--- Button Capture ---");
      captureAndSave();
      while (digitalRead(BUTTON_PIN) == LOW) delay(10);  // 버튼 해제 대기
    }
  }
#endif

  // 주기적 자동 촬영
  if (millis() - lastCaptureTime >= CAPTURE_INTERVAL_MS) {
    DEBUG_SERIAL.println("\n--- Auto Capture ---");
    captureAndSave();
    lastCaptureTime = millis();
  }

  delay(10);
}

// ===========================
// 촬영 및 저장 워크플로우
// ===========================
bool captureAndSave() {
  digitalWrite(LED_PIN, LOW);  // LED ON
  
  bool success = false;
  
  if (requestCapture()) {
    if (receiveJPEG()) {
      if (saveJPEGToSD()) {
        success = true;
        DEBUG_SERIAL.println("Capture complete!");
      }
    }
  }
  
  digitalWrite(LED_PIN, HIGH);  // LED OFF
  return success;
}

// ===========================
// SD 카드 초기화
// ===========================
bool initSDCard() {
  DEBUG_SERIAL.println("Initializing SD card...");
  
  if (!sd.begin(SD_CS_PIN, SD_SCK_MHZ(8))) {
    DEBUG_SERIAL.println("SD initialization failed!");
    return false;
  }
  
  DEBUG_SERIAL.println("SD card initialized");
  
  // 기존 파일 개수 확인
  findNextFileNumber();
  DEBUG_SERIAL.printf("File counter starts at: %lu\n", fileCounter);
  
  return true;
}

// ===========================
// 다음 파일 번호 찾기
// ===========================
void findNextFileNumber() {
  FatFile root;
  FatFile entry;
  char name[32];
  
  if (!root.open("/")) return;
  
  while (entry.openNext(&root, O_RDONLY)) {
    entry.getName(name, sizeof(name));
    
    int num;
    if (sscanf(name, "photo_%d.jpg", &num) == 1) {
      if (num >= (int)fileCounter) {
        fileCounter = num + 1;
      }
    }
    entry.close();
  }
  root.close();
}

// ===========================
// 사진 촬영 요청
// ===========================
bool requestCapture() {
  DEBUG_SERIAL.println("Sending capture command...");
  
  // 버퍼 초기화
  while (ESP32_SERIAL.available()) ESP32_SERIAL.read();
  jpegLength = 0;
  expectedLength = 0;
  
  // CAP 명령 전송
  ESP32_SERIAL.print(CMD_CAPTURE);
  
  return true;
}

// ===========================
// JPEG 데이터 수신
// ===========================
bool receiveJPEG() {
  DEBUG_SERIAL.println("Waiting for JPEG data...");
  
  unsigned long startTime = millis();
  String header = "";
  bool headerReceived = false;
  
  // Step 1: "IMG:<size>\n" 헤더 수신
  while (millis() - startTime < RX_TIMEOUT_MS) {
    if (ESP32_SERIAL.available()) {
      char c = ESP32_SERIAL.read();
      header += c;
      
      if (c == '\n') {
        // 헤더 완료 확인
        int imgIdx = header.indexOf(RESP_IMG);
        if (imgIdx >= 0) {
          expectedLength = header.substring(imgIdx + 4).toInt();
          DEBUG_SERIAL.printf("Expected size: %lu bytes\n", expectedLength);
          
          if (expectedLength == 0 || expectedLength > JPEG_MAX_SIZE) {
            DEBUG_SERIAL.println("ERROR: Invalid JPEG size");
            return false;
          }
          
          headerReceived = true;
          break;
        }
        
        // 에러 확인
        if (header.indexOf(RESP_ERR) >= 0) {
          DEBUG_SERIAL.print("ESP32 Error: ");
          DEBUG_SERIAL.println(header);
          return false;
        }
        
        header = "";  // 다음 라인 준비
      }
    }
  }
  
  if (!headerReceived) {
    DEBUG_SERIAL.println("ERROR: Timeout waiting for header");
    return false;
  }
  
  // Step 2: JPEG 바이너리 수신
  startTime = millis();
  int lastPercent = 0;
  
  while (jpegLength < expectedLength && (millis() - startTime < RX_TIMEOUT_MS)) {
    if (ESP32_SERIAL.available()) {
      jpegBuffer[jpegLength++] = ESP32_SERIAL.read();
      startTime = millis();  // 타임아웃 리셋
      
      // 진행률 표시
      int percent = (jpegLength * 100) / expectedLength;
      if (percent / 10 > lastPercent / 10) {
        DEBUG_SERIAL.printf("Receiving: %d%%\n", percent);
        lastPercent = percent;
      }
    }
  }
  
  if (jpegLength < expectedLength) {
    DEBUG_SERIAL.printf("ERROR: Incomplete data (%lu/%lu bytes)\n", jpegLength, expectedLength);
    return false;
  }
  
  // Step 3: END 마커 및 체크섬 수신
  delay(100);  // 추가 데이터 대기
  
  String trailer = "";
  startTime = millis();
  while (millis() - startTime < 1000) {
    if (ESP32_SERIAL.available()) {
      char c = ESP32_SERIAL.read();
      trailer += c;
      if (trailer.length() > 20) break;  // 너무 길면 중단
    }
  }
  
  // 체크섬 확인
  int endIdx = trailer.indexOf(RESP_END);
  if (endIdx >= 0) {
    String checksumStr = trailer.substring(endIdx + 4, endIdx + 6);
    uint8_t receivedChecksum = strtol(checksumStr.c_str(), NULL, 16);
    uint8_t calculatedChecksum = calculateChecksum(jpegBuffer, jpegLength);
    
    if (receivedChecksum == calculatedChecksum) {
      DEBUG_SERIAL.println("Checksum OK!");
    } else {
      DEBUG_SERIAL.printf("WARNING: Checksum mismatch (received: %02X, calculated: %02X)\n",
                          receivedChecksum, calculatedChecksum);
    }
  } else {
    DEBUG_SERIAL.println("WARNING: END marker not found");
  }
  
  DEBUG_SERIAL.printf("JPEG received: %lu bytes\n", jpegLength);
  return true;
}

// ===========================
// SD 카드에 저장
// ===========================
bool saveJPEGToSD() {
  if (jpegLength == 0) {
    DEBUG_SERIAL.println("ERROR: No JPEG data");
    return false;
  }
  
  char filename[32];
  snprintf(filename, sizeof(filename), "photo_%04lu.jpg", fileCounter);
  
  DEBUG_SERIAL.printf("Saving to: %s\n", filename);
  
  if (!file.open(filename, O_WRONLY | O_CREAT | O_TRUNC)) {
    DEBUG_SERIAL.println("ERROR: Failed to create file");
    return false;
  }
  
  // 청크 단위로 쓰기
  const size_t chunkSize = 4096;
  size_t written = 0;
  
  while (written < jpegLength) {
    size_t toWrite = min(chunkSize, jpegLength - written);
    size_t result = file.write(&jpegBuffer[written], toWrite);
    if (result != toWrite) {
      DEBUG_SERIAL.println("ERROR: Write failed");
      file.close();
      return false;
    }
    written += result;
  }
  
  file.close();
  
  DEBUG_SERIAL.printf("Saved: %s (%lu bytes)\n", filename, jpegLength);
  fileCounter++;
  
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

