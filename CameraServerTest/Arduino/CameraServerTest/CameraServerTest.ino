#include "esp_camera.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include "FS.h"
#include "SD_MMC.h"
#include "esp_http_server.h"

// ===========================
// Select camera model in board_config.h
// ===========================
#include "board_config.h"

// ===========================
// LED PWM 설정 (ESP32 Arduino Core 3.x)
// ===========================
#define LED_LEDC_FREQ 5000
#define LED_LEDC_RESOLUTION 8  // 0-255

// ===========================
// WiFi 설정
// ===========================
const char *ssid = "KT_WiFi_7_81F6";
const char *password = "ffe0000767";

// ===========================
// SD 카드 저장 관련 변수
// ===========================
unsigned long lastCaptureTime = 0;
const unsigned long captureInterval = 60000; // 60초

// ===========================
// 라즈베리파이 자동 전송 설정
// ===========================
#define ENABLE_AUTO_UPLOAD true  // 자동 전송 활성화
const char* rpiServerIP = "172.30.1.52";  // 라즈베리파이 IP
const int rpiServerPort = 5000;  // 라즈베리파이 서버 포트
const char* rpiApiKey = "esp32cam_secret_key_2024";  // API 키 (라즈베리파이와 동일하게!)
const bool deleteAfterUpload = true;  // 전송 성공 후 SD에서 삭제

// 웹 서버
httpd_handle_t camera_httpd = NULL;
httpd_handle_t stream_httpd = NULL;

// 함수 프로토타입 선언
void captureAndSave();
void startCameraServer();

// 스트림 파트 경계
#define PART_BOUNDARY "123456789000000000000987654321"
static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* _STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* _STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

// ===========================
// 라이브 스트림 핸들러
// ===========================
static esp_err_t stream_handler(httpd_req_t *req) {
  camera_fb_t *fb = NULL;
  esp_err_t res = ESP_OK;
  char part_buf[64];

  res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
  if (res != ESP_OK) {
    return res;
  }

  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  while (true) {
    fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("Camera capture failed");
      res = ESP_FAIL;
    } else {
      size_t hlen = snprintf(part_buf, 64, _STREAM_PART, fb->len);
      res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
      if (res == ESP_OK) {
        res = httpd_resp_send_chunk(req, part_buf, hlen);
      }
      if (res == ESP_OK) {
        res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
      }
      esp_camera_fb_return(fb);
      fb = NULL;
    }
    if (res != ESP_OK) {
      break;
    }
    delay(10);
  }
  return res;
}

// ===========================
// 단일 캡처 핸들러 (현재 카메라 화면)
// ===========================
static esp_err_t capture_handler(httpd_req_t *req) {
  camera_fb_t *fb = NULL;
  esp_err_t res = ESP_OK;

  fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Camera capture failed");
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  
  res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
  esp_camera_fb_return(fb);
  
  return res;
}

