/*
 * Arctic Heat Pump Controller
 * Home Assistant production WebSocket push transport
 */
#include "ha_websocket.h"

#include "auth_manager.h"
#include "ha_integration.h"

#include <cJSON.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <lwip/sockets.h>
#include <stdio.h>
#include <string.h>

namespace {

constexpr size_t MAX_CLIENTS = 3;
constexpr uint32_t POLL_INTERVAL_MS = 250;
constexpr int64_t HEARTBEAT_INTERVAL_US = 15 * 1000000LL;
constexpr int64_t CLIENT_TIMEOUT_US = 45 * 1000000LL;
constexpr int64_t PERIODIC_SNAPSHOT_US = 30 * 1000000LL;
constexpr int SEND_TIMEOUT_US = 100000;

const char* TAG = "ha_websocket";

struct Client {
    bool active = false;
    bool initial_pending = false;
    int fd = -1;
    uint32_t connection_id = 0;
    uint32_t auth_generation = 0;
    uint64_t last_revision = 0;
    int64_t last_pong_us = 0;
    int64_t last_ping_us = 0;
    int64_t last_snapshot_us = 0;
};

httpd_handle_t s_server = nullptr;
TaskHandle_t s_task = nullptr;
SemaphoreHandle_t s_clients_mutex = nullptr;
SemaphoreHandle_t s_task_done = nullptr;
volatile bool s_running = false;
uint32_t s_next_connection_id = 1;
Client s_clients[MAX_CLIENTS];

bool extractBearerToken(
    httpd_req_t* req,
    char token[AUTH_INTEGRATION_TOKEN_LEN + 1])
{
    static constexpr char PREFIX[] = "Bearer ";
    static constexpr size_t PREFIX_LEN = sizeof(PREFIX) - 1;
    char authorization[PREFIX_LEN + AUTH_INTEGRATION_TOKEN_LEN + 1] = {};
    const size_t header_len =
        httpd_req_get_hdr_value_len(req, "Authorization");
    if (header_len != sizeof(authorization) - 1 ||
        httpd_req_get_hdr_value_str(
            req, "Authorization", authorization,
            sizeof(authorization)) != ESP_OK ||
        strncmp(authorization, PREFIX, PREFIX_LEN) != 0) {
        return false;
    }

    memcpy(
        token,
        authorization + PREFIX_LEN,
        AUTH_INTEGRATION_TOKEN_LEN + 1);
    memset(authorization, 0, sizeof(authorization));
    return true;
}

void clearClientLocked(Client& client)
{
    client = {};
    client.fd = -1;
}

void closeClient(int fd, uint32_t connection_id, const char* reason)
{
    bool should_close = false;
    if (xSemaphoreTake(s_clients_mutex, pdMS_TO_TICKS(250)) == pdTRUE) {
        for (Client& client : s_clients) {
            if (client.active && client.fd == fd &&
                client.connection_id == connection_id) {
                clearClientLocked(client);
                should_close = true;
                break;
            }
        }
        xSemaphoreGive(s_clients_mutex);
    }

    if (should_close && s_server != nullptr) {
        ESP_LOGI(TAG, "Closing client fd=%d: %s", fd, reason);
        httpd_sess_trigger_close(s_server, fd);
    }
}

bool sendFrame(
    const Client& client,
    httpd_ws_type_t type,
    const char* payload,
    size_t payload_len)
{
    if (s_server == nullptr ||
        httpd_ws_get_fd_info(s_server, client.fd) !=
            HTTPD_WS_CLIENT_WEBSOCKET) {
        return false;
    }

    httpd_ws_frame_t frame = {};
    frame.type = type;
    frame.payload =
        reinterpret_cast<uint8_t*>(const_cast<char*>(payload));
    frame.len = payload_len;
    const esp_err_t result =
        httpd_ws_send_data(s_server, client.fd, &frame);
    return result == ESP_OK;
}

char* createHello(const cJSON* snapshot)
{
    const cJSON* revision =
        cJSON_GetObjectItemCaseSensitive(snapshot, "revision");
    cJSON* hello = cJSON_CreateObject();
    if (hello == nullptr) {
        return nullptr;
    }
    cJSON_AddStringToObject(hello, "type", "hello");
    cJSON_AddNumberToObject(
        hello, "protocol_version", arctic::ha::PROTOCOL_VERSION);
    cJSON_AddStringToObject(
        hello, "device_id", arctic::ha::deviceId());
    cJSON_AddStringToObject(hello, "boot_id", arctic::ha::bootId());
    cJSON_AddNumberToObject(
        hello,
        "revision",
        cJSON_IsNumber(revision) ? revision->valuedouble : 0);
    char* serialized = cJSON_PrintUnformatted(hello);
    cJSON_Delete(hello);
    return serialized;
}

char* createSnapshotMessage(cJSON* snapshot)
{
    cJSON* message = cJSON_CreateObject();
    if (message == nullptr) {
        cJSON_Delete(snapshot);
        return nullptr;
    }
    cJSON_AddStringToObject(message, "type", "snapshot");
    cJSON_AddItemToObject(message, "snapshot", snapshot);
    char* serialized = cJSON_PrintUnformatted(message);
    cJSON_Delete(message);
    return serialized;
}

void updateClientAfterSend(
    const Client& sent,
    uint64_t revision,
    int64_t now_us,
    bool initial_completed,
    bool snapshot_sent,
    bool ping_sent)
{
    if (xSemaphoreTake(s_clients_mutex, pdMS_TO_TICKS(250)) != pdTRUE) {
        return;
    }
    for (Client& client : s_clients) {
        if (!client.active || client.fd != sent.fd ||
            client.connection_id != sent.connection_id) {
            continue;
        }
        if (initial_completed) {
            client.initial_pending = false;
        }
        if (snapshot_sent) {
            client.last_revision = revision;
            client.last_snapshot_us = now_us;
        }
        if (ping_sent) {
            client.last_ping_us = now_us;
        }
        break;
    }
    xSemaphoreGive(s_clients_mutex);
}

void closeAllClients()
{
    Client clients[MAX_CLIENTS];
    size_t count = 0;
    if (xSemaphoreTake(s_clients_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        for (Client& client : s_clients) {
            if (client.active) {
                clients[count++] = client;
                clearClientLocked(client);
            }
        }
        xSemaphoreGive(s_clients_mutex);
    }
    for (size_t i = 0; i < count; ++i) {
        if (s_server != nullptr) {
            httpd_sess_trigger_close(s_server, clients[i].fd);
        }
    }
}

void pushTask(void* argument)
{
    (void)argument;
    while (s_running) {
        Client clients[MAX_CLIENTS];
        size_t count = 0;
        if (xSemaphoreTake(
                s_clients_mutex, pdMS_TO_TICKS(250)) == pdTRUE) {
            for (Client& client : s_clients) {
                if (client.active) {
                    clients[count++] = client;
                }
            }
            xSemaphoreGive(s_clients_mutex);
        }

        if (count > 0) {
            const int64_t now_us = esp_timer_get_time();
            const uint32_t auth_generation =
                auth_mgr_get_integration_generation();
            bool eligible[MAX_CLIENTS] = {};

            for (size_t i = 0; i < count; ++i) {
                Client& client = clients[i];
                if (client.auth_generation != auth_generation) {
                    closeClient(
                        client.fd,
                        client.connection_id,
                        "credential changed");
                    continue;
                }
                if (now_us - client.last_pong_us >
                    CLIENT_TIMEOUT_US) {
                    closeClient(
                        client.fd,
                        client.connection_id,
                        "heartbeat timeout");
                    continue;
                }
                eligible[i] = true;

                if (now_us - client.last_ping_us >=
                    HEARTBEAT_INTERVAL_US) {
                    if (!sendFrame(
                            client,
                            HTTPD_WS_TYPE_PING,
                            nullptr,
                            0)) {
                        closeClient(
                            client.fd,
                            client.connection_id,
                            "heartbeat send failed");
                        eligible[i] = false;
                        continue;
                    }
                    updateClientAfterSend(
                        client,
                        client.last_revision,
                        now_us,
                        false,
                        false,
                        true);
                }
            }

            cJSON* snapshot = arctic::ha::createStateSnapshot();
            if (snapshot != nullptr) {
                const cJSON* revision_json =
                    cJSON_GetObjectItemCaseSensitive(
                        snapshot, "revision");
                const uint64_t revision =
                    cJSON_IsNumber(revision_json)
                        ? (uint64_t)revision_json->valuedouble
                        : 0;
                char* hello = createHello(snapshot);
                char* snapshot_message =
                    createSnapshotMessage(snapshot);

                for (size_t i = 0; i < count; ++i) {
                    Client& client = clients[i];
                    if (!eligible[i]) {
                        continue;
                    }

                    bool initial_completed = false;
                    bool snapshot_sent = false;
                    if (client.initial_pending) {
                        if (hello == nullptr ||
                            !sendFrame(
                                client,
                                HTTPD_WS_TYPE_TEXT,
                                hello,
                                strlen(hello))) {
                            closeClient(
                                client.fd,
                                client.connection_id,
                                "hello send failed");
                            continue;
                        }
                        initial_completed = true;
                    }

                    const bool snapshot_due =
                        client.initial_pending ||
                        revision != client.last_revision ||
                        now_us - client.last_snapshot_us >=
                            PERIODIC_SNAPSHOT_US;
                    if (snapshot_due) {
                        if (snapshot_message == nullptr ||
                            !sendFrame(
                                client,
                                HTTPD_WS_TYPE_TEXT,
                                snapshot_message,
                                strlen(snapshot_message))) {
                            closeClient(
                                client.fd,
                                client.connection_id,
                                "snapshot send failed");
                            continue;
                        }
                        snapshot_sent = true;
                    }

                    updateClientAfterSend(
                        client,
                        revision,
                        now_us,
                        initial_completed,
                        snapshot_sent,
                        false);
                }

                if (hello != nullptr) {
                    cJSON_free(hello);
                }
                if (snapshot_message != nullptr) {
                    cJSON_free(snapshot_message);
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }

    closeAllClients();
    s_task = nullptr;
    xSemaphoreGive(s_task_done);
    vTaskDelete(nullptr);
}

size_t activeClientCount()
{
    size_t count = 0;
    if (xSemaphoreTake(s_clients_mutex, pdMS_TO_TICKS(250)) == pdTRUE) {
        for (Client& client : s_clients) {
            if (client.active &&
                s_server != nullptr &&
                httpd_ws_get_fd_info(s_server, client.fd) ==
                    HTTPD_WS_CLIENT_WEBSOCKET) {
                count++;
            } else if (client.active) {
                clearClientLocked(client);
            }
        }
        xSemaphoreGive(s_clients_mutex);
    }
    return count;
}

bool registerClient(int fd, uint32_t auth_generation)
{
    const int64_t now_us = esp_timer_get_time();
    if (xSemaphoreTake(s_clients_mutex, pdMS_TO_TICKS(250)) != pdTRUE) {
        return false;
    }
    for (Client& client : s_clients) {
        if (!client.active) {
            client.active = true;
            client.initial_pending = true;
            client.fd = fd;
            client.connection_id = s_next_connection_id++;
            client.auth_generation = auth_generation;
            client.last_pong_us = now_us;
            client.last_ping_us = now_us;
            client.last_snapshot_us = 0;
            xSemaphoreGive(s_clients_mutex);
            ESP_LOGI(TAG, "Client connected fd=%d", fd);
            return true;
        }
    }
    xSemaphoreGive(s_clients_mutex);
    return false;
}

void notePong(int fd)
{
    if (xSemaphoreTake(s_clients_mutex, pdMS_TO_TICKS(250)) != pdTRUE) {
        return;
    }
    for (Client& client : s_clients) {
        if (client.active && client.fd == fd) {
            client.last_pong_us = esp_timer_get_time();
            break;
        }
    }
    xSemaphoreGive(s_clients_mutex);
}

void removeClientByFd(int fd)
{
    if (xSemaphoreTake(s_clients_mutex, pdMS_TO_TICKS(250)) != pdTRUE) {
        return;
    }
    for (Client& client : s_clients) {
        if (client.active && client.fd == fd) {
            clearClientLocked(client);
            break;
        }
    }
    xSemaphoreGive(s_clients_mutex);
}

}  // namespace

bool ha_websocket_start(httpd_handle_t server)
{
    if (s_running) {
        return true;
    }
    if (server == nullptr) {
        return false;
    }

    if (s_clients_mutex == nullptr) {
        s_clients_mutex = xSemaphoreCreateMutex();
    }
    if (s_task_done == nullptr) {
        s_task_done = xSemaphoreCreateBinary();
    }
    if (s_clients_mutex == nullptr || s_task_done == nullptr) {
        ESP_LOGE(TAG, "Unable to allocate WebSocket synchronization");
        return false;
    }

    while (xSemaphoreTake(s_task_done, 0) == pdTRUE) {
    }
    s_server = server;
    s_running = true;
    if (xTaskCreate(
            pushTask,
            "ha_ws_push",
            8192,
            nullptr,
            5,
            &s_task) != pdPASS) {
        s_running = false;
        s_server = nullptr;
        ESP_LOGE(TAG, "Unable to start WebSocket push task");
        return false;
    }
    ESP_LOGI(TAG, "Home Assistant WebSocket push task started");
    return true;
}

void ha_websocket_stop(void)
{
    if (!s_running) {
        s_server = nullptr;
        return;
    }

    s_running = false;
    xSemaphoreTake(s_task_done, portMAX_DELAY);
    s_server = nullptr;
}

esp_err_t ha_websocket_pre_handshake(httpd_req_t* req)
{
    char token[AUTH_INTEGRATION_TOKEN_LEN + 1] = {};
    uint32_t generation = 0;
    const bool valid =
        extractBearerToken(req, token) &&
        auth_mgr_validate_integration_token_with_generation(
            token, &generation);
    memset(token, 0, sizeof(token));
    if (!valid) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(
            req, "{\"error\":\"Integration token required\"}");
        return ESP_FAIL;
    }
    if (activeClientCount() >= MAX_CLIENTS) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(
            req, "{\"error\":\"WebSocket client limit reached\"}");
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t ha_websocket_handler(httpd_req_t* req)
{
    const int fd = httpd_req_to_sockfd(req);
    if (req->method == HTTP_GET) {
        char token[AUTH_INTEGRATION_TOKEN_LEN + 1] = {};
        uint32_t generation = 0;
        const bool valid =
            extractBearerToken(req, token) &&
            auth_mgr_validate_integration_token_with_generation(
                token, &generation);
        memset(token, 0, sizeof(token));
        if (!valid) {
            return ESP_FAIL;
        }

        const timeval send_timeout = {
            .tv_sec = 0,
            .tv_usec = SEND_TIMEOUT_US,
        };
        if (setsockopt(
                fd,
                SOL_SOCKET,
                SO_SNDTIMEO,
                &send_timeout,
                sizeof(send_timeout)) != 0) {
            ESP_LOGE(TAG, "Unable to set send timeout fd=%d", fd);
            return ESP_FAIL;
        }
        return registerClient(fd, generation) ? ESP_OK : ESP_FAIL;
    }

    httpd_ws_frame_t frame = {};
    esp_err_t result = httpd_ws_recv_frame(req, &frame, 0);
    if (result != ESP_OK) {
        removeClientByFd(fd);
        return result;
    }

    if (frame.type == HTTPD_WS_TYPE_PONG) {
        if (frame.len > 125) {
            removeClientByFd(fd);
            httpd_sess_trigger_close(req->handle, fd);
            return ESP_ERR_INVALID_SIZE;
        }
        uint8_t payload[125];
        if (frame.len > 0) {
            frame.payload = payload;
            result = httpd_ws_recv_frame(req, &frame, frame.len);
        }
        if (result == ESP_OK) {
            notePong(fd);
        }
        return result;
    }

    if (frame.type == HTTPD_WS_TYPE_PING) {
        if (frame.len > 125) {
            removeClientByFd(fd);
            httpd_sess_trigger_close(req->handle, fd);
            return ESP_ERR_INVALID_SIZE;
        }
        uint8_t payload[125];
        if (frame.len > 0) {
            frame.payload = payload;
            result = httpd_ws_recv_frame(req, &frame, frame.len);
            if (result != ESP_OK) {
                return result;
            }
        }
        httpd_ws_frame_t pong = {};
        pong.type = HTTPD_WS_TYPE_PONG;
        pong.payload = payload;
        pong.len = frame.len;
        result = httpd_ws_send_frame(req, &pong);
        return result;
    }

    if (frame.type == HTTPD_WS_TYPE_CLOSE) {
        if (frame.len > 125) {
            removeClientByFd(fd);
            httpd_sess_trigger_close(req->handle, fd);
            return ESP_ERR_INVALID_SIZE;
        }
        uint8_t payload[125];
        if (frame.len > 0) {
            frame.payload = payload;
            result = httpd_ws_recv_frame(req, &frame, frame.len);
        }
        if (result == ESP_OK) {
            result = httpd_ws_send_frame(req, &frame);
        }
        removeClientByFd(fd);
        httpd_sess_trigger_close(req->handle, fd);
        return result;
    }

    removeClientByFd(fd);
    httpd_sess_trigger_close(req->handle, fd);
    return ESP_ERR_INVALID_ARG;
}
