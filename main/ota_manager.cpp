/*
 * Arctic Heat Pump Controller
 * OTA Update Manager Implementation
 */
#include "ota_manager.h"
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_http_client.h>
#include <esp_https_ota.h>
#include <esp_app_format.h>
#include <esp_system.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <mdns.h>
#include "api_server.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_task_wdt.h>
#include <string.h>
#include <cJSON.h>

static const char* TAG = "ota_manager";

// OTA task stack size
#define OTA_TASK_STACK_SIZE 8192

// GitHub API URL for releases
#define GITHUB_API_URL "https://api.github.com/repos/sslivins/arctic-controller/releases/latest"

// Allowed URL prefixes for firmware updates (security)
#define ALLOWED_OTA_URL_PREFIX "https://github.com/sslivins/arctic-controller/"
#define ALLOWED_OTA_URL_PREFIX2 "https://objects.githubusercontent.com/"

bool ota_mgr_is_url_allowed(const char* url)
{
    if (url == NULL || strlen(url) == 0) {
        return false;
    }
    // Allow both GitHub repo URLs and GitHub's CDN (objects.githubusercontent.com)
    return strncmp(url, ALLOWED_OTA_URL_PREFIX, strlen(ALLOWED_OTA_URL_PREFIX)) == 0 ||
           strncmp(url, ALLOWED_OTA_URL_PREFIX2, strlen(ALLOWED_OTA_URL_PREFIX2)) == 0;
}

// Current OTA status
static ota_status_t ota_status = {
    .state = OTA_STATE_IDLE,
    .progress_percent = 0,
    .bytes_downloaded = 0,
    .total_bytes = 0,
    .error_msg = "",
    .current_version = "",
    .new_version = ""
};

// Cached release info
static ota_release_info_t release_info = {
    .update_available = false,
    .latest_version = "",
    .download_url = "",
    .release_notes = "",
    .published_at = ""
};

// URL for current update
static char update_url[256] = "";

// Mutex for status access
static SemaphoreHandle_t status_mutex = NULL;

// Forward declarations
static void ota_task(void* pvParameter);

// OTA watchdog note (ESP32-P4): the earlier manual OTA erased the whole partition
// up front, masking interrupts for seconds and starving the FreeRTOS idle tasks long
// enough for the Task WDT to hard-reset the device mid-OTA. That drove a hack that
// unsubscribed the idle tasks from the Task WDT for the OTA duration -- but the IDF
// idle hook keeps calling esp_task_wdt_reset() on the unsubscribed idle task, spewing
// "E task_wdt: esp_task_wdt_reset(707): task not found" at ~160/sec whenever the OTA
// blocked, and it disabled real idle-starvation protection.
//
// The root cause is now fixed properly: OTA uses OTA_WITH_SEQUENTIAL_WRITES, so each
// esp_ota_write() erases at most one ~4KB sector (~50ms masked) and the idle tasks run
// between sectors and feed the Task WDT normally. Idle starvation can no longer occur,
// so these are intentionally NO-OPS -- the idle tasks stay Task-WDT-protected. The
// call sites are kept so the OTA control flow is unchanged.
static void ota_twdt_pause_idle(void)
{
    // no-op: see note above (OTA_WITH_SEQUENTIAL_WRITES removes the idle-starvation risk)
}

static void ota_twdt_resume_idle(void)
{
    // no-op: idle tasks are never unsubscribed anymore
}

bool ota_mgr_init(void)
{
    ESP_LOGI(TAG, "Initializing OTA manager...");
    
    // Create mutex
    status_mutex = xSemaphoreCreateMutex();
    if (status_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return false;
    }
    
    // Get current app version
    const esp_app_desc_t* app_desc = esp_app_get_description();
    if (app_desc) {
        strncpy(ota_status.current_version, app_desc->version, sizeof(ota_status.current_version) - 1);
        ESP_LOGI(TAG, "Current firmware version: %s", ota_status.current_version);
    }
    
    // Log partition info
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (running) {
        ESP_LOGI(TAG, "Running from partition: %s @ 0x%lx", running->label, running->address);
    }
    
    // Check if this is first boot after OTA
    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            ESP_LOGW(TAG, "First boot after OTA - firmware pending verification");
        }
    }
    
    return true;
}

