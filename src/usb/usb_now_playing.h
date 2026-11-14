#pragma once

// [USB17a] Збір даних «зараз грає» для екрана режиму «USB» (малює USB17b). Лише ЧИТАННЯ API usb_mode / usb_queue /
// AudioPlayer; нічого не змінює й не дублює розбір тегів: теги бере AudioPlayer::fileTags() (USB18a), ті самі, що йдуть у
// веб-API nowPlaying. Текст нормалізується під шрифти дисплея (usb/usb_np_text.h) при копіюванні у фіксовані буфери.
//
// Потоки. collect() викликає ЛИШЕ AppController::syncPlayer() (задача контролера, ядро 0, під його мʼютексом): функція
// тримає статичні буфери (шлях 256 Б, теги 192 Б) замість стеку й не реентерабельна. Блокує не довше за
// usb_browser_cfg::kApiLockWaitMs (мʼютекс usb_queue::status()); решта викликів — короткі критичні розділи / атомарні кеші.
// Заголовок не підключає заголовки IDF, usb_* і AudioPlayer.

#include "core/app_state.h"

namespace usb_now_playing {

// Заповнює *out (спершу обнуляє). Поза режимом «USB» (usb_mode не Ready / Playing) або без FEATURE_USB_MODE — нулі, true.
// Режим активний: shuffle / repeatAll / count — завжди; решта — лише коли черга не Idle (active = true). false — мʼютекс
// черги зайнятий (*out нульовий і НЕ має публікуватись: лишається попереднє значення в AppState).
bool collect(UsbNowPlaying* out);

}  // namespace usb_now_playing
