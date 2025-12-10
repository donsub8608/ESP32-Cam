/**
 * MDBT53 (nRF52833) Camera Controller
 * 
 * ESP32-CAM과 시리얼 통신하여 사진을 촬영하고 SD 카드에 저장
 * 
 * 개발 환경: nRF Connect SDK (Zephyr RTOS)
 * 
 * 하드웨어 연결:
 *   MDBT53 TX (P0.06) → ESP32-CAM RX (GPIO3)
 *   MDBT53 RX (P0.08) → ESP32-CAM TX (GPIO1)
 *   GND ↔ GND
 *   
 *   SD 카드 (SPI):
 *   MDBT53 P0.28 → SD_MOSI
 *   MDBT53 P0.29 → SD_MISO
 *   MDBT53 P0.30 → SD_SCK
 *   MDBT53 P0.31 → SD_CS
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/fs/fs.h>
#include <zephyr/storage/disk_access.h>
#include <ff.h>
#include <zephyr/logging/log.h>

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

LOG_MODULE_REGISTER(camera_ctrl, LOG_LEVEL_INF);

/* ===========================
 * 설정값
 * =========================== */
#define UART_DEVICE_NODE DT_NODELABEL(uart0)
#define UART_BUF_SIZE 2048
#define JPEG_MAX_SIZE (512 * 1024)  // 최대 512KB
#define CAPTURE_INTERVAL_SEC 60     // 60초마다 자동 촬영

/* 명령 및 응답 문자열 */
#define CMD_CAPTURE    "CAP\n"
#define CMD_STATUS     "STATUS\n"
#define RESP_IMG       "IMG:"
#define RESP_END       "END:"
#define RESP_OK        "OK:"
#define RESP_ERR       "ERR:"

/* ===========================
 * 전역 변수
 * =========================== */
static const struct device *uart_dev;
static uint8_t uart_rx_buf[UART_BUF_SIZE];
static volatile size_t uart_rx_len = 0;

/* JPEG 수신 버퍼 (PSRAM이 없으므로 정적 할당) */
static uint8_t jpeg_buffer[JPEG_MAX_SIZE] __attribute__((aligned(4)));
static volatile size_t jpeg_len = 0;

/* 파일 시스템 */
static FATFS fat_fs;
static struct fs_mount_t mp = {
    .type = FS_FATFS,
    .fs_data = &fat_fs,
};

/* 파일 카운터 */
static uint32_t file_counter = 0;

/* 상태 플래그 */
static volatile bool receiving_jpeg = false;
static volatile bool jpeg_complete = false;
static volatile size_t expected_jpeg_len = 0;

/* ===========================
 * 함수 프로토타입
 * =========================== */
static int init_uart(void);
static int init_sd_card(void);
static void uart_send(const char *data);
static int request_capture(void);
static int receive_jpeg(void);
static int save_jpeg_to_sd(void);
static uint8_t calculate_checksum(uint8_t *data, size_t len);
static void uart_isr(const struct device *dev, void *user_data);

/* ===========================
 * UART 초기화
 * =========================== */
static int init_uart(void)
{
    uart_dev = DEVICE_DT_GET(UART_DEVICE_NODE);
    if (!device_is_ready(uart_dev)) {
        LOG_ERR("UART device not ready");
        return -ENODEV;
    }

    /* UART 인터럽트 설정 */
    uart_irq_callback_user_data_set(uart_dev, uart_isr, NULL);
    uart_irq_rx_enable(uart_dev);

    LOG_INF("UART initialized");
    return 0;
}

/* ===========================
 * UART 인터럽트 핸들러
 * =========================== */