// ===========================
// HTML 메인 페이지
// ===========================
static esp_err_t index_handler(httpd_req_t *req) {
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset='UTF-8'>
  <meta name='viewport' content='width=device-width, initial-scale=1.0'>
  <title>ESP32-CAM</title>
  <script src='https://cdn.jsdelivr.net/npm/tesseract.js@4/dist/tesseract.min.js'></script>
  <style>
    * { box-sizing: border-box; }
    body { 
      font-family: 'Segoe UI', Arial, sans-serif; 
      margin: 0; 
      padding: 20px; 
      background: #ffffff;
      min-height: 100vh;
      color: #333;
    }
    .container { 
      max-width: 1400px; 
      margin: 0 auto; 
    }
    h1 { 
      color: #0077cc; 
      text-align: center;
      font-size: 2.5em;
      margin-bottom: 10px;
    }
    .subtitle {
      text-align: center;
      color: #666;
      margin-bottom: 30px;
    }
    .stream-section {
      background: #f8f9fa;
      border-radius: 15px;
      padding: 20px;
      margin-bottom: 30px;
      border: 1px solid #e0e0e0;
    }
    .stream-container {
      display: flex;
      justify-content: center;
      margin-bottom: 15px;
    }
    #stream-img {
      max-width: 100%;
      height: auto;
      border-radius: 10px;
      box-shadow: 0 4px 15px rgba(0,0,0,0.1);
    }
    .controls {
      display: flex;
      justify-content: center;
      gap: 15px;
      flex-wrap: wrap;
    }
    .btn { 
      display: inline-block; 
      padding: 12px 25px; 
      background: linear-gradient(135deg, #0099cc 0%, #0077aa 100%);
      color: #fff; 
      text-decoration: none; 
      border-radius: 8px; 
      border: none;
      cursor: pointer;
      font-weight: bold;
      font-size: 14px;
      transition: all 0.3s ease;
    }
    .btn:hover { 
      transform: translateY(-2px);
      box-shadow: 0 5px 15px rgba(0, 119, 204, 0.3);
    }
    .btn-secondary {
      background: linear-gradient(135deg, #dc3545 0%, #c82333 100%);
    }
    .btn-green {
      background: linear-gradient(135deg, #28a745 0%, #218838 100%);
    }
    h2 { 
      color: #0077cc; 
      border-bottom: 2px solid #e0e0e0;
      padding-bottom: 10px;
      margin-top: 0;
    }
    .gallery { 
      display: grid; 
      grid-template-columns: repeat(auto-fill, minmax(280px, 1fr)); 
      gap: 20px; 
    }
    .photo-card { 
      background: #fff;
      border: 1px solid #e0e0e0;
      border-radius: 12px; 
      padding: 15px;
      transition: all 0.3s ease;
      box-shadow: 0 2px 8px rgba(0,0,0,0.05);
    }
    .photo-card:hover {
      transform: translateY(-5px);
      box-shadow: 0 8px 25px rgba(0,0,0,0.1);
      border-color: #0099cc;
    }
    .photo-card img { 
      width: 100%; 
      height: 200px;
      object-fit: cover;
      border-radius: 8px; 
      cursor: pointer; 
    }
    .photo-card img:hover { 
      opacity: 0.9; 
    }
    .photo-name { 
      margin: 12px 0 8px 0; 
      font-weight: bold; 
      color: #0077cc;
      font-size: 14px;
    }
    .photo-actions {
      display: flex;
      gap: 10px;
    }
    .photo-actions .btn {
      flex: 1;
      text-align: center;
      padding: 8px 12px;
      font-size: 12px;
    }
    .no-photos {
      text-align: center;
      padding: 40px;
      color: #999;
      font-style: italic;
    }
    .status {
      text-align: center;
      padding: 10px;
      margin-bottom: 20px;
      border-radius: 8px;
      background: #e8f4fc;
      color: #0077cc;
    }
    .btn-ocr {
      background: linear-gradient(135deg, #6f42c1 0%, #5a32a3 100%);
    }
    .ocr-result {
      margin-top: 10px;
      padding: 10px;
      background: #f0f0f0;
      border-radius: 6px;
      font-family: monospace;
      font-size: 13px;
      color: #333;
      display: none;
      word-break: break-all;
    }
    .ocr-result.show {
      display: block;
    }
    .ocr-value {
      font-size: 18px;
      font-weight: bold;
      color: #0077cc;
      margin-top: 5px;
    }
    .ocr-loading {
      color: #666;
      font-style: italic;
    }
    .crop-container {
      position: relative;
      display: inline-block;
      cursor: crosshair;
    }
    .crop-overlay {
      position: absolute;
      border: 2px dashed #ff0000;
      background: rgba(255,0,0,0.1);
      pointer-events: none;
      display: none;
    }
    .crop-instructions {
      font-size: 12px;
      color: #666;
      margin-top: 5px;
      text-align: center;
    }
    .modal {
      display: none;
      position: fixed;
      top: 0;
      left: 0;
      width: 100%;
      height: 100%;
      background: rgba(0,0,0,0.8);
      z-index: 1000;
      justify-content: center;
      align-items: center;
    }
    .modal.show {
      display: flex;
    }
    .modal-content {
      background: #fff;
      padding: 20px;
      border-radius: 12px;
      max-width: 90%;
      max-height: 90%;
      overflow: auto;
      position: relative;
    }
    .modal-close {
      position: absolute;
      top: 10px;
      right: 15px;
      font-size: 24px;
      cursor: pointer;
      color: #333;
    }
    .modal-title {
      margin-bottom: 15px;
      color: #0077cc;
    }
  </style>
</head>
<body>
  <div class='container'>
    <h1>🎥 ESP32-CAM</h1>
    <p class='subtitle'>실시간 카메라 스트림 및 사진 갤러리</p>
    
    <div class='status' id='sd-status'>)rawliteral";

  // SD 카드 상태 표시
  if (SD_MMC.cardType() == CARD_NONE) {
    html += "❌ SD 카드 없음 - 자동 촬영 비활성";
  } else {
    uint64_t cardSize = SD_MMC.cardSize() / (1024 * 1024);
    uint64_t usedBytes = SD_MMC.usedBytes() / (1024 * 1024);
    String cardType = "알수없음";
    if (SD_MMC.cardType() == CARD_MMC) cardType = "MMC";
    else if (SD_MMC.cardType() == CARD_SD) cardType = "SD";
    else if (SD_MMC.cardType() == CARD_SDHC) cardType = "SDHC";
    
    html += "✅ SD: " + cardType + " | 용량: " + String((uint32_t)cardSize) + "MB | 사용: " + String((uint32_t)usedBytes) + "MB | ⏱ 1분마다 자동촬영 중";
  }
  
  html += R"rawliteral(</div>
    
    <div class='stream-section'>
      <h2>📹 실시간 스트림</h2>
      <div class='stream-container'>
        <img id='stream-img' src='/capture' alt='Camera Stream'>
      </div>
      <div class='controls'>
        <button class='btn' onclick='startStream()'>▶ 스트림 시작</button>
        <button class='btn btn-secondary' onclick='stopStream()'>⏹ 스트림 정지</button>
        <button class='btn btn-green' onclick='captureNow()'>📷 사진 촬영</button>
        <button class='btn' onclick='location.reload()'>🔄 새로고침</button>
      </div>
    </div>
    
    <div class='stream-section'>
      <h2>🖼 저장된 사진</h2>
      <div class='gallery' id='gallery'>
)rawliteral";

  // SD 카드에서 사진 목록 읽기
  File root = SD_MMC.open("/");
  if (!root) {
    html += "<div class='no-photos'>SD 카드를 읽을 수 없습니다</div>";
  } else {
    int photoCount = 0;
    File file = root.openNextFile();
    while (file) {
      String fileName = String(file.name());
      // 파일 이름에서 경로 제거 (혹시 있다면)
      int lastSlash = fileName.lastIndexOf('/');
      if (lastSlash >= 0) {
        fileName = fileName.substring(lastSlash + 1);
      }
      
      if (fileName.endsWith(".jpg") || fileName.endsWith(".JPG") || 
          fileName.endsWith(".jpeg") || fileName.endsWith(".JPEG")) {
        photoCount++;
        html += "<div class='photo-card' id='card-" + String(photoCount) + "'>";
        html += "<img src='/photo?name=" + fileName + "' onclick=\"window.open(this.src, '_blank')\" loading='lazy' id='img-" + String(photoCount) + "'>";
        html += "<div class='photo-name'>" + fileName + "</div>";
        html += "<div class='photo-actions'>";
        html += "<a href='/photo?name=" + fileName + "' class='btn' download>💾 저장</a>";
        html += "<button class='btn btn-ocr' onclick=\"runOCR('img-" + String(photoCount) + "', 'ocr-" + String(photoCount) + "')\">🔍 OCR</button>";
        html += "<a href='/delete?name=" + fileName + "' class='btn btn-secondary' onclick=\"return confirm('정말 삭제하시겠습니까?')\">🗑</a>";
        html += "</div>";
        html += "<div class='ocr-result' id='ocr-" + String(photoCount) + "'></div>";
        html += "</div>";
      }
      file = root.openNextFile();
    }
    
    if (photoCount == 0) {
      html += "<div class='no-photos'>저장된 사진이 없습니다</div>";
    }
  }

  html += R"rawliteral(
      </div>
    </div>
  </div>
  
  <!-- OCR 영역 선택 모달 -->
  <div class='modal' id='ocrModal'>
    <div class='modal-content'>
      <span class='modal-close' onclick='closeOcrModal()'>&times;</span>
      <h3 class='modal-title'>🔍 OCR 영역 선택</h3>
      <p class='crop-instructions'>마우스로 드래그하여 인식할 영역을 선택하세요</p>
      <div class='crop-container' id='cropContainer'>
        <img id='cropImage' style='max-width:100%;'>
        <div class='crop-overlay' id='cropOverlay'></div>
      </div>
      <div style='margin-top:15px; text-align:center;'>
        <button class='btn btn-ocr' onclick='performCropOCR()'>🔍 선택 영역 OCR</button>
        <button class='btn' onclick='performFullOCR()'>📄 전체 이미지 OCR</button>
        <button class='btn btn-secondary' onclick='closeOcrModal()'>취소</button>
      </div>
      <div class='ocr-result' id='modalOcrResult'></div>
    </div>
  </div>

  <script>
    var streamActive = false;
    var cropStartX, cropStartY, cropEndX, cropEndY;
    var isCropping = false;
    var currentOcrImgSrc = '';
    var currentResultId = '';
    
    function startStream() {
      fetch('/led_on').then(() => console.log('LED ON'));
      var img = document.getElementById('stream-img');
      img.src = '/stream?' + new Date().getTime();
      streamActive = true;
    }
    
    function stopStream() {
      fetch('/led_off').then(() => console.log('LED OFF'));
      var img = document.getElementById('stream-img');
      img.src = '/capture?' + new Date().getTime();
      streamActive = false;
    }
    
    function captureNow() {
      fetch('/save')
        .then(response => response.text())
        .then(data => {
          alert(data);
          location.reload();
        })
        .catch(err => alert('촬영 실패: ' + err));
    }
    
    // OCR 모달 열기
    function runOCR(imgId, resultId) {
      var img = document.getElementById(imgId);
      currentOcrImgSrc = img.src;
      currentResultId = resultId;
      
      document.getElementById('cropImage').src = img.src;
      document.getElementById('ocrModal').classList.add('show');
      document.getElementById('modalOcrResult').className = 'ocr-result';
      document.getElementById('modalOcrResult').innerHTML = '';
      
      // 크롭 영역 초기화
      var overlay = document.getElementById('cropOverlay');
      overlay.style.display = 'none';
      cropStartX = cropStartY = cropEndX = cropEndY = 0;
      
      setupCropEvents();
    }
    
    function closeOcrModal() {
      document.getElementById('ocrModal').classList.remove('show');
    }
    
    // 크롭 영역 선택 이벤트
    function setupCropEvents() {
      var container = document.getElementById('cropContainer');
      var overlay = document.getElementById('cropOverlay');
      var img = document.getElementById('cropImage');
      
      container.onmousedown = function(e) {
        var rect = img.getBoundingClientRect();
        cropStartX = e.clientX - rect.left;
        cropStartY = e.clientY - rect.top;
        isCropping = true;
        overlay.style.display = 'block';
        overlay.style.left = cropStartX + 'px';
        overlay.style.top = cropStartY + 'px';
        overlay.style.width = '0px';
        overlay.style.height = '0px';
      };
      
      container.onmousemove = function(e) {
        if (!isCropping) return;
        var rect = img.getBoundingClientRect();
        cropEndX = e.clientX - rect.left;
        cropEndY = e.clientY - rect.top;
        
        var x = Math.min(cropStartX, cropEndX);
        var y = Math.min(cropStartY, cropEndY);
        var w = Math.abs(cropEndX - cropStartX);
        var h = Math.abs(cropEndY - cropStartY);
        
        overlay.style.left = x + 'px';
        overlay.style.top = y + 'px';
        overlay.style.width = w + 'px';
        overlay.style.height = h + 'px';
      };
      
      container.onmouseup = function(e) {
        isCropping = false;
      };
    }
    
    // 선택 영역 OCR
    async function performCropOCR() {
      var img = document.getElementById('cropImage');
      var overlay = document.getElementById('cropOverlay');
      var resultDiv = document.getElementById('modalOcrResult');
      var cardResult = document.getElementById(currentResultId);
      
      if (overlay.style.display === 'none' || 
          parseInt(overlay.style.width) < 10 || 
          parseInt(overlay.style.height) < 10) {
        alert('OCR 영역을 선택해주세요!');
        return;
      }
      
      resultDiv.className = 'ocr-result show';
      resultDiv.innerHTML = '<span class="ocr-loading">🔄 선택 영역 OCR 분석 중...</span>';
      
      try {
        // 캔버스에 선택 영역 그리기
        var canvas = document.createElement('canvas');
        var ctx = canvas.getContext('2d');
        
        var scaleX = img.naturalWidth / img.width;
        var scaleY = img.naturalHeight / img.height;
        
        var x = Math.min(cropStartX, cropEndX) * scaleX;
        var y = Math.min(cropStartY, cropEndY) * scaleY;
        var w = Math.abs(cropEndX - cropStartX) * scaleX;
        var h = Math.abs(cropEndY - cropStartY) * scaleY;
        
        canvas.width = w;
        canvas.height = h;
        ctx.drawImage(img, x, y, w, h, 0, 0, w, h);
        
        var croppedDataUrl = canvas.toDataURL('image/jpeg');
        
        const result = await Tesseract.recognize(croppedDataUrl, 'eng+kor', {
          logger: m => {
            if (m.status === 'recognizing text') {
              resultDiv.innerHTML = '<span class="ocr-loading">🔄 분석 중... ' + Math.round(m.progress * 100) + '%</span>';
            }
          }
        });
        
        var text = result.data.text.trim();
        var numbers = text.match(/[\\d.]+/g);
        var numericValues = numbers ? numbers.join(', ') : '숫자 없음';
        
        var resultHtml = '<strong>📝 인식 텍스트:</strong><br>' + (text || '텍스트 없음') + 
          '<div class="ocr-value">📊 수치값: ' + numericValues + '</div>';
        
        resultDiv.innerHTML = resultHtml;
        if (cardResult) {
          cardResult.className = 'ocr-result show';
          cardResult.innerHTML = resultHtml;
        }
      } catch(err) {
        resultDiv.innerHTML = '<span style="color:red;">❌ OCR 실패: ' + err.message + '</span>';
      }
    }
    
    // 전체 이미지 OCR
    async function performFullOCR() {
      var resultDiv = document.getElementById('modalOcrResult');
      var cardResult = document.getElementById(currentResultId);
      
      resultDiv.className = 'ocr-result show';
      resultDiv.innerHTML = '<span class="ocr-loading">🔄 전체 이미지 OCR 분석 중...</span>';
      
      try {
        const result = await Tesseract.recognize(currentOcrImgSrc, 'eng+kor', {
          logger: m => {
            if (m.status === 'recognizing text') {
              resultDiv.innerHTML = '<span class="ocr-loading">🔄 분석 중... ' + Math.round(m.progress * 100) + '%</span>';
            }
          }
        });
        
        var text = result.data.text.trim();
        var numbers = text.match(/[\\d.]+/g);
        var numericValues = numbers ? numbers.join(', ') : '숫자 없음';
        
        var resultHtml = '<strong>📝 인식 텍스트:</strong><br>' + (text || '텍스트 없음') + 
          '<div class="ocr-value">📊 수치값: ' + numericValues + '</div>';
        
        resultDiv.innerHTML = resultHtml;
        if (cardResult) {
          cardResult.className = 'ocr-result show';
          cardResult.innerHTML = resultHtml;
        }
      } catch(err) {
        resultDiv.innerHTML = '<span style="color:red;">❌ OCR 실패: ' + err.message + '</span>';
      }
    }
    
    // 초기 이미지 로드
    document.getElementById('stream-img').onerror = function() {
      this.src = 'data:image/svg+xml,<svg xmlns="http://www.w3.org/2000/svg" width="640" height="480"><rect fill="%23eee" width="640" height="480"/><text fill="%23666" font-size="24" x="50%" y="50%" text-anchor="middle">카메라 연결 대기중...</text></svg>';
    };
    
    // 60초마다 갤러리 자동 새로고침 (스트림 중이 아닐 때만)
    setInterval(function() {
      if (!streamActive) {
        location.reload();
      }
    }, 65000);  // 65초 (촬영 후 약간의 여유)
  </script>
</body>
</html>
)rawliteral";
  
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, html.c_str(), html.length());
}

