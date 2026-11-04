#pragma once

// [USB10] ВНУТРІШНІЙ заголовок модулів usb_browser / usb_queue (задача usb_nav). НЕ підключати ззовні src/usb/.
// Спільні типи списку, команди фонової задачі, мьютекс, хуки черги.

#include <stddef.h>
#include <stdint.h>

#include "config/features.h"

#if FEATURE_USB_MODE

#include "config/usb_browser_config.h"

namespace usb_nav {

constexpr uint32_t kGenUnknown = 0xFFFFFFFFu;  // покоління не вдалося прочитати (том зайнятий довше kGenLockWaitMs)

// Запис списку: імʼя — у пулі (nameOff, з '\0'). 12 Б.
struct Entry {
    uint32_t nameOff;
    uint32_t size;
    uint16_t nameLen;
    uint8_t isDir;
    uint8_t reserved;
};

// Список однієї папки. Буфери e / pool — у PSRAM, виділяються один раз на сеанс режиму (allocListing).
struct Listing {
    Entry* e;
    char* pool;
    uint16_t cap;
    uint32_t poolCap;
    uint16_t count;
    uint16_t dirs;  // записів-папок (вони першими)
    uint32_t poolUsed;
    bool truncated;
    bool valid;
    uint16_t skippedHidden;
    uint16_t skippedOther;
    uint16_t skippedLong;
    uint8_t depth;
    uint32_t gen;      // покоління монтування, у якому прочитано (usb_msc_mount::volumeGenerationLocked)
    uint32_t version;  // змінюється при кожній публікації / інвалідації списку
    uint32_t readMs;
    char cwd[usb_browser_cfg::kPathMax];  // "/" — корінь; без '/' наприкінці
};

enum class CmdType : uint8_t { Refresh, EnterDir, Up, QPlay, QNext, QPrev, QStop };

struct Cmd {
    CmdType type;
    uint16_t index;
    uint32_t expectVersion;  // 0 — не перевіряти
    uint32_t reqId;
};

enum class ReadResult : uint8_t { Ok, NotMounted, VolumeBusy, OpenFailed, NotDir, MediaChanged, ModeGone };
const char* readResultName(ReadResult r);

bool allocListing(Listing& l);   // PSRAM; false — немає PSRAM / памʼяті
void freeListing(Listing& l);
void resetListing(Listing& l);   // скидає вміст, зберігає буфери
// Читає каталог path (від кореня флешки) у out: фільтр, ліміти, сортування. Працює лише в задачі usb_nav; без мьютекса списку.
ReadResult readDir(const char* path, uint8_t depth, Listing& out);
const char* nameAt(const Listing& l, uint16_t i);
// Копіює лише файли (у порядку списку) з src у dst; false — не вмістилось (не повинно, ємності однакові).
bool copyFiles(const Listing& src, Listing& dst);

uint32_t generation();  // 0 — том не змонтовано; kGenUnknown — не вдалося прочитати
bool lock(uint32_t waitMs);  // UINT32_MAX = чекати безкінечно
void unlock();
bool modeOk();  // usb_mode::state() ∈ {Ready, Playing}

// Постановка команди в чергу задачі usb_nav (запускає задачу, якщо її ще нема). Неблокуюча. why — статичний рядок.
bool post(CmdType t, uint16_t index, uint32_t expectVersion, uint32_t* reqIdOut, const char** why);
// Завершення команди (лише з задачі usb_nav).
void finish(uint32_t reqId, bool ok, const char* why);
// Поточний «живий» список (ЧИТАТИ без мьютекса дозволено лише з задачі usb_nav).
const Listing& live();

// Хуки черги (виконуються в задачі usb_nav).
namespace q {
bool onWorkerStart();   // виділяє буфери черги; false — немає памʼяті
void onWorkerStop();    // режим вийшов із Ready / Playing
void tick();            // кожні kPollMs
void handle(const Cmd& c);  // QPlay / QNext / QPrev / QStop (сама викликає finish)
void onMediaGone();     // покоління змінилось / том знято
}  // namespace q

}  // namespace usb_nav

#endif  // FEATURE_USB_MODE