bool ota_mgr_start_update(const char* url)
{
    if (url == NULL || strlen(url) == 0) {
        ESP_LOGE(TAG, "Invalid URL");
        return false;
    }
    
    // Security: Only allow updates from official GitHub repository
    if (strncmp(url, ALLOWED_OTA_URL_PREFIX, strlen(ALLOWED_OTA_URL_PREFIX)) != 0) {
        ESP_LOGE(TAG, "URL not allowed: must start with %s", ALLOWED_OTA_URL_PREFIX);
        return false;
    }
    
    xSemaphoreTake(status_mutex, portMAX_DELAY);
    
    // Check for any ongoing OTA operation (including file upload)
    if (ota_status.state == OTA_STATE_UPLOADING ||
        ota_status.state == OTA_STATE_DOWNLOADING || 
        ota_status.state == OTA_STATE_VERIFYING) {
        ESP_LOGW(TAG, "OTA already in progress (state=%d)", ota_status.state);
        xSemaphoreGive(status_mutex);
        return false;
    }
    
    // Store URL and reset status
    strncpy(update_url, url, sizeof(update_url) - 1);
    update_url[sizeof(update_url) - 1] = '\0';
    
    ota_status.state = OTA_STATE_DOWNLOADING;
    ota_status.progress_percent = 0;
    ota_status.bytes_downloaded = 0;
    ota_status.total_bytes = 0;
    ota_status.error_msg[0] = '\0';
    ota_status.new_version[0] = '\0';
    
    xSemaphoreGive(status_mutex);
    
    ESP_LOGI(TAG, "Starting OTA update from: %s", url);
    
    // Create OTA task
    // Lower priority than LVGL (5) to avoid display glitches during flash writes
    BaseType_t ret = xTaskCreate(ota_task, "ota_task", OTA_TASK_STACK_SIZE, NULL, 3, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create OTA task");
        xSemaphoreTake(status_mutex, portMAX_DELAY);
        ota_status.state = OTA_STATE_FAILED;
        strncpy(ota_status.error_msg, "Failed to start OTA task", sizeof(ota_status.error_msg));
        xSemaphoreGive(status_mutex);
        return false;
    }
    
    return true;
}

ota_status_t ota_mgr_get_status(void)
{
    ota_status_t status;
    xSemaphoreTake(status_mutex, portMAX_DELAY);
    memcpy(&status, &ota_status, sizeof(ota_status_t));
    xSemaphoreGive(status_mutex);
    return status;
}

bool ota_mgr_is_busy(void)
{
    xSemaphoreTake(status_mutex, portMAX_DELAY);
    bool busy = (ota_status.state == OTA_STATE_UPLOADING ||
                 ota_status.state == OTA_STATE_DOWNLOADING || 
                 ota_status.state == OTA_STATE_VERIFYING);
    xSemaphoreGive(status_mutex);
    return busy;
}

bool ota_mgr_try_lock_upload(void)
{
    xSemaphoreTake(status_mutex, portMAX_DELAY);
    
    // Check if any OTA operation is in progress
    if (ota_status.state == OTA_STATE_UPLOADING ||
        ota_status.state == OTA_STATE_DOWNLOADING ||
        ota_status.state == OTA_STATE_VERIFYING) {
        ESP_LOGW(TAG, "Cannot start upload: OTA already in progress (state=%d)", ota_status.state);
        xSemaphoreGive(status_mutex);
        return false;
    }
    
    // Acquire lock by setting state
    ota_status.state = OTA_STATE_UPLOADING;
    ota_status.progress_percent = 0;
    ota_status.error_msg[0] = '\0';
    
    xSemaphoreGive(status_mutex);
    ESP_LOGI(TAG, "Upload lock acquired");
    return true;
}

void ota_mgr_unlock_upload(void)
{
    xSemaphoreTake(status_mutex, portMAX_DELAY);
    
    // Only unlock if we're in uploading state (don't disturb other states)
    if (ota_status.state == OTA_STATE_UPLOADING) {
        ota_status.state = OTA_STATE_IDLE;
        ESP_LOGI(TAG, "Upload lock released");
    }
    
    xSemaphoreGive(status_mutex);
}