// ===========================
// 저장된 사진 파일 전송
// ===========================
static esp_err_t photo_handler(httpd_req_t *req) {
  char query[128] = {0};
  char fileName[64] = {0};
  
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
    Serial.println("No query string");
    httpd_resp_send_404(req);
    return ESP_FAIL;
  }
  
  if (httpd_query_key_value(query, "name", fileName, sizeof(fileName)) != ESP_OK) {
    Serial.println("No name parameter");
    httpd_resp_send_404(req);
    return ESP_FAIL;
  }
  
  // URL 디코딩 (간단한 버전)
  String decodedName = String(fileName);
  decodedName.replace("%20", " ");
  
  // 파일 경로 생성 (앞에 / 추가)
  String filePath = "/" + decodedName;
  
  Serial.printf("Requested file: %s\n", filePath.c_str());
  
  File file = SD_MMC.open(filePath.c_str(), FILE_READ);
  if (!file) {
    Serial.printf("Failed to open file: %s\n", filePath.c_str());
    httpd_resp_send_404(req);
    return ESP_FAIL;
  }
  
  size_t fileSize = file.size();
  Serial.printf("Sending file: %s, size: %d bytes\n", filePath.c_str(), fileSize);
  
  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Cache-Control", "max-age=3600");
  
  // 청크 단위로 전송 (더 큰 버퍼 사용)
  uint8_t *buffer = (uint8_t *)malloc(4096);
  if (!buffer) {
    file.close();
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  
  size_t bytesRead;
  while ((bytesRead = file.read(buffer, 4096)) > 0) {
    if (httpd_resp_send_chunk(req, (const char *)buffer, bytesRead) != ESP_OK) {
      free(buffer);
      file.close();
      return ESP_FAIL;
    }
  }
  
  free(buffer);
  file.close();
  httpd_resp_send_chunk(req, NULL, 0);
  
  Serial.println("File sent successfully");
  return ESP_OK;
}

