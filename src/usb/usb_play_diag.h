#pragma once

// [USB7] Діагностична консоль відтворення файлу з /usb (лише USB_HOST_DIAG = 1):
//   usb.play <шлях> [стартова_секунда] | usb.pstat | usb.seek <секунди> | usb.stopf | usb.ended [take]
//   usb.fx [extra <мс> | dig <мс|-1> | level <дБ>]  — підбір паузи XSMT / цифрового розмʼюту / рівня файлу на льоту
// Викликається з usb_msc_mount::handleLine (loopTask, ядро 1), як usb.* і usb.fs.*. Не створює задач, нічого не пише
// на носій. Керує лише AudioPlayer (playFile / stop / seekFileSec); AppController, UI, веб не чіпає.

#include "config/features.h"

#if FEATURE_USB_HOST && USB_HOST_DIAG

namespace usb_play_diag {

// true, якщо рядок — одна з команд цього модуля (слово повністю, далі пробіл або кінець).
bool isPlayCommand(const char* line);

// Виконує команду. Друкує результат у Serial (тег «[USB]»).
void handleConsoleLine(const char* line);

}  // namespace usb_play_diag

#endif  // FEATURE_USB_HOST && USB_HOST_DIAG
