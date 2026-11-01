#pragma once

// [USB2] Постійний модуль монтування USB-флешки (USB-host MSC на рідному порту S3).
// [USB3] ДОДАНО end(): повне зняття драйвера USB-host (ліниве піднімання, варіант 2) і стан Off.
// Лише монтування / розмонтування й діагностика: НІЧОГО не пише на носій і не форматує його.
// Файли після монтування доступні через звичайний POSIX / VFS за шляхом mountPath() ("/usb").
//
// Потоки:
//  - begin() / end() серіалізуються одним мьютексом; можна викликати з будь-якої задачі,
//    КРІМ задачі usb_mgr (тобто НЕ з EventCallback): end() з колбека повертає false;
//  - усі події драйвера обробляє власна задача usb_mgr (ядро 0, див. config/usb_host_config.h);
//    end() теж виконується в ній (caller чекає не довше usb_host_cfg::kEndWaitMs);
//  - колбек події викликається з задачі usb_mgr (НЕ з задач USB-host); він не має блокувати:
//    довге монтування / розмонтування іде в цій самій задачі.
//
// Заголовок не підключає заголовки IDF / драйвера MSC, щоб наступні кроки (fs::FS-обгортка,
// connecttoFS) могли його включати без впливу на збірку.

#include <stdint.h>

#include "config/features.h"

#if FEATURE_USB_HOST

namespace usb_msc_mount {

enum class State : uint8_t {
    NoDevice,  // USB-host працює, носія немає
    Mounting,  // носій вставлено, іде монтування
    Mounted,   // том змонтовано у mountPath()
    Error,     // носій є, але змонтувати не вдалося (lastError()), або end() не зміг зняти стек
    Off,       // [USB3] USB-host не піднято (до begin() або після end())
};

enum class Event : uint8_t {
    Inserted,  // носій вставлено (далі Mounted або Error)
    Mounted,   // том змонтовано
    Removed,   // носій вийнято (том розмонтовано); також при end() зі змонтованим томом
    Error,     // монтування не вдалося (всі спроби вичерпано)
};

// Остання помилка (історія, не очищується успіхом). code — esp_err_t (0 = помилок не було),
// where — статичний рядок із назвою кроку, напр. "vfs_register".
struct LastError {
    int32_t code;
    const char* where;
};

// Колбек події. Викликається з задачі usb_mgr; не блокувати; не викликати з нього begin() / end().
using EventCallback = void (*)(Event ev, void* arg);

// Запуск USB-host і драйвера MSC. Працює повторно після end().
// Повертає true, якщо стек піднято протягом usb_host_cfg::kInstallWaitMs; якщо вже піднято — true
// (нешкідливо); якщо йде зняття (end) — false. [USB3: раніше «один раз»]
bool begin();

// [USB3] Повне зняття: розмонтування, msc_host_uninstall, звільнення пристроїв, зупинка usb_lib,
// usb_host_uninstall, завершення usb_mgr. Чекає не довше usb_host_cfg::kEndWaitMs.
// true — стан Off (також коли вже було Off); false — таймаут або помилка (див. lastError() / usb.stat;
// при таймауті зняття триває у фоні, при помилці стан Error і можна повторити end()).
bool end();

// [USB3] true, якщо стек піднято (Up) або піднімається.
bool isStarted();

bool isMounted();
State state();
LastError lastError();
const char* mountPath();

void setEventCallback(EventCallback cb, void* arg);

const char* stateName(State s);
// Імʼя коду помилки (включно з кодами драйвера MSC 0x1701…0x1704).
const char* errorName(int32_t code);

#if USB_HOST_DIAG
// Опитування консолі Serial (usb.*). Викликати з loop(); команди обробляє той, хто викликав
// (loopTask). Не споживає рядків, що не починаються з 'u' (щоб не красти їх у інших консолей).
// УВАГА: usb.life N блокує викликача на час N циклів.
void pollConsole();
#endif

}  // namespace usb_msc_mount

#endif  // FEATURE_USB_HOST