// ===========================
// 사진 저장 핸들러 (웹에서 촬영)
// ===========================
static esp_err_t save_handler(httpd_req_t *req) {
  camera_fb_t *fb = NULL;
  
  // LED 먼저 켜고 3초 대기 (카메라 노출 안정화)
#if defined(LED_GPIO_NUM)
  ledcWrite(LED_GPIO_NUM, 255);
  delay(3000);  // 3초 대기
#endif

  fb = esp_camera_fb_get();

  if (!fb) {
#if defined(LED_GPIO_NUM)
    ledcWrite(LED_GPIO_NUM, 0);  // 실패 시에도 LED 끄기
#endif
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_send(req, "카메라 캡처 실패", -1);
    return ESP_FAIL;
  }

  // 파일 이름 생성 (타임스탬프 사용)
  static int webCaptureCount = 1000;  // 웹 촬영은 1000번부터
  String path = "/web_" + String(webCaptureCount) + ".jpg";
  webCaptureCount++;

  File file = SD_MMC.open(path.c_str(), FILE_WRITE);
  if (!file) {
    esp_camera_fb_return(fb);
#if defined(LED_GPIO_NUM)
    ledcWrite(LED_GPIO_NUM, 0);  // 실패 시에도 LED 끄기
#endif
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_send(req, "파일 저장 실패", -1);
    return ESP_FAIL;
  }

  file.write(fb->buf, fb->len);
  file.close();
  esp_camera_fb_return(fb);

#if defined(LED_GPIO_NUM)
  // 촬영 완료 후 LED 끄기
  ledcWrite(LED_GPIO_NUM, 0);
#endif

  String response = "사진 저장 완료: " + path;
  httpd_resp_set_type(req, "text/plain; charset=utf-8");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, response.c_str(), response.length());
}