void ota_mgr_reboot(void)
{
    ESP_LOGI(TAG, "Rebooting to apply OTA update...");
    vTaskDelay(pdMS_TO_TICKS(500));  // Allow log to flush
    esp_restart();
}

void ota_mgr_mark_valid(void)
{
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            ESP_LOGI(TAG, "Marking current firmware as valid — rollback cancelled");
            esp_ota_mark_app_valid_cancel_rollback();
        } else {
            ESP_LOGI(TAG, "Firmware already validated (state=%d)", ota_state);
        }
    }
}

bool ota_mgr_is_pending_verify(void)
{
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        return ota_state == ESP_OTA_IMG_PENDING_VERIFY;
    }
    return false;
}

void ota_mgr_get_partition_info(char* label, uint32_t* address, uint32_t* size)
{
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (running) {
        if (label) {
            strncpy(label, running->label, 16);
        }
        if (address) {
            *address = running->address;
        }
        if (size) {
            *size = running->size;
        }
    }
}

// HTTP event handler for progress tracking
static esp_err_t http_event_handler(esp_http_client_event_t* evt)
{
    switch (evt->event_id) {
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGD(TAG, "HTTP connected");
            break;
        case HTTP_EVENT_HEADER_SENT:
            break;
        case HTTP_EVENT_ON_HEADER:
            // Lightweight handler used for the OTA HEAD request (size probe) and as
            // the default handler outside the streaming download. The streaming GET
            // uses ota_stream_event_handler instead.
            ESP_LOGD(TAG, "Header: %s = %s", evt->header_key, evt->header_value);
            break;
        case HTTP_EVENT_ON_DATA:
            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGD(TAG, "HTTP finished");
            break;
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGD(TAG, "HTTP disconnected");
            break;
        default:
            break;
    }
    return ESP_OK;
}

// Streaming context + event handler for the manual, pre-erase OTA download.
//
// Root cause of the post-OTA brick (ESP32-P4 host + ESP32-C6 esp_hosted 1.4.0 over
// SDIO): any FLASH operation (erase or write) executes on CPU0 with the cache disabled
// and interrupts masked. While that window is open the host SDIO-RX path cannot run, so
// any bytes the C6 is pushing up the link (inbound TLS handshakes from status polling /
// the HA websocket, mDNS, etc.) back up and wedge the C6. CPU0 then stays stuck long
// enough (>2x INT_WDT) to trip the Interrupt Watchdog stage-1 HARD reset -- reported as
// rst:0x7 / ESP_RST_WDT with no backtrace (confirmed: Core1 parked in panic_handler.c
// busy_wait for PANIC_RSN_INTWDT_CPU0). The device comes back on a half-written slot.
// Interleaving flash with an active download (esp_https_ota, or sequential-write, or a
// pre-erase while servers stay up) all keep SDIO traffic flowing during flash -> brick.
//
// The only robust host-side fix (no esp_hosted bump, no C6 reflash -- esp_hosted is
// pinned to 1.4.0 to match the Tab5's pre-flashed C6) is to remove ALL concurrency:
//   1. HEAD the URL for the image size.
//   2. Download the ENTIRE image into a PSRAM buffer -- network up, but ZERO flash
//      access, so nothing ever blocks the SDIO-RX path (proven safe).
//   3. Tear the network down (stop HTTPS/HA-websocket/mDNS servers) so no inbound SDIO
//      traffic can arrive.
//   4. Only now erase + write the buffered image to flash. With the network quiesced,
//      the cache-disabled/interrupts-masked flash windows starve nothing -> no wedge,
//      no INT_WDT. Reboot on success OR failure (servers are down either way).
// The streaming SDIO-RX buffer itself must be PSRAM-backed (esp_hosted os_wrapper patch)
// so step 2 doesn't exhaust internal DMA SRAM under OTA heap pressure.
typedef struct {
    uint8_t* buf;            // PSRAM destination for the whole image
    size_t   buf_cap;        // allocated capacity of buf (bytes)
    int image_size;          // expected total from HEAD, 0 if unknown
    int bytes_received;
    bool version_logged;
    int last_logged_decile;
    esp_err_t err;           // sticky: set on overflow / internal error
} ota_buf_ctx_t;

