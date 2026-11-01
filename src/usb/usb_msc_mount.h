#pragma once

// [USB2] Постійний модуль монтування USB-флешки (USB-host MSC на рідному порту S3).
// Лише монтування / розмонтування й діагностика: НІЧОГО не пише на носій і не форматує його.
// Файли після монтування доступні через звичайний POSIX / VFS за шляхом mountPath() ("/usb").
//
// Потоки:
//  - begin() викликається один раз (з setup() або консольною командою usb.begin);
//  - усі події драйвера обробляє власна задача usb_mgr (ядро 0, див. config/usb_host_config.h);
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
    NoDevice,  // носія немає (або USB-host не запущено)
    Mounting,  // носій вставлено, іде монтування
    Mounted,   // том змонтовано у mountPath()
    Error,     // носій є, але змонтувати не вдалося (lastError())
};

enum class Event : uint8_t {
    Inserted,  // носій вставлено (далі Mounted або Error)
    Mounted,   // том змонтовано
    Removed,   // носій вийнято (том розмонтовано)
    Error,     // монтування не вдалося (всі спроби вичерпано)
};

// Остання помилка (історія, не очищується успіхом). code — esp_err_t (0 = помилок не було),
// where — статичний рядок із назвою кроку, напр. "vfs_register".
struct LastError {
    int32_t code;
    const char* where;
};

// Колбек події. Викликається з задачі usb_mgr; не блокувати.
using EventCallback = void (*)(Event ev, void* arg);

// Запуск USB-host і драйвера MSC (один раз; повторний виклик повертає результат першого).
// Повертає true, якщо інсталяція завершилась успішно протягом usb_host_cfg::kInstallWaitMs.
bool begin();

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
void pollConsole();
#endif

}  // namespace usb_msc_mount

#endif  // FEATURE_USB_HOST
