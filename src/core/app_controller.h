#pragma once

// Контролер логіки режимів і розкладки керування.
// Читає EventBus у власній задачі (ядро 0, пріоритет 3) і керує AudioProcessor,
// AudioPlayer, Settings та AppState за розкладкою з розділу 5 MASTER SPEC.
// Сам нічого не малює й не чіпає DisplayManager: екрани читають AppState.
//
// [Prompt 8] Заглушку замінено.
//   ЗМІНЕНО — begin() тепер приймає вказівник на аудіопроцесор (AppController
//             ним не володіє; main.cpp створив його раніше). nullptr дозволений:
//             тоді керування звуком недоступне, решта логіки працює.
// [Prompt 13] ДОДАНО: setTone() — вхід для веб-сервера. AppController тримає власні
//             копії bass/treble/balance і публікує їх у AppState/Settings після КОЖНОЇ
//             події, тож пряма зміна чипа повз нього була б затерта наступною подією.
// [Prompt 15] ДОДАНО: beginIrLearn()/cancelIrLearn()/confirmIrOverwrite() — вхід для
//             веб-сервера до навчання IR. IR-приймач — апаратний ресурс, а навчання
//             змінює глобальну поведінку (Mode::IrLearn), тож веб не викликає
//             IrRc5::beginLearn() та інші напряму.

#include "core/events.h"

class AudioProcessor;

// [Prompt 13] ДОДАНО: запит на зміну тембру/балансу ззовні (веб).
// Значення мають бути вже перевірені за capabilities. Поля *Ok — результат.
struct ToneUpdate {
    bool hasBass = false;
    int8_t bass = 0;
    bool hasTreble = false;
    int8_t treble = 0;
    bool hasBalance = false;
    int8_t balance = 0;
    bool bassOk = false;
    bool trebleOk = false;
    bool balanceOk = false;
};

class AppController {
public:
    // Ініціалізація: AppState::begin() (першим ділом), стан із Settings,
    // створення задачі. Викликати ПІСЛЯ EventBus::begin(), SettingsStore::begin()
    // /load(), створення аудіопроцесора та AudioPlayer::begin().
    // Повторний виклик безпечний (повертає true).
    static bool begin(AudioProcessor* processorOrNull);

    // Обробка однієї події. Основний шлях — внутрішня задача, яка сама читає
    // чергу; публічний виклик лишено для тестування. Захищено внутрішнім
    // мʼютексом, тож безпечний і з інших задач. Не робити з обробника подій.
    static void handleEvent(const Event& event);

    // [Prompt 13] ДОДАНО: під внутрішнім мʼютексом викликає setBass/setTreble/setBalance
    // процесора, оновлює власні копії, публікує AppState і відкладено зберігає Settings.
    // false — контролер не запущено або мʼютекс зайнятий довше kLockTimeoutMs (нічого не
    // змінено). true — запит оброблено; успіх кожного поля — у *Ok (false = I2C не
    // відповів або чип не підтримує функцію; копія в такому разі не змінюється).
    static bool setTone(ToneUpdate& update);

    // [Prompt 15] ДОДАНО: починає навчання дії target. Запамʼятовує поточний Mode,
    // ставить Mode::IrLearn і викликає IrRc5::beginLearn(). Звук/потік не чіпає.
    // false — контролер не запущено, мʼютекс зайнятий довше kLockTimeoutMs, навчання
    // вже триває (Waiting/Confirm/Conflict) або IrRc5::beginLearn() відмовив (дія
    // некоректна чи мапа заповнена). Якщо на екрані ще результат попереднього навчання
    // (Success/Timeout) — його знімає одразу й починає нове.
    static bool beginIrLearn(Action target);

    // [Prompt 15] ДОДАНО: скасовує навчання (IrRc5::cancelLearn()) і одразу повертає
    // збережений Mode; статус у AppState стає Idle. Поза навчанням — лише скидає
    // «липкий» результат (Success/Timeout) у Idle. Безпечно викликати завжди.
    static void cancelIrLearn();

    // [Prompt 15] ДОДАНО: підтверджує перепризначення коду в стані Conflict
    // (IrRc5::confirmOverwrite()). false — не в Conflict, контролер зайнятий або
    // IrRc5 відмовив.
    static bool confirmIrOverwrite();
};
