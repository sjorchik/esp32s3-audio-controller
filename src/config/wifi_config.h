#pragma once

// Константи модулів net/wifi_manager та net/mdns (Prompt 12).
// [Prompt 29] ДОДАНО (лише нові константи в кінці namespace): кілька збережених мереж, вибір
// мережі серед них, бюджет часу до AP, параметри скану в режимі STA, NVS-сховище списку.
// Кількість невдалих спроб до AP і назва точки доступу/mDNS НЕ дублюються тут:
// беруться з config/defaults.h (defaults::kWifiRetryBeforeAp, kApSsid, kMdnsName).
// Часові значення — у мілісекундах.

#include <stddef.h>
#include <stdint.h>

#include "config/defaults.h"
#include "config/display_config.h"  // display_cfg::kTaskPriority (для static_assert)

// Serial-лог менеджера: "[WIFI] STA: connected ..." Вимкнути = 0.
#define WIFI_MANAGER_DEBUG 1

namespace wifi_cfg {

// ---------------------------------------------------------------------------
// Задача FreeRTOS (MASTER SPEC, розділ 5: мережа — ядро 0, найнижчий пріоритет)
// ---------------------------------------------------------------------------
constexpr int      kTaskCore       = 0;
constexpr uint8_t  kTaskPriority   = 1;
constexpr uint32_t kTaskStackBytes = 8192;

// Період опитування WiFi.status() у режимі STA (перша спроба, моніторинг, повтор).
constexpr uint32_t kStaPollMs = 250;
// Період циклу в режимі AP: DNSServer::processNextRequest() має викликатись часто.
constexpr uint32_t kApPollMs = 10;

// ---------------------------------------------------------------------------
// STA
// ---------------------------------------------------------------------------
// Скільки чекати на одну спробу підключення (кількість спроб — defaults::kWifiRetryBeforeAp).
constexpr uint32_t kConnectTimeoutMs = 15000;
// Пауза між повторними підключеннями після втрати звʼязку (під час роботи).
constexpr uint32_t kReconnectPeriodMs = 5000;
// Як часто оновлювати кешований RSSI.
constexpr uint32_t kRssiPeriodMs = 2000;
// Тайм-аут внутрішнього мʼютекса (його беруть і HTTP-обробники порталу).
constexpr uint32_t kLockTimeoutMs = 50;

// ---------------------------------------------------------------------------
// AP + captive portal
// ---------------------------------------------------------------------------
constexpr uint8_t  kApIp[4]      = {192, 168, 4, 1};
constexpr uint8_t  kApNetmask[4] = {255, 255, 255, 0};
constexpr uint8_t  kApChannel    = 1;
constexpr uint8_t  kApMaxClients = 4;
// Порожній рядок = відкрита мережа (зручно для captive portal). Інакше >= 8 символів.
constexpr const char* kApPassword = "";
constexpr uint16_t kDnsPort        = 53;
constexpr uint16_t kPortalHttpPort = 80;

// Скан мереж для сторінки порталу.
constexpr uint8_t  kMaxNetworks   = 16;
constexpr uint32_t kScanTimeoutMs = 15000;

// Підключення з порталу: пауза, щоб HTTP-відповідь дійшла до телефону, потім
// очікування результату і (при успіху) перезапуск.
constexpr uint32_t kPortalReplyFlushMs     = 800;
constexpr uint32_t kPortalConnectTimeoutMs = 20000;
constexpr uint32_t kRestartDelayMs         = 1000;

// Межі введення (WPA2: пароль 8..63 символи, SSID до 32 байтів).
constexpr size_t kSsidMax = 32;
constexpr size_t kPassMax = 63;

// ---------------------------------------------------------------------------
// mDNS: порт _http._tcp (узгоджено з майбутнім net/web_server, Prompt 13)
// ---------------------------------------------------------------------------
constexpr uint16_t kMdnsHttpPort = 80;

// ---------------------------------------------------------------------------
// [Prompt 29] Кілька збережених мереж
// ---------------------------------------------------------------------------
// Скільки мереж (роутерів) можна зберегти. Порядок у списку = пріоритет (0 — найвищий).
constexpr uint8_t kMaxSavedNetworks = 5;

// NVS-сховище списку: окремий namespace (НЕ audioctl), один блоб із версією формату.
constexpr const char* kNetNvsNamespace = "wifinets";
constexpr const char* kNetNvsKey       = "list";
constexpr uint8_t     kNetFormatVersion = 1;

// Скільки чекати на мʼютекс запису в NVS (запис у задачі wifi; clear() чекає довше).
constexpr uint32_t kNvsLockTimeoutMs      = 500;
constexpr uint32_t kNvsClearLockTimeoutMs = 3000;
// Пауза між повторами запису списку в NVS, якщо запис не вдався (список лишається «брудним»).
constexpr uint32_t kPersistRetryMs = 2000;

// Вибір мережі (FirstConnect / Reconnect). Раунд = скан + спроби кандидатів за пріоритетом.
// При ОДНІЙ збереженій мережі скану немає: пряма спроба, як до P29.
constexpr uint8_t kMaxCandidatesPerRound = 5;
// Бюджет часу від початку першого підключення після старту. Коли він вичерпаний, поточна спроба
// дограється, нові кандидати й нові раунди не починаються -> AP. Оцінка найгіршого випадку до AP:
// одна мережа — 3 x kConnectTimeoutMs = 45 с (як до P29); кілька — приблизно
// kApBudgetMs + kConnectTimeoutMs = 75 с (при швидких відмовах — набагато менше).
constexpr uint32_t kApBudgetMs = 60000;
// Тайм-аут однієї спроби під час роботи (Reconnect). Раніше стек перезапускав спробу кожні
// kReconnectPeriodMs; тепер одна спроба триває стільки, потім пауза kReconnectPeriodMs.
constexpr uint32_t kReconnectAttemptTimeoutMs = 8000;

// Скан у режимі STA (вибір мережі й GET/POST /api/wifi/scan): коротший час на канал, щоб
// менше переривати потік. Скан порталу AP лишається з типовими 300 мс.
constexpr uint32_t kScanChannelStaMs = 120;
constexpr uint32_t kScanChannelApMs  = 300;

// Примусове підключення з API: пауза, щоб HTTP-відповідь дійшла до клієнта до перемикання.
constexpr uint32_t kConnectReplyFlushMs = 800;

// ---------------------------------------------------------------------------
// Перевірки на етапі компіляції
// ---------------------------------------------------------------------------
constexpr size_t cstrLen(const char* s) { return *s ? 1 + cstrLen(s + 1) : 0; }

static_assert(kTaskCore == 0, "Wi-Fi runs on core 0 (MASTER SPEC, section 5)");
static_assert(kTaskPriority >= 1 && kTaskPriority <= display_cfg::kTaskPriority,
              "network priority must be the lowest (<= UI)");
static_assert(defaults::kWifiRetryBeforeAp >= 1, "need at least one STA attempt");
static_assert(kStaPollMs >= 1 && kApPollMs >= 1, "poll periods must be >= 1 ms");
static_assert(kConnectTimeoutMs >= 1000, "connect timeout too short");
static_assert(kMaxNetworks >= 1, "kMaxNetworks must be >= 1");
static_assert(cstrLen(kApPassword) == 0 || cstrLen(kApPassword) >= 8,
              "AP password must be empty (open) or >= 8 chars");
static_assert(cstrLen(defaults::kApSsid) >= 1 && cstrLen(defaults::kApSsid) <= kSsidMax,
              "defaults::kApSsid must be 1..32 chars");

static_assert(kMaxSavedNetworks >= 1 && kMaxSavedNetworks <= 16, "kMaxSavedNetworks out of range");
static_assert(kMaxCandidatesPerRound >= 1 && kMaxCandidatesPerRound <= kMaxSavedNetworks,
              "candidates per round must be 1..kMaxSavedNetworks");
static_assert(kApBudgetMs >= kConnectTimeoutMs, "AP budget must cover at least one attempt");
static_assert(kReconnectAttemptTimeoutMs >= 1000, "reconnect attempt timeout too short");
static_assert(kScanChannelStaMs >= 20 && kScanChannelApMs >= 20, "scan dwell too short");
static_assert(kConnectReplyFlushMs >= 200, "reply flush pause too short");
static_assert(kPersistRetryMs >= 100, "persist retry period too short");

}  // namespace wifi_cfg