// ===========================
// 사진 삭제 핸들러
// ===========================
static esp_err_t delete_handler(httpd_req_t *req) {
  char query[128] = {0};
  char fileName[64] = {0};
  
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
    httpd_resp_send_404(req);
    return ESP_FAIL;
  }
  
  if (httpd_query_key_value(query, "name", fileName, sizeof(fileName)) != ESP_OK) {
    httpd_resp_send_404(req);
    return ESP_FAIL;
  }
  
  String filePath = "/" + String(fileName);
  
  if (SD_MMC.remove(filePath.c_str())) {
    // 삭제 성공 - 메인 페이지로 리다이렉트
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    return httpd_resp_send(req, NULL, 0);
  } else {
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_send(req, "삭제 실패", -1);
  }
}

// ===========================
// 파일 목록 JSON API (라즈베리파이 연동용)
// ===========================
static esp_err_t api_files_handler(httpd_req_t *req) {
  String json = "{\"files\":[";
  
  File root = SD_MMC.open("/");
  if (!root) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, "{\"error\":\"SD card not available\"}", -1);
  }
  
  bool first = true;
  File file = root.openNextFile();
  while (file) {
    String fileName = String(file.name());
    int lastSlash = fileName.lastIndexOf('/');
    if (lastSlash >= 0) {
      fileName = fileName.substring(lastSlash + 1);
    }
    
    if (fileName.endsWith(".jpg") || fileName.endsWith(".JPG") || 
        fileName.endsWith(".jpeg") || fileName.endsWith(".JPEG")) {
      if (!first) json += ",";
      first = false;
      json += "{\"name\":\"" + fileName + "\",\"size\":" + String(file.size()) + "}";
    }
    file = root.openNextFile();
  }
  
  json += "]}";
  
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, json.c_str(), json.length());
}

// ===========================
// 파일 다운로드 API (바이너리 전송 최적화)
// ===========================
static esp_err_t api_download_handler(httpd_req_t *req) {
  char query[128] = {0};
  char fileName[64] = {0};
  
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"error\":\"No query string\"}", -1);
  }
  
  if (httpd_query_key_value(query, "name", fileName, sizeof(fileName)) != ESP_OK) {
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"error\":\"No name parameter\"}", -1);
  }
  
  String decodedName = String(fileName);
  decodedName.replace("%20", " ");
  String filePath = "/" + decodedName;
  
  File file = SD_MMC.open(filePath.c_str(), FILE_READ);
  if (!file) {
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"error\":\"File not found\"}", -1);
  }
  
  size_t fileSize = file.size();
  
  // Content-Length 헤더 설정
  char contentLength[32];
  snprintf(contentLength, sizeof(contentLength), "%d", fileSize);
  
  httpd_resp_set_type(req, "application/octet-stream");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Content-Length", contentLength);
  httpd_resp_set_hdr(req, "Content-Disposition", ("attachment; filename=" + decodedName).c_str());
  
  // 더 큰 버퍼로 빠르게 전송
  uint8_t *buffer = (uint8_t *)malloc(8192);
  if (!buffer) {
    file.close();
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  
  size_t bytesRead;
  while ((bytesRead = file.read(buffer, 8192)) > 0) {
    if (httpd_resp_send_chunk(req, (const char *)buffer, bytesRead) != ESP_OK) {
      free(buffer);
      file.close();
      return ESP_FAIL;
    }
  }
  
  free(buffer);
  file.close();
  httpd_resp_send_chunk(req, NULL, 0);
  
  return ESP_OK;
}