static void uart_isr(const struct device *dev, void *user_data)
{
    ARG_UNUSED(user_data);

    while (uart_irq_update(dev) && uart_irq_is_pending(dev)) {
        if (uart_irq_rx_ready(dev)) {
            uint8_t c;
            while (uart_fifo_read(dev, &c, 1) > 0) {
                if (receiving_jpeg) {
                    /* JPEG 바이너리 데이터 수신 중 */
                    if (jpeg_len < JPEG_MAX_SIZE) {
                        jpeg_buffer[jpeg_len++] = c;
                    }
                } else {
                    /* 명령/응답 텍스트 수신 */
                    if (uart_rx_len < UART_BUF_SIZE - 1) {
                        uart_rx_buf[uart_rx_len++] = c;
                        uart_rx_buf[uart_rx_len] = '\0';
                    }
                }
            }
        }
    }
}

/* ===========================
 * UART 데이터 전송
 * =========================== */
static void uart_send(const char *data)
{
    while (*data) {
        uart_poll_out(uart_dev, *data++);
    }
}

/* ===========================
 * SD 카드 초기화
 * =========================== */
static int init_sd_card(void)
{
    static const char *disk_mount_pt = "/SD:";
    static const char *disk_pdrv = "SD";
    int ret;

    /* 디스크 접근 초기화 */
    ret = disk_access_init(disk_pdrv);
    if (ret) {
        LOG_ERR("SD card init failed: %d", ret);
        return ret;
    }

    /* 마운트 포인트 설정 */
    mp.mnt_point = disk_mount_pt;

    /* 파일시스템 마운트 */
    ret = fs_mount(&mp);
    if (ret) {
        LOG_ERR("SD mount failed: %d", ret);
        return ret;
    }

    LOG_INF("SD card mounted at %s", disk_mount_pt);
    
    /* 기존 파일 개수 확인하여 카운터 설정 */
    struct fs_dir_t dir;
    struct fs_dirent entry;
    
    fs_dir_t_init(&dir);
    ret = fs_opendir(&dir, disk_mount_pt);
    if (ret == 0) {
        while (fs_readdir(&dir, &entry) == 0 && entry.name[0] != 0) {
            if (strstr(entry.name, "photo_") != NULL) {
                int num;
                if (sscanf(entry.name, "photo_%d.jpg", &num) == 1) {
                    if (num >= file_counter) {
                        file_counter = num + 1;
                    }
                }
            }
        }
        fs_closedir(&dir);
    }
    
    LOG_INF("File counter starts at %d", file_counter);
    return 0;
}

/* ===========================
 * 사진 촬영 요청
 * =========================== */
static int request_capture(void)
{
    LOG_INF("Requesting capture from ESP32-CAM...");
    
    /* 버퍼 초기화 */
    uart_rx_len = 0;
    memset(uart_rx_buf, 0, sizeof(uart_rx_buf));
    jpeg_len = 0;
    receiving_jpeg = false;
    jpeg_complete = false;
    expected_jpeg_len = 0;
    
    /* CAP 명령 전송 */
    uart_send(CMD_CAPTURE);
    
    return 0;
}

/* ===========================
 * JPEG 데이터 수신
 * =========================== */
