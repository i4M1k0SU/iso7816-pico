// USB-serial console: commands for diagnostics, plus the log implementation declared in log.h.
#pragma once

void console_init(void);

// Handle console input (commands) and host connect/disconnect events.
void console_poll(void);