// ===========================
// 전체 파일 삭제 후 확인 API
// ===========================
static esp_err_t api_delete_handler(httpd_req_t *req) {
  char query[128] = {0};
  char fileName[64] = {0};
  
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
      httpd_query_key_value(query, "name", fileName, sizeof(fileName)) != ESP_OK) {
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"success\":false,\"error\":\"Invalid request\"}", -1);
  }
  
  String filePath = "/" + String(fileName);
  
  if (SD_MMC.remove(filePath.c_str())) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, "{\"success\":true}", -1);
  } else {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, "{\"success\":false,\"error\":\"Delete failed\"}", -1);
  }
}

// ===========================
// LED 제어 핸들러
// ===========================
static esp_err_t led_on_handler(httpd_req_t *req) {
#if defined(LED_GPIO_NUM)
  // 절반 밝기 (127/255)
  ledcWrite(LED_GPIO_NUM, 127);
  Serial.println("LED ON (50% brightness)");
#endif
  httpd_resp_set_type(req, "text/plain");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, "LED ON", -1);
}

static esp_err_t led_off_handler(httpd_req_t *req) {
#if defined(LED_GPIO_NUM)
  ledcWrite(LED_GPIO_NUM, 0);
  Serial.println("LED OFF");
#endif
  httpd_resp_set_type(req, "text/plain");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, "LED OFF", -1);
}

// ===========================
// 웹 서버 시작
// ===========================
void startCameraServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.max_uri_handlers = 15;
  config.stack_size = 8192;
  config.max_open_sockets = 5;

  // 메인 서버 핸들러
  httpd_uri_t index_uri = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = index_handler,
    .user_ctx = NULL
  };

  httpd_uri_t capture_uri = {
    .uri = "/capture",
    .method = HTTP_GET,
    .handler = capture_handler,
    .user_ctx = NULL
  };

  httpd_uri_t photo_uri = {
    .uri = "/photo",
    .method = HTTP_GET,
    .handler = photo_handler,
    .user_ctx = NULL
  };

  httpd_uri_t save_uri = {
    .uri = "/save",
    .method = HTTP_GET,
    .handler = save_handler,
    .user_ctx = NULL
  };

  httpd_uri_t delete_uri = {
    .uri = "/delete",
    .method = HTTP_GET,
    .handler = delete_handler,
    .user_ctx = NULL
  };

  httpd_uri_t led_on_uri = {
    .uri = "/led_on",
    .method = HTTP_GET,
    .handler = led_on_handler,
    .user_ctx = NULL
  };

  httpd_uri_t led_off_uri = {
    .uri = "/led_off",
    .method = HTTP_GET,
    .handler = led_off_handler,
    .user_ctx = NULL
  };

  // 라즈베리파이 연동용 API
  httpd_uri_t api_files_uri = {
    .uri = "/api/files",
    .method = HTTP_GET,
    .handler = api_files_handler,
    .user_ctx = NULL
  };

  httpd_uri_t api_download_uri = {
    .uri = "/api/download",
    .method = HTTP_GET,
    .handler = api_download_handler,
    .user_ctx = NULL
  };

  httpd_uri_t api_delete_uri = {
    .uri = "/api/delete",
    .method = HTTP_GET,
    .handler = api_delete_handler,
    .user_ctx = NULL
  };

  Serial.println("Starting web server on port 80...");
  if (httpd_start(&camera_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(camera_httpd, &index_uri);
    httpd_register_uri_handler(camera_httpd, &capture_uri);
    httpd_register_uri_handler(camera_httpd, &photo_uri);
    httpd_register_uri_handler(camera_httpd, &save_uri);
    httpd_register_uri_handler(camera_httpd, &delete_uri);
    httpd_register_uri_handler(camera_httpd, &led_on_uri);
    httpd_register_uri_handler(camera_httpd, &led_off_uri);
    httpd_register_uri_handler(camera_httpd, &api_files_uri);
    httpd_register_uri_handler(camera_httpd, &api_download_uri);
    httpd_register_uri_handler(camera_httpd, &api_delete_uri);
    Serial.println("HTTP server started successfully");
    Serial.println("API Endpoints for Raspberry Pi:");
    Serial.println("  - GET /api/files     : Get file list (JSON)");
    Serial.println("  - GET /api/download  : Download file");
    Serial.println("  - GET /api/delete    : Delete file");
  } else {
    Serial.println("Failed to start HTTP server!");
  }

  // 스트림 서버 (별도 포트)
  config.server_port = 81;
  config.ctrl_port = 32769;
  config.max_open_sockets = 3;

  httpd_uri_t stream_uri = {
    .uri = "/stream",
    .method = HTTP_GET,
    .handler = stream_handler,
    .user_ctx = NULL
  };

  // 포트 80에서도 스트림 접근 가능하게
  httpd_uri_t stream_uri_main = {
    .uri = "/stream",
    .method = HTTP_GET,
    .handler = stream_handler,
    .user_ctx = NULL
  };

  if (camera_httpd) {
    httpd_register_uri_handler(camera_httpd, &stream_uri_main);
  }

  Serial.println("Starting stream server on port 81...");
  if (httpd_start(&stream_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(stream_httpd, &stream_uri);
    Serial.println("Stream server started successfully");
  } else {
    Serial.println("Failed to start stream server!");
  }
}

// ===========================
// Setup 함수
// ===========================
void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  Serial.println();
  Serial.println("=================================");
  Serial.println("  ESP32-CAM Web Server Starting  ");
  Serial.println("=================================");
  
  // PSRAM 확인
  if (psramFound()) {
    Serial.println("PSRAM found and initialized");
  } else {
    Serial.println("Warning: PSRAM not found. Performance may be limited.");
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
  config.jpeg_quality = 12;
  config.fb_count = 1;

  // PSRAM 사용 가능 시 설정
  if (psramFound()) {
    config.frame_size = FRAMESIZE_UXGA;  // 1600x1200
    config.jpeg_quality = 10;
    config.fb_count = 2;
    config.grab_mode = CAMERA_GRAB_LATEST;
  } else {
    config.frame_size = FRAMESIZE_SVGA;  // 800x600
    config.fb_location = CAMERA_FB_IN_DRAM;
  }

  // 카메라 초기화
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x\n", err);
    delay(1000);
    ESP.restart();
  }
  Serial.println("Camera initialized successfully");

  // 센서 설정
  sensor_t *s = esp_camera_sensor_get();
  if (s->id.PID == OV3660_PID) {
    s->set_vflip(s, 1);
    s->set_brightness(s, 1);
    s->set_saturation(s, -2);
  }

#if defined(CAMERA_MODEL_M5STACK_WIDE) || defined(CAMERA_MODEL_M5STACK_ESP32CAM)
  s->set_vflip(s, 1);
  s->set_hmirror(s, 1);
#endif

#if defined(CAMERA_MODEL_ESP32S3_EYE)
  s->set_vflip(s, 1);
#endif

  // LED PWM 설정 (밝기 조절 가능)
#if defined(LED_GPIO_NUM)
  ledcAttach(LED_GPIO_NUM, LED_LEDC_FREQ, LED_LEDC_RESOLUTION);
  ledcWrite(LED_GPIO_NUM, 0);  // 초기 OFF
  Serial.println("LED PWM configured");
#endif

  // --- SD 카드 초기화 (SD_MMC 사용) ---
  Serial.println("Initializing SD card...");
  if (!SD_MMC.begin("/sdcard", true)) {  // true = 1-bit mode
    Serial.println("SD Card Mount Failed! Continuing without SD...");
  } else {
    uint8_t cardType = SD_MMC.cardType();
    if (cardType == CARD_NONE) {
      Serial.println("No SD card attached");
    } else {
      Serial.print("SD Card Type: ");
      if (cardType == CARD_MMC) {
        Serial.println("MMC");
      } else if (cardType == CARD_SD) {
        Serial.println("SDSC");
      } else if (cardType == CARD_SDHC) {
        Serial.println("SDHC");
      } else {
        Serial.println("UNKNOWN");
      }
      uint64_t cardSize = SD_MMC.cardSize() / (1024 * 1024);
      Serial.printf("SD Card Size: %lluMB\n", cardSize);
    }
  }

  // --- Wi-Fi 연결 ---
  Serial.printf("Connecting to WiFi: %s\n", ssid);
  WiFi.begin(ssid, password);
  WiFi.setSleep(false);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.println("=================================");
    Serial.println("  WiFi connected successfully!   ");
    Serial.println("=================================");
    Serial.print("  IP Address: ");
    Serial.println(WiFi.localIP());
    Serial.println("---------------------------------");
    Serial.print("  Web Interface: http://");
    Serial.println(WiFi.localIP());
    Serial.print("  Stream URL: http://");
    Serial.print(WiFi.localIP());
    Serial.println(":81/stream");
    Serial.println("=================================");
    
    // 웹 서버 시작
    startCameraServer();
  } else {
    Serial.println("\nWiFi connection failed!");
    Serial.println("Please check SSID and password");
  }

  // 초기 촬영 (SD 카드가 있는 경우)
  if (SD_MMC.cardType() != CARD_NONE) {
    delay(1000);
    captureAndSave();
    lastCaptureTime = millis();
  }
}

