#!/usr/bin/env python3
"""
ESP32-CAM 파일 수신 서버 (라즈베리파이용)

ESP32-CAM에서 촬영한 사진을 자동으로 수신하여 저장합니다.

사용법:
  python3 rpi_server.py                      # 기본 설정으로 실행
  python3 rpi_server.py --port 5000          # 포트 지정
  python3 rpi_server.py --dir /home/donsub/ESP32  # 저장 디렉토리 지정

라즈베리파이에서 서비스로 등록:
  sudo nano /etc/systemd/system/esp32cam.service
  
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

  sudo systemctl enable esp32cam
  sudo systemctl start esp32cam
"""

import os
import sys
import argparse
import threading
import time
import json
from datetime import datetime
from pathlib import Path

# Flask 설치 확인
try:
    from flask import Flask, request, jsonify
except ImportError:
    print("❌ Flask가 설치되어 있지 않습니다.")
    print("   설치 명령: pip3 install flask")
    sys.exit(1)

# requests 설치 확인
try:
    import requests
except ImportError:
    print("❌ requests가 설치되어 있지 않습니다.")
    print("   설치 명령: pip3 install requests")
    sys.exit(1)

# 기본 설정
DEFAULT_PORT = 5000
DEFAULT_SAVE_DIR = "./esp32_photos"

# 외부 서버 전송 설정
REMOTE_SERVER_IP = "118.42.62.78"
REMOTE_SERVER_PORT = 6000
REMOTE_UPLOAD_URL = f"http://{REMOTE_SERVER_IP}:{REMOTE_SERVER_PORT}/upload"
UPLOAD_INTERVAL_SECONDS = 60  # 1분 (60초)

app = Flask(__name__)

# 전역 설정 (argparse에서 설정)
SAVE_DIR = DEFAULT_SAVE_DIR
RECEIVED_COUNT = 0
UPLOADED_COUNT = 0  # 원격 서버로 업로드 성공한 수
SENT_FILES_LOG = "./sent_files.json"  # 이미 전송한 파일 기록


def load_sent_files() -> set:
    """이미 전송한 파일 목록 로드"""
    try:
        if os.path.exists(SENT_FILES_LOG):
            with open(SENT_FILES_LOG, 'r') as f:
                return set(json.load(f))
    except Exception as e:
        print(f"⚠️ 전송 기록 로드 실패: {e}")
    return set()


def save_sent_files(sent_files: set):
    """전송한 파일 목록 저장"""
    try:
        with open(SENT_FILES_LOG, 'w') as f:
            json.dump(list(sent_files), f)
    except Exception as e:
        print(f"⚠️ 전송 기록 저장 실패: {e}")


def upload_to_remote_server(file_path: Path) -> bool:
    """원격 서버로 파일 전송"""
    global UPLOADED_COUNT
    
    try:
        with open(file_path, 'rb') as f:
            files = {'file': (file_path.name, f, 'image/jpeg')}
            response = requests.post(
                REMOTE_UPLOAD_URL,
                files=files,
                timeout=30
            )
        
        if response.status_code == 200:
            UPLOADED_COUNT += 1
            print(f"📤 [{UPLOADED_COUNT}] 원격 전송 성공: {file_path.name}")
            return True
        else:
            print(f"❌ 원격 전송 실패: {file_path.name} (상태 코드: {response.status_code})")
            return False
            
    except requests.exceptions.Timeout:
        print(f"⏰ 원격 전송 타임아웃: {file_path.name}")
        return False
    except requests.exceptions.ConnectionError:
        print(f"🔌 원격 서버 연결 실패: {REMOTE_UPLOAD_URL}")
        return False
    except Exception as e:
        print(f"❌ 원격 전송 오류: {file_path.name} - {e}")
        return False


def periodic_upload_task():
    """주기적으로 새로운 사진을 원격 서버로 전송"""
    print(f"\n🔄 원격 전송 스케줄러 시작 (매 {UPLOAD_INTERVAL_SECONDS}초)")
    print(f"   대상 서버: {REMOTE_UPLOAD_URL}\n")
    
    sent_files = load_sent_files()
    
    while True:
        try:
            save_dir = Path(SAVE_DIR)
            if save_dir.exists():
                # 모든 jpg 파일 검색
                all_files = list(save_dir.glob("*.jpg"))
                new_files = [f for f in all_files if f.name not in sent_files]
                
                if new_files:
                    print(f"\n📊 전송할 새 파일: {len(new_files)}개")
                    
                    for file_path in new_files:
                        if upload_to_remote_server(file_path):
                            sent_files.add(file_path.name)
                            save_sent_files(sent_files)
                    
                    print(f"✅ 전송 완료 (총 {len(sent_files)}개 전송됨)\n")
                else:
                    current_time = datetime.now().strftime("%H:%M:%S")
                    print(f"⏳ [{current_time}] 새로운 파일 없음 (전송 대기 중...)")
            
        except Exception as e:
            print(f"❌ 전송 스케줄러 오류: {e}")
        
        # 다음 전송까지 대기
        time.sleep(UPLOAD_INTERVAL_SECONDS)


