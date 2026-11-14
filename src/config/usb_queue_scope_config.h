#pragma once

// [USB20a-1] Опція області черги плеєра USB: «folder» (файли поточної папки, як і було) або «all» (усі файли флешки; саму
// чергу «all» реалізує крок USB20a-2). Значення зберігається ОКРЕМИМ ключем NVS `pl_scope` (namespace «audioctl», як
// `pl_usb_*`): blob Settings (v9, 174 Б) НЕ змінюється, тож старі налаштування не скидаються; відсутній ключ = «folder».
// Прапорців умовної компіляції тут немає: константи безпечно підключати завжди.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace usb_queue_scope_cfg {

enum class Scope : uint8_t {
    Folder = 0,  // черга = файли ПОТОЧНОЇ папки (без рекурсії)
    All = 1,     // черга = усі .mp3 флешки рекурсивно (USB20a-2)
};

constexpr Scope kDefaultScope = Scope::Folder;

// Ключ NVS (≤ 15 символів). Значення: 1 = all; ключа нема = folder (при збереженні «folder» ключ видаляється).
constexpr const char* kNvsKeyScope = "pl_scope";

// Імена в JSON веб-API (стабільні рядки).
constexpr const char* kNameFolder = "folder";
constexpr const char* kNameAll = "all";

// Біти WebCommand::value для WebCmdType::UsbOptionsSet (біти 0..3 — shuffle / repeat, див. app_controller.h).
constexpr int32_t kOptBitChange = 0x10;  // змінювати scope
constexpr int32_t kOptBitValue = 0x20;   // нове значення: 1 = all, 0 = folder

// Причина відмови «черга ще добудовується» (режим «all», USB20a-2): next / prev / shuffle -> HTTP 409. У USB20a-1 лише
// зарезервовано в контракті (черга «folder» не добудовується ніколи).
constexpr const char* kWhyQueueBuilding = "queue_building";

inline const char* name(Scope s) { return s == Scope::All ? kNameAll : kNameFolder; }

// Розбір рядка з JSON. false — невідоме значення (out не змінюється).
inline bool parse(const char* s, Scope* out) {
    if (s == nullptr || out == nullptr) {
        return false;
    }
    if (strcmp(s, kNameFolder) == 0) {
        *out = Scope::Folder;
        return true;
    }
    if (strcmp(s, kNameAll) == 0) {
        *out = Scope::All;
        return true;
    }
    return false;
}

static_assert((kOptBitChange & 0xF) == 0 && (kOptBitValue & 0xF) == 0 && kOptBitChange != kOptBitValue,
              "scope bits must not overlap the shuffle / repeat bits (0x1..0x8)");
static_assert(sizeof("pl_scope") - 1 <= 15, "NVS key name must be at most 15 characters");

}  // namespace usb_queue_scope_cfg