// ===========================
// Loop 함수
// ===========================
void loop() {
  unsigned long currentTime = millis();
  
  // 1분마다 촬영 (SD 카드가 있는 경우)
  if (SD_MMC.cardType() != CARD_NONE) {
    if (currentTime - lastCaptureTime >= captureInterval) {
      captureAndSave();
      lastCaptureTime = currentTime;
    }
  }

  // WiFi 연결 확인 및 재연결
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi disconnected! Attempting to reconnect...");
    WiFi.reconnect();
    delay(5000);
  }

  delay(100);
}

// ===========================
// 라즈베리파이로 파일 전송 함수
// ===========================
bool uploadToRaspberryPi(const char* filePath, const char* fileName) {
#if ENABLE_AUTO_UPLOAD
  Serial.printf("📤 라즈베리파이로 전송 시작: %s\n", fileName);
  
  File file = SD_MMC.open(filePath, FILE_READ);
  if (!file) {
    Serial.println("❌ 파일 열기 실패");
    return false;
  }
  
  size_t fileSize = file.size();
  Serial.printf("📁 파일 크기: %d bytes\n", fileSize);
  
  // HTTP 클라이언트 설정
  HTTPClient http;
  String serverUrl = "http://" + String(rpiServerIP) + ":" + String(rpiServerPort) + "/upload";
  
  http.begin(serverUrl);
  http.setTimeout(30000);  // 30초 타임아웃
  
  // Multipart 경계 설정
  String boundary = "----ESP32CAMBoundary";
  http.addHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
  http.addHeader("X-API-Key", rpiApiKey);  // API 키 인증
  
  // Multipart 데이터 구성
  String bodyStart = "--" + boundary + "\r\n";
  bodyStart += "Content-Disposition: form-data; name=\"file\"; filename=\"" + String(fileName) + "\"\r\n";
  bodyStart += "Content-Type: image/jpeg\r\n\r\n";
  
  String bodyEnd = "\r\n--" + boundary + "--\r\n";
  
  size_t totalLen = bodyStart.length() + fileSize + bodyEnd.length();
  
  // 스트림 전송을 위한 버퍼
  uint8_t* buffer = (uint8_t*)malloc(fileSize + bodyStart.length() + bodyEnd.length());
  if (!buffer) {
    Serial.println("❌ 메모리 할당 실패");
    file.close();
    http.end();
    return false;
  }
  
  // 데이터 조립
  size_t pos = 0;
  memcpy(buffer + pos, bodyStart.c_str(), bodyStart.length());
  pos += bodyStart.length();
  
  file.read(buffer + pos, fileSize);
  pos += fileSize;
  file.close();
  
  memcpy(buffer + pos, bodyEnd.c_str(), bodyEnd.length());
  pos += bodyEnd.length();
  
  // POST 전송
  int httpCode = http.POST(buffer, totalLen);
  free(buffer);
  
  if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_CREATED) {
    String response = http.getString();
    Serial.printf("✅ 전송 성공! 응답: %s\n", response.c_str());
    http.end();
    return true;
  } else {
    Serial.printf("❌ 전송 실패! HTTP 코드: %d\n", httpCode);
    if (httpCode > 0) {
      Serial.printf("   응답: %s\n", http.getString().c_str());
    }
    http.end();
    return false;
  }