def start_upload_scheduler():
    """백그라운드에서 업로드 스케줄러 시작"""
    upload_thread = threading.Thread(target=periodic_upload_task, daemon=True)
    upload_thread.start()
    return upload_thread


def get_unique_filename(directory: Path, original_name: str) -> str:
    """중복되지 않는 파일명 생성"""
    base_name = Path(original_name).stem
    extension = Path(original_name).suffix
    
    # 타임스탬프 추가
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    new_name = f"{base_name}_{timestamp}{extension}"
    
    # 그래도 중복이면 숫자 추가
    counter = 1
    final_path = directory / new_name
    while final_path.exists():
        new_name = f"{base_name}_{timestamp}_{counter}{extension}"
        final_path = directory / new_name
        counter += 1
    
    return new_name


@app.route('/upload', methods=['POST'])
def upload_file():
    """ESP32-CAM에서 파일 수신"""
    global RECEIVED_COUNT
    
    if 'file' not in request.files:
        print("❌ 요청에 파일이 없음")
        return jsonify({"success": False, "error": "No file in request"}), 400
    
    file = request.files['file']
    
    if file.filename == '':
        print("❌ 파일명이 없음")
        return jsonify({"success": False, "error": "No filename"}), 400
    
    # 저장 디렉토리 확인
    save_dir = Path(SAVE_DIR)
    save_dir.mkdir(parents=True, exist_ok=True)
    
    # 고유 파일명 생성
    unique_name = get_unique_filename(save_dir, file.filename)
    save_path = save_dir / unique_name
    
    # 파일 저장
    try:
        file.save(str(save_path))
        file_size = save_path.stat().st_size
        RECEIVED_COUNT += 1
        
        print(f"✅ [{RECEIVED_COUNT}] 수신 완료: {unique_name} ({file_size:,} bytes)")
        print(f"   저장 위치: {save_path.absolute()}")
        
        return jsonify({
            "success": True,
            "filename": unique_name,
            "size": file_size,
            "path": str(save_path.absolute())
        }), 200
        
    except Exception as e:
        print(f"❌ 파일 저장 실패: {e}")
        return jsonify({"success": False, "error": str(e)}), 500


@app.route('/health', methods=['GET'])
def health_check():
    """서버 상태 확인"""
    return jsonify({
        "status": "ok",
        "received_count": RECEIVED_COUNT,
        "save_dir": str(Path(SAVE_DIR).absolute())
    }), 200


@app.route('/list', methods=['GET'])
def list_files():
    """수신된 파일 목록"""
    save_dir = Path(SAVE_DIR)
    if not save_dir.exists():
        return jsonify({"files": []}), 200
    
    files = []
    for f in save_dir.glob("*.jpg"):
        files.append({
            "name": f.name,
            "size": f.stat().st_size,
            "modified": datetime.fromtimestamp(f.stat().st_mtime).isoformat()
        })
    
    # 최신순 정렬
    files.sort(key=lambda x: x["modified"], reverse=True)
    
    return jsonify({"files": files, "count": len(files)}), 200


