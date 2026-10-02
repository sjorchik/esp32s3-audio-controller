#pragma once

// Serial-тест StationStore (лише STATION_STORE_TEST == 1).
// Команди (рядок, завершений \n або \r):
//   st.help
//   st.list                  — друк усіх станцій
//   st.export                — друк JSON списку
//   st.reset                 — видалити /stations.json (seed при наступному begin(),
//                              тобто після перезавантаження; список у RAM не змінюється)
//   st.import <json>         — імпорт JSON з одного рядка (масив або обʼєкт з version)

class StationStoreTest {
public:
    // Створює задачу читання Serial. Викликати ПІСЛЯ AppController::begin()
    // (тобто після StationStore::begin()). Повторний виклик безпечний.
    static bool begin();
};