#else
  return false;
#endif
}

// 메모리 버퍼에서 직접 전송 (SD 저장 없이)
bool uploadBufferToRaspberryPi(uint8_t* jpgBuf, size_t jpgLen, const char* fileName) {
#if ENABLE_AUTO_UPLOAD
  Serial.printf("📤 라즈베리파이로 직접 전송: %s (%d bytes)\n", fileName, jpgLen);
  
  HTTPClient http;
  String serverUrl = "http://" + String(rpiServerIP) + ":" + String(rpiServerPort) + "/upload";
  
  http.begin(serverUrl);
  http.setTimeout(30000);
  
  String boundary = "----ESP32CAMBoundary";
  http.addHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
  http.addHeader("X-API-Key", rpiApiKey);  // API 키 인증
  
  String bodyStart = "--" + boundary + "\r\n";
  bodyStart += "Content-Disposition: form-data; name=\"file\"; filename=\"" + String(fileName) + "\"\r\n";
  bodyStart += "Content-Type: image/jpeg\r\n\r\n";
  
  String bodyEnd = "\r\n--" + boundary + "--\r\n";
  
  size_t totalLen = bodyStart.length() + jpgLen + bodyEnd.length();
  
  uint8_t* buffer = (uint8_t*)malloc(totalLen);
  if (!buffer) {
    Serial.println("❌ 메모리 할당 실패");
    http.end();
    return false;
  }
  
  size_t pos = 0;
  memcpy(buffer + pos, bodyStart.c_str(), bodyStart.length());
  pos += bodyStart.length();
  memcpy(buffer + pos, jpgBuf, jpgLen);
  pos += jpgLen;
  memcpy(buffer + pos, bodyEnd.c_str(), bodyEnd.length());
  
  int httpCode = http.POST(buffer, totalLen);
  free(buffer);
  
  if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_CREATED) {
    Serial.println("✅ 전송 성공!");
    http.end();
    return true;
  } else {
    Serial.printf("❌ 전송 실패! HTTP 코드: %d\n", httpCode);
    http.end();
    return false;
  }
#else
  return false;
#endif
}

// ===========================
// 사진 촬영 및 저장 함수
// ===========================
void captureAndSave() {
  Serial.println("Preparing to take picture...");
  
#if defined(LED_GPIO_NUM)
  // LED 먼저 켜고 3초 대기 (카메라 노출 안정화)
  ledcWrite(LED_GPIO_NUM, 255);  // 최대 밝기
  Serial.println("LED ON - waiting 3 seconds...");
  delay(3000);  // 3초 대기
#endif

  Serial.println("Taking picture now!");
  camera_fb_t *fb = esp_camera_fb_get();

  if (!fb) {
    Serial.println("Camera capture failed");
#if defined(LED_GPIO_NUM)
    ledcWrite(LED_GPIO_NUM, 0);  // 실패 시에도 LED 끄기
#endif
    return;
  }

  // 파일 이름 생성 (타임스탬프 사용)
  static int imageCount = 0;
  String fileName = "photo_" + String(imageCount) + ".jpg";
  String path = "/" + fileName;
  imageCount++;

  bool savedToSD = false;
  bool uploadedToRpi = false;

  // SD 카드에 저장
  Serial.printf("💾 SD 저장: %s\n", path.c_str());
  File file = SD_MMC.open(path.c_str(), FILE_WRITE);
  
  if (!file) {
    Serial.println("⚠️ SD 저장 실패 - 라즈베리파이로 직접 전송 시도");
  } else {
    file.write(fb->buf, fb->len);
    file.close();
    Serial.printf("✅ SD 저장 완료: %s (%d bytes)\n", path.c_str(), fb->len);
    savedToSD = true;
  }

#if ENABLE_AUTO_UPLOAD
  // 라즈베리파이로 전송
  if (savedToSD) {
    // SD에 저장된 파일에서 전송
    uploadedToRpi = uploadToRaspberryPi(path.c_str(), fileName.c_str());
  } else {
    // SD 저장 실패 시 버퍼에서 직접 전송
    uploadedToRpi = uploadBufferToRaspberryPi(fb->buf, fb->len, fileName.c_str());
  }
  
  // 전송 성공 후 SD에서 삭제 (설정된 경우)
  if (uploadedToRpi && savedToSD && deleteAfterUpload) {
    if (SD_MMC.remove(path.c_str())) {
      Serial.printf("🗑️ SD에서 삭제됨: %s\n", path.c_str());
    }
  }
#endif

  esp_camera_fb_return(fb);
  
#if defined(LED_GPIO_NUM)
  // 촬영 완료 후 LED 끄기
  ledcWrite(LED_GPIO_NUM, 0);
  Serial.println("LED OFF");
#endif

  Serial.println("📷 촬영 완료!");
}