@app.route('/', methods=['GET'])
def index():
    """간단한 상태 페이지"""
    save_dir = Path(SAVE_DIR)
    file_count = len(list(save_dir.glob("*.jpg"))) if save_dir.exists() else 0
    
    html = f"""
    <!DOCTYPE html>
    <html>
    <head>
        <meta charset="UTF-8">
        <meta name="viewport" content="width=device-width, initial-scale=1.0">
        <title>ESP32-CAM 수신 서버</title>
        <style>
            body {{ 
                font-family: 'Segoe UI', Arial, sans-serif; 
                background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
                min-height: 100vh;
                margin: 0;
                padding: 20px;
                color: #fff;
            }}
            .container {{
                max-width: 800px;
                margin: 0 auto;
                background: rgba(255,255,255,0.1);
                backdrop-filter: blur(10px);
                border-radius: 20px;
                padding: 30px;
            }}
            h1 {{ text-align: center; margin-bottom: 30px; }}
            .stats {{
                display: grid;
                grid-template-columns: repeat(auto-fit, minmax(200px, 1fr));
                gap: 20px;
                margin-bottom: 30px;
            }}
            .stat-card {{
                background: rgba(255,255,255,0.2);
                border-radius: 15px;
                padding: 20px;
                text-align: center;
            }}
            .stat-value {{
                font-size: 2.5em;
                font-weight: bold;
            }}
            .stat-label {{
                opacity: 0.8;
                margin-top: 5px;
            }}
            .endpoint {{
                background: rgba(0,0,0,0.2);
                border-radius: 10px;
                padding: 15px;
                margin: 10px 0;
                font-family: monospace;
            }}
            .method {{
                display: inline-block;
                padding: 3px 8px;
                border-radius: 5px;
                font-size: 12px;
                font-weight: bold;
            }}
            .post {{ background: #49cc90; }}
            .get {{ background: #61affe; }}
        </style>
    </head>
    <body>
        <div class="container">
            <h1>📷 ESP32-CAM 수신 서버</h1>
            
            <div class="stats">
                <div class="stat-card">
                    <div class="stat-value">{RECEIVED_COUNT}</div>
                    <div class="stat-label">이번 세션 수신</div>
                </div>
                <div class="stat-card">
                    <div class="stat-value">{file_count}</div>
                    <div class="stat-label">저장된 파일</div>
                </div>
                <div class="stat-card">
                    <div class="stat-value">{UPLOADED_COUNT}</div>
                    <div class="stat-label">원격 전송 완료</div>
                </div>
            </div>
            
            <h3>🌐 원격 서버 전송</h3>
            <div class="endpoint">
                <strong>대상 서버:</strong> {REMOTE_UPLOAD_URL}<br>
                <strong>전송 주기:</strong> {UPLOAD_INTERVAL_SECONDS}초마다
            </div>
            
            <h3>📡 API 엔드포인트</h3>
            <div class="endpoint">
                <span class="method post">POST</span> /upload - 파일 업로드
            </div>
            <div class="endpoint">
                <span class="method get">GET</span> /list - 파일 목록
            </div>
            <div class="endpoint">
                <span class="method get">GET</span> /health - 서버 상태
            </div>
            
            <h3>📁 저장 위치</h3>
            <div class="endpoint">{Path(SAVE_DIR).absolute()}</div>
        </div>
        
        <script>
            // 10초마다 자동 새로고침
            setTimeout(() => location.reload(), 10000);
        </script>
    </body>
    </html>
    """
    return html


def main():
    global SAVE_DIR
    
    parser = argparse.ArgumentParser(
        description="ESP32-CAM 파일 수신 서버"
    )
    parser.add_argument(
        "--port", "-p",
        type=int,
        default=DEFAULT_PORT,
        help=f"서버 포트 (기본값: {DEFAULT_PORT})"
    )
    parser.add_argument(
        "--dir", "-d",
        default=DEFAULT_SAVE_DIR,
        help=f"저장 디렉토리 (기본값: {DEFAULT_SAVE_DIR})"
    )
    parser.add_argument(
        "--host",
        default="0.0.0.0",
        help="바인딩 호스트 (기본값: 0.0.0.0)"
    )
    
    args = parser.parse_args()
    SAVE_DIR = args.dir
    
    # 저장 디렉토리 생성
    save_dir = Path(SAVE_DIR)
    save_dir.mkdir(parents=True, exist_ok=True)
    
    print("=" * 60)
    print("  📷 ESP32-CAM 파일 수신 서버")
    print("=" * 60)
    print(f"  🌐 주소: http://{args.host}:{args.port}")
    print(f"  📁 저장 위치: {save_dir.absolute()}")
    print("-" * 60)
    print("  ESP32-CAM 설정:")
    print(f"    rpiServerIP = \"{args.host}\"  // 라즈베리파이 IP로 변경")
    print(f"    rpiServerPort = {args.port}")
    print("-" * 60)
    print("  🌐 원격 서버 전송 설정:")
    print(f"    대상: {REMOTE_UPLOAD_URL}")
    print(f"    전송 주기: {UPLOAD_INTERVAL_SECONDS}초 (1분)")
    print("=" * 60)
    print("\n🚀 서버 시작... (Ctrl+C로 종료)\n")
    
    # 원격 서버 전송 스케줄러 시작
    start_upload_scheduler()
    
    # Flask 서버 실행
    app.run(host=args.host, port=args.port, debug=False, threaded=True)


if __name__ == "__main__":
    main()