static esp_err_t ota_buffer_event_handler(esp_http_client_event_t* evt)
{
    if (evt->event_id != HTTP_EVENT_ON_DATA) {
        return ESP_OK;
    }
    ota_buf_ctx_t* ctx = (ota_buf_ctx_t*)evt->user_data;
    if (ctx == NULL || ctx->err != ESP_OK) {
        return ESP_OK;
    }

    // Best-effort: surface the incoming firmware version from the app descriptor,
    // which lives at offset 0x20 in the image (after the image header + first segment
    // header). The first data chunk always contains it whole.
    const size_t desc_off = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
    if (!ctx->version_logged && ctx->bytes_received == 0 &&
        evt->data_len >= (int)(desc_off + sizeof(esp_app_desc_t))) {
        const esp_app_desc_t* desc =
            (const esp_app_desc_t*)((const uint8_t*)evt->data + desc_off);
        if (desc->magic_word == 0xABCD5432) {  // ESP_APP_DESC_MAGIC_WORD
            xSemaphoreTake(status_mutex, portMAX_DELAY);
            strncpy(ota_status.new_version, desc->version, sizeof(ota_status.new_version) - 1);
            xSemaphoreGive(status_mutex);
            ESP_LOGI(TAG, "New firmware version: %s", desc->version);
        }
        ctx->version_logged = true;
    }

    // Copy straight into the PSRAM buffer -- NO flash access here. Writing flash while
    // the C6 streams bytes into the SDIO RX path is exactly what wedges the link and
    // trips the INT_WDT (rst:0x7). We buffer the whole image, then tear the network
    // down, then touch flash. Guard against a body larger than the buffer (HEAD/GET
    // size mismatch) so we never overflow the heap allocation.
    if ((size_t)ctx->bytes_received + (size_t)evt->data_len > ctx->buf_cap) {
        ESP_LOGE(TAG, "Image exceeds buffer (%d + %d > %u bytes)",
                 ctx->bytes_received, evt->data_len, (unsigned)ctx->buf_cap);
        ctx->err = ESP_ERR_INVALID_SIZE;
        return ESP_FAIL;
    }
    memcpy(ctx->buf + ctx->bytes_received, evt->data, evt->data_len);
    ctx->bytes_received += evt->data_len;

    xSemaphoreTake(status_mutex, portMAX_DELAY);
    ota_status.bytes_downloaded = ctx->bytes_received;
    if (ctx->image_size > 0) {
        int progress = (ctx->bytes_received * 100) / ctx->image_size;
        ota_status.progress_percent = (progress > 99) ? 99 : progress;
    }
    int pct = ota_status.progress_percent;
    xSemaphoreGive(status_mutex);

    int decile = pct / 10;
    if (decile != ctx->last_logged_decile) {
        ctx->last_logged_decile = decile;
        ESP_LOGI(TAG, "OTA download: %d%% (%d bytes)", pct, ctx->bytes_received);
    }
    return ESP_OK;
}