static int receive_jpeg(void)
{
    int timeout_ms = 30000;  /* 30초 타임아웃 */
    int elapsed = 0;
    
    LOG_INF("Waiting for JPEG data...");
    
    /* Step 1: "IMG:<size>\n" 헤더 대기 */
    while (elapsed < timeout_ms) {
        k_msleep(10);
        elapsed += 10;
        
        /* "IMG:" 헤더 확인 */
        char *img_ptr = strstr((char *)uart_rx_buf, RESP_IMG);
        if (img_ptr) {
            char *newline = strchr(img_ptr, '\n');
            if (newline) {
                /* 크기 파싱 */
                expected_jpeg_len = atoi(img_ptr + 4);
                LOG_INF("Expected JPEG size: %d bytes", expected_jpeg_len);
                
                if (expected_jpeg_len == 0 || expected_jpeg_len > JPEG_MAX_SIZE) {
                    LOG_ERR("Invalid JPEG size: %d", expected_jpeg_len);
                    return -EINVAL;
                }
                
                /* JPEG 수신 모드로 전환 */
                receiving_jpeg = true;
                jpeg_len = 0;
                uart_rx_len = 0;
                break;
            }
        }
        
        /* 에러 응답 확인 */
        if (strstr((char *)uart_rx_buf, RESP_ERR)) {
            LOG_ERR("ESP32-CAM error: %s", uart_rx_buf);
            return -EIO;
        }
    }
    
    if (!receiving_jpeg) {
        LOG_ERR("Timeout waiting for IMG header");
        return -ETIMEDOUT;
    }
    
    /* Step 2: JPEG 바이너리 데이터 수신 */
    elapsed = 0;
    while (elapsed < timeout_ms) {
        k_msleep(10);
        elapsed += 10;
        
        /* 예상 크기만큼 수신했는지 확인 */
        if (jpeg_len >= expected_jpeg_len) {
            /* 추가로 "END:" 마커 대기 */
            k_msleep(100);
            
            /* END 마커와 체크섬 확인 (마지막 몇 바이트에 있음) */
            receiving_jpeg = false;
            
            /* JPEG 끝 부분에서 "END:" 찾기 */
            /* 실제 JPEG 데이터 뒤에 "\nEND:XX\n" 형태로 옴 */
            if (jpeg_len > 10) {
                /* 마지막 10바이트에서 END 마커 검색 */
                char *end_marker = NULL;
                for (int i = jpeg_len - 10; i < jpeg_len - 4; i++) {
                    if (i >= 0 && memcmp(&jpeg_buffer[i], "\nEND:", 5) == 0) {
                        end_marker = (char *)&jpeg_buffer[i];
                        /* 실제 JPEG 데이터 길이 조정 */
                        jpeg_len = i;
                        break;
                    }
                }
                
                if (end_marker) {
                    /* 체크섬 확인 */
                    uint8_t received_checksum;
                    if (sscanf(end_marker + 5, "%02hhX", &received_checksum) == 1) {
                        uint8_t calc_checksum = calculate_checksum(jpeg_buffer, jpeg_len);
                        if (calc_checksum == received_checksum) {
                            LOG_INF("Checksum OK! JPEG received: %d bytes", jpeg_len);
                            jpeg_complete = true;
                            return 0;
                        } else {
                            LOG_WRN("Checksum mismatch: expected %02X, got %02X", 
                                    received_checksum, calc_checksum);
                            /* 체크섬이 틀려도 저장 시도 */
                            jpeg_complete = true;
                            return 0;
                        }
                    }
                }
            }
            
            /* END 마커 없어도 데이터 완료로 처리 */
            LOG_WRN("END marker not found, using received data");
            jpeg_complete = true;
            return 0;
        }
        
        /* 진행 상황 로그 (10% 단위) */
        static int last_percent = 0;
        int percent = (jpeg_len * 100) / expected_jpeg_len;
        if (percent / 10 > last_percent / 10) {
            LOG_INF("Receiving: %d%%", percent);
            last_percent = percent;
        }
    }
    
    LOG_ERR("Timeout receiving JPEG data");
    receiving_jpeg = false;
    return -ETIMEDOUT;
}

/* ===========================
 * JPEG를 SD 카드에 저장
 * =========================== */
static int save_jpeg_to_sd(void)
{
    if (!jpeg_complete || jpeg_len == 0) {
        LOG_ERR("No JPEG data to save");
        return -ENODATA;
    }
    
    char filename[64];
    snprintf(filename, sizeof(filename), "/SD:/photo_%04d.jpg", file_counter);
    
    struct fs_file_t file;
    fs_file_t_init(&file);
    
    int ret = fs_open(&file, filename, FS_O_CREATE | FS_O_WRITE);
    if (ret) {
        LOG_ERR("Failed to create file %s: %d", filename, ret);
        return ret;
    }
    
    /* 데이터 쓰기 (청크 단위) */
    const size_t chunk_size = 4096;
    size_t written = 0;
    
    while (written < jpeg_len) {
        size_t to_write = MIN(chunk_size, jpeg_len - written);
        ssize_t result = fs_write(&file, &jpeg_buffer[written], to_write);
        if (result < 0) {
            LOG_ERR("Write error: %d", result);
            fs_close(&file);
            return result;
        }
        written += result;
    }
    
    fs_close(&file);
    
    LOG_INF("Saved: %s (%d bytes)", filename, jpeg_len);
    file_counter++;
    
    return 0;
}

