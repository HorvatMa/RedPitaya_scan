#ifndef SERVER_H
#define SERVER_H

#include <signal.h>

#include "../app.h"

/*
 * Serves one client at a time on the given port until *quit becomes non-zero.
 * A client disconnect stops the scan. Returns 0, or -1 if the port cannot be opened.
 */
int server_run(app_t *app, int port, volatile sig_atomic_t *quit);

#endif