static void ota_task(void* pvParameter)
{
    ESP_LOGI(TAG, "OTA task started");
    
    // Reduce verbosity of certificate bundle logging
    esp_log_level_set("esp-x509-crt-bundle", ESP_LOG_WARN);
    
    esp_err_t err;
    
    // Configure HTTP client
    esp_http_client_config_t config = {};
    config.url = update_url;
    config.event_handler = http_event_handler;
    config.timeout_ms = 60000;  // 60 second timeout for large files
    config.keep_alive_enable = true;
    config.buffer_size = 8192;  // Larger buffer for HTTPS TLS handshake
    config.buffer_size_tx = 4096;  // Larger TX buffer for GitHub
    
    // Check if HTTPS
    if (strncmp(update_url, "https://", 8) == 0) {
        // Use ESP's built-in certificate bundle for HTTPS verification
        config.crt_bundle_attach = esp_crt_bundle_attach;
        config.skip_cert_common_name_check = false;
        ESP_LOGI(TAG, "HTTPS: Using certificate bundle for verification");
    }
    
    // GitHub uses redirects for release downloads (to objects.githubusercontent.com)
    config.disable_auto_redirect = false;
    config.max_redirection_count = 10;

    // ---- Manual, buffer-then-flash OTA (P4+C6 SDIO-vs-flash brick fix) ----
    // See ota_buffer_event_handler above for the full rationale. Phases:
    //   1. HEAD the URL to learn the image size (short, minimal network traffic).
    //   2. Download the WHOLE image into a PSRAM buffer -- network up, no flash access.
    //   3. Quiesce the network (stop servers + mDNS) so no inbound SDIO traffic remains.
    //   4. esp_ota_begin() (erase) + esp_ota_write() from the buffer + finalize, all with
    //      the network down so flash ops can't starve the SDIO-RX path. Reboot either way.
    // Keep the Task-WDT idle unsubscribe as defense during the flash-write phase.
    ota_twdt_pause_idle();

    const esp_partition_t* update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        ESP_LOGE(TAG, "No OTA partition available");
        ota_twdt_resume_idle();
        xSemaphoreTake(status_mutex, portMAX_DELAY);
        ota_status.state = OTA_STATE_FAILED;
        strncpy(ota_status.error_msg, "No OTA partition", sizeof(ota_status.error_msg));
        xSemaphoreGive(status_mutex);
        vTaskDelete(NULL);
        return;
    }

    // Phase 1: HEAD for the image size (follows GitHub's redirect to the CDN).
    int image_size = 0;
    config.method = HTTP_METHOD_HEAD;
    esp_http_client_handle_t head_client = esp_http_client_init(&config);
    if (head_client != NULL) {
        esp_err_t herr = esp_http_client_perform(head_client);
        if (herr == ESP_OK) {
            int hstatus = esp_http_client_get_status_code(head_client);
            int64_t clen = esp_http_client_get_content_length(head_client);
            ESP_LOGI(TAG, "HEAD status=%d content-length=%d", hstatus, (int)clen);
            if (clen > 0) {
                image_size = (int)clen;
            }
        } else {
            ESP_LOGW(TAG, "HEAD failed: %s (will erase whole partition)", esp_err_to_name(herr));
        }
        esp_http_client_cleanup(head_client);
    }
    config.method = HTTP_METHOD_GET;

    xSemaphoreTake(status_mutex, portMAX_DELAY);
    ota_status.total_bytes = (image_size > 0) ? image_size : (1600 * 1024);
    xSemaphoreGive(status_mutex);

    // Phase 2: allocate a PSRAM buffer for the WHOLE image (see the design note above
    // ota_buffer_event_handler). Cap it at the HEAD-advertised size, or the partition
    // size if HEAD gave us nothing. A body larger than this is rejected in the handler.
    size_t buf_cap = (image_size > 0) ? (size_t)image_size : (size_t)update_partition->size;
    if (buf_cap > update_partition->size) {
        ESP_LOGE(TAG, "Advertised image (%u) larger than partition (%u)",
                 (unsigned)buf_cap, (unsigned)update_partition->size);
        ota_twdt_resume_idle();
        xSemaphoreTake(status_mutex, portMAX_DELAY);
        ota_status.state = OTA_STATE_FAILED;
        strncpy(ota_status.error_msg, "Image larger than partition", sizeof(ota_status.error_msg));
        xSemaphoreGive(status_mutex);
        vTaskDelete(NULL);
        return;
    }
    uint8_t* img_buf = (uint8_t*)heap_caps_malloc(buf_cap, MALLOC_CAP_SPIRAM);
    if (img_buf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate %u-byte PSRAM image buffer", (unsigned)buf_cap);
        ota_twdt_resume_idle();
        xSemaphoreTake(status_mutex, portMAX_DELAY);
        ota_status.state = OTA_STATE_FAILED;
        strncpy(ota_status.error_msg, "Out of memory for image buffer", sizeof(ota_status.error_msg));
        xSemaphoreGive(status_mutex);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "Allocated %u-byte PSRAM image buffer", (unsigned)buf_cap);

    // Phase 3: download the ENTIRE image into PSRAM (network up, ZERO flash access).
    ota_buf_ctx_t buf_ctx = {};
    buf_ctx.buf = img_buf;
    buf_ctx.buf_cap = buf_cap;
    buf_ctx.image_size = image_size;
    buf_ctx.last_logged_decile = -1;
    buf_ctx.err = ESP_OK;

    config.event_handler = ota_buffer_event_handler;
    config.user_data = &buf_ctx;
    esp_http_client_handle_t dl_client = esp_http_client_init(&config);
    if (dl_client == NULL) {
        ESP_LOGE(TAG, "Failed to init download client");
        heap_caps_free(img_buf);
        ota_twdt_resume_idle();
        xSemaphoreTake(status_mutex, portMAX_DELAY);
        ota_status.state = OTA_STATE_FAILED;
        strncpy(ota_status.error_msg, "HTTP init failed", sizeof(ota_status.error_msg));
        xSemaphoreGive(status_mutex);
        vTaskDelete(NULL);
        return;
    }

    err = esp_http_client_perform(dl_client);
    int http_status = esp_http_client_get_status_code(dl_client);
    esp_http_client_cleanup(dl_client);
    config.event_handler = http_event_handler;
    config.user_data = NULL;

    if (err != ESP_OK || buf_ctx.err != ESP_OK || http_status != 200) {
        ESP_LOGE(TAG, "Download failed: http=%d perform=%s buf=%s",
                 http_status, esp_err_to_name(err), esp_err_to_name(buf_ctx.err));
        heap_caps_free(img_buf);
        ota_twdt_resume_idle();
        xSemaphoreTake(status_mutex, portMAX_DELAY);
        ota_status.state = OTA_STATE_FAILED;
        snprintf(ota_status.error_msg, sizeof(ota_status.error_msg),
                 "Download failed (http %d)", http_status);
        xSemaphoreGive(status_mutex);
        vTaskDelete(NULL);
        return;
    }

    int img_len = buf_ctx.bytes_received;
    if (image_size > 0 && img_len != image_size) {
        ESP_LOGE(TAG, "Incomplete download: %d/%d bytes", img_len, image_size);
        heap_caps_free(img_buf);
        ota_twdt_resume_idle();
        xSemaphoreTake(status_mutex, portMAX_DELAY);
        ota_status.state = OTA_STATE_FAILED;
        strncpy(ota_status.error_msg, "Incomplete data received", sizeof(ota_status.error_msg));
        xSemaphoreGive(status_mutex);
        vTaskDelete(NULL);
        return;
    }

    // Basic sanity check before we erase the target slot: a valid ESP app image starts
    // with magic byte 0xE9. (esp_ota_end() still does the full hash/signature check.)
    if (img_len < 1024 || img_buf[0] != 0xE9) {
        ESP_LOGE(TAG, "Downloaded data is not a valid image (len=%d, magic=0x%02x)",
                 img_len, img_len > 0 ? img_buf[0] : 0);
        heap_caps_free(img_buf);
        ota_twdt_resume_idle();
        xSemaphoreTake(status_mutex, portMAX_DELAY);
        ota_status.state = OTA_STATE_FAILED;
        strncpy(ota_status.error_msg, "Invalid firmware image", sizeof(ota_status.error_msg));
        xSemaphoreGive(status_mutex);
        vTaskDelete(NULL);
        return;
    }

    xSemaphoreTake(status_mutex, portMAX_DELAY);
    ota_status.state = OTA_STATE_VERIFYING;
    ota_status.progress_percent = 100;
    xSemaphoreGive(status_mutex);
    ESP_LOGI(TAG, "Download complete (%d bytes) buffered in PSRAM", img_len);

    // ---- Point of no return: from here we quiesce the network and touch flash. ----
    // Any failure below leaves the servers stopped, so we MUST reboot rather than
    // vTaskDelete() (which would strand the device unreachable). A failed flash still
    // leaves the OTHER (valid) slot bootable, so the bootloader comes back on it.

    // Phase 4: QUIESCE all inbound SDIO traffic so nothing arrives while the flash ops
    // hold the cache disabled / interrupts masked. Stop the HTTPS + HA-websocket + WS
    // servers and mDNS, then let any queued RX drain briefly.
    ESP_LOGI(TAG, "Quiescing network before flashing (stopping servers + mDNS)...");
    api_server_stop();
    mdns_free();
    vTaskDelay(pdMS_TO_TICKS(500));

    // Phase 5: erase + write the buffered image from PSRAM. With the network down the
    // long erase can no longer starve the SDIO-RX path -> no C6 wedge, no INT_WDT.
    esp_ota_handle_t ota_handle = 0;
    ESP_LOGI(TAG, "Erasing + writing %d bytes to '%s'...", img_len, update_partition->label);
    err = esp_ota_begin(update_partition, img_len, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s -- rebooting", esp_err_to_name(err));
        heap_caps_free(img_buf);
        esp_restart();
    }

    const int WRITE_CHUNK = 64 * 1024;
    for (int off = 0; off < img_len; off += WRITE_CHUNK) {
        int n = (img_len - off < WRITE_CHUNK) ? (img_len - off) : WRITE_CHUNK;
        err = esp_ota_write(ota_handle, img_buf + off, n);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed at %d: %s -- rebooting", off, esp_err_to_name(err));
            esp_ota_abort(ota_handle);
            heap_caps_free(img_buf);
            esp_restart();
        }
    }
    heap_caps_free(img_buf);

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s -- rebooting", esp_err_to_name(err));
        esp_restart();
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s -- rebooting", esp_err_to_name(err));
        esp_restart();
    }

    // Success!
    ESP_LOGI(TAG, "OTA update successful! Rebooting...");
    xSemaphoreTake(status_mutex, portMAX_DELAY);
    ota_status.state = OTA_STATE_READY_TO_REBOOT;
    xSemaphoreGive(status_mutex);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();

    // Never reached
    vTaskDelete(NULL);
}

