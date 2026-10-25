#pragma once

// [W1] Діагностика веб-запитів (WEB_TRACE у config/web_server_config.h).
// begin(server): глобальний middleware AsyncWebServer формує рядок
//   "[WEB] t=<мс> <метод> <шлях> <код> <мс обробки>"
// і кладе його в чергу без очікування; друкує низькопріоритетна задача (ядро 0).
// Один раз за ~kTraceProbeDelayMs друкує: режим сну Wi-Fi, ядро/пріоритет async_tcp.
// "мс обробки" = час виконання обробника (не час передачі відповіді по Wi-Fi).
// При WEB_TRACE = 0 begin() порожня й увесь код компілюється геть.

#include "config/web_server_config.h"

class AsyncWebServer;

namespace web_trace {

#if WEB_TRACE
// Викликати один раз (з registerStaticRoutes), до server.begin(). Повторний виклик ігнорується.
void begin(AsyncWebServer& server);
#else
inline void begin(AsyncWebServer&) {}
#endif

}  // namespace web_trace