/* ===========================
 * XOR 체크섬 계산
 * =========================== */
static uint8_t calculate_checksum(uint8_t *data, size_t len)
{
    uint8_t checksum = 0;
    for (size_t i = 0; i < len; i++) {
        checksum ^= data[i];
    }
    return checksum;
}

/* ===========================
 * 캡처 및 저장 워크플로우
 * =========================== */
static int capture_and_save(void)
{
    int ret;
    
    /* 촬영 요청 */
    ret = request_capture();
    if (ret) {
        LOG_ERR("Capture request failed: %d", ret);
        return ret;
    }
    
    /* JPEG 수신 */
    ret = receive_jpeg();
    if (ret) {
        LOG_ERR("JPEG receive failed: %d", ret);
        return ret;
    }
    
    /* SD 카드에 저장 */
    ret = save_jpeg_to_sd();
    if (ret) {
        LOG_ERR("Save to SD failed: %d", ret);
        return ret;
    }
    
    return 0;
}

/* ===========================
 * 버튼 인터럽트 핸들러 (수동 촬영용)
 * =========================== */
#define BUTTON_NODE DT_ALIAS(sw0)
#if DT_NODE_EXISTS(BUTTON_NODE)

static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(BUTTON_NODE, gpios);
static struct gpio_callback button_cb_data;

static void button_pressed(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
    LOG_INF("Button pressed - capturing photo");
    /* 실제 캡처는 메인 루프에서 처리 */
}

static int init_button(void)
{
    if (!gpio_is_ready_dt(&button)) {
        LOG_WRN("Button device not ready");
        return -ENODEV;
    }
    
    gpio_pin_configure_dt(&button, GPIO_INPUT);
    gpio_pin_interrupt_configure_dt(&button, GPIO_INT_EDGE_TO_ACTIVE);
    gpio_init_callback(&button_cb_data, button_pressed, BIT(button.pin));
    gpio_add_callback(button.port, &button_cb_data);
    
    LOG_INF("Button initialized");
    return 0;
}

#endif

/* ===========================
 * 메인 함수
 * =========================== */
int main(void)
{
    int ret;
    
    LOG_INF("=== MDBT53 Camera Controller ===");
    LOG_INF("Initializing...");
    
    /* UART 초기화 */
    ret = init_uart();
    if (ret) {
        LOG_ERR("UART init failed");
        return ret;
    }
    
    /* SD 카드 초기화 */
    ret = init_sd_card();
    if (ret) {
        LOG_ERR("SD card init failed");
        /* SD 없어도 계속 진행 (디버그용) */
    }
    
#if DT_NODE_EXISTS(BUTTON_NODE)
    /* 버튼 초기화 (선택사항) */
    init_button();
#endif
    
    LOG_INF("System ready!");
    
    /* ESP32-CAM 부팅 대기 */
    k_msleep(3000);
    
    /* 상태 확인 */
    uart_send(CMD_STATUS);
    k_msleep(1000);
    if (uart_rx_len > 0) {
        LOG_INF("ESP32-CAM response: %s", uart_rx_buf);
    }
    
    /* 메인 루프: 주기적 촬영 */
    while (1) {
        LOG_INF("--- Starting capture cycle ---");
        
        ret = capture_and_save();
        if (ret) {
            LOG_ERR("Capture cycle failed: %d", ret);
        } else {
            LOG_INF("Capture cycle completed successfully");
        }
        
        /* 다음 촬영까지 대기 */
        LOG_INF("Waiting %d seconds for next capture...", CAPTURE_INTERVAL_SEC);
        k_sleep(K_SECONDS(CAPTURE_INTERVAL_SEC));
    }
    
    return 0;
}