// Compare semantic versions (returns: -1 if v1<v2, 0 if equal, 1 if v1>v2)
int ota_mgr_compare_versions(const char* v1, const char* v2)
{
    int major1 = 0, minor1 = 0, patch1 = 0;
    int major2 = 0, minor2 = 0, patch2 = 0;
    
    // Skip 'v' prefix if present
    if (v1[0] == 'v' || v1[0] == 'V') v1++;
    if (v2[0] == 'v' || v2[0] == 'V') v2++;
    
    sscanf(v1, "%d.%d.%d", &major1, &minor1, &patch1);
    sscanf(v2, "%d.%d.%d", &major2, &minor2, &patch2);
    
    if (major1 != major2) return (major1 > major2) ? 1 : -1;
    if (minor1 != minor2) return (minor1 > minor2) ? 1 : -1;
    if (patch1 != patch2) return (patch1 > patch2) ? 1 : -1;
    return 0;
}

// HTTP response buffer for GitHub API
static char* http_response_buffer = NULL;
static size_t http_response_len = 0;
static size_t http_response_capacity = 0;

static esp_err_t github_http_event_handler(esp_http_client_event_t* evt)
{
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (http_response_buffer == NULL) {
                // Initial allocation
                http_response_capacity = 4096;
                http_response_buffer = (char*)malloc(http_response_capacity);
                http_response_len = 0;
            }
            // Grow buffer if needed
            if (http_response_len + evt->data_len >= http_response_capacity) {
                http_response_capacity = http_response_len + evt->data_len + 1024;
                http_response_buffer = (char*)realloc(http_response_buffer, http_response_capacity);
            }
            if (http_response_buffer) {
                memcpy(http_response_buffer + http_response_len, evt->data, evt->data_len);
                http_response_len += evt->data_len;
                http_response_buffer[http_response_len] = '\0';
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

bool ota_mgr_check_github_releases(ota_release_info_t* info)
{
    ESP_LOGI(TAG, "Checking GitHub for updates...");
    
    // Reset release info
    memset(&release_info, 0, sizeof(release_info));
    
    // Free any previous response buffer
    if (http_response_buffer) {
        free(http_response_buffer);
        http_response_buffer = NULL;
    }
    http_response_len = 0;
    http_response_capacity = 0;
    
    // Configure HTTP client for GitHub API
    esp_http_client_config_t config = {};
    config.url = GITHUB_API_URL;
    config.event_handler = github_http_event_handler;
    config.timeout_ms = 15000;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.buffer_size = 2048;
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        return false;
    }
    
    // GitHub API requires User-Agent header
    esp_http_client_set_header(client, "User-Agent", "arctic-controller");
    esp_http_client_set_header(client, "Accept", "application/vnd.github.v3+json");
    
    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    
    if (err != ESP_OK || status_code != 200) {
        ESP_LOGE(TAG, "GitHub API request failed: %s (HTTP %d)", esp_err_to_name(err), status_code);
        if (http_response_buffer) {
            free(http_response_buffer);
            http_response_buffer = NULL;
        }
        return false;
    }
    
    ESP_LOGI(TAG, "Got GitHub response (%d bytes)", http_response_len);
    
    // Parse JSON response
    cJSON* root = cJSON_Parse(http_response_buffer);
    free(http_response_buffer);
    http_response_buffer = NULL;
    
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to parse GitHub JSON response");
        return false;
    }
    
    // Extract version tag
    cJSON* tag_name = cJSON_GetObjectItem(root, "tag_name");
    if (tag_name && cJSON_IsString(tag_name)) {
        strncpy(release_info.latest_version, tag_name->valuestring, sizeof(release_info.latest_version) - 1);
    }
    
    // Extract published date
    cJSON* published_at = cJSON_GetObjectItem(root, "published_at");
    if (published_at && cJSON_IsString(published_at)) {
        strncpy(release_info.published_at, published_at->valuestring, sizeof(release_info.published_at) - 1);
    }
    
    // Extract release notes (body), truncate if needed
    cJSON* body = cJSON_GetObjectItem(root, "body");
    if (body && cJSON_IsString(body)) {
        strncpy(release_info.release_notes, body->valuestring, sizeof(release_info.release_notes) - 1);
    }
    
    // Find the .bin asset in assets array
    cJSON* assets = cJSON_GetObjectItem(root, "assets");
    if (assets && cJSON_IsArray(assets)) {
        cJSON* asset;
        cJSON_ArrayForEach(asset, assets) {
            cJSON* name = cJSON_GetObjectItem(asset, "name");
            if (name && cJSON_IsString(name)) {
                // Look for .bin file
                if (strstr(name->valuestring, ".bin") != NULL) {
                    cJSON* download_url = cJSON_GetObjectItem(asset, "browser_download_url");
                    if (download_url && cJSON_IsString(download_url)) {
                        strncpy(release_info.download_url, download_url->valuestring, sizeof(release_info.download_url) - 1);
                        ESP_LOGI(TAG, "Found firmware: %s", name->valuestring);
                        break;
                    }
                }
            }
        }
    }
    
    cJSON_Delete(root);
    
    // Compare versions
    if (strlen(release_info.latest_version) > 0 && strlen(ota_status.current_version) > 0) {
        int cmp = ota_mgr_compare_versions(release_info.latest_version, ota_status.current_version);
        release_info.update_available = (cmp > 0);
        ESP_LOGI(TAG, "Current: %s, Latest: %s, Update available: %s",
                 ota_status.current_version, release_info.latest_version,
                 release_info.update_available ? "YES" : "NO");
    }
    
    if (info) {
        memcpy(info, &release_info, sizeof(ota_release_info_t));
    }
    
    return true;
}

const ota_release_info_t* ota_mgr_get_release_info(void)
{
    return &release_info;
}

bool ota_mgr_start_github_update(void)
{
    if (strlen(release_info.download_url) == 0) {
        ESP_LOGE(TAG, "No download URL available - run check first");
        return false;
    }
    
    if (!release_info.update_available) {
        ESP_LOGW(TAG, "No update available");
        return false;
    }
    
    ESP_LOGI(TAG, "Starting GitHub OTA update to %s", release_info.latest_version);
    return ota_mgr_start_update(release_info.download_url);
}
