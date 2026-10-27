/*
 * rpsipm_server: scans with the galvos, reconstructs images on the Red Pitaya and
 * sends them to one TCP client (protocol: documentation/PROTOCOL.md).
 *
 *   rpsipm_server [-p PORT]
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "app.h"
#include "hw/hw.h"
#include "net/server.h"

static volatile sig_atomic_t quit;

static void on_signal(int sig)
{
    (void)sig;
    quit = 1;
}

int main(int argc, char **argv)
{
    int port = 5000;
    int opt;
    while ((opt = getopt(argc, argv, "p:h")) != -1) {
        if (opt == 'p') {
            port = atoi(optarg);
        } else {
            fprintf(stderr, "usage: %s [-p PORT]\n", argv[0]);
            return opt == 'h' ? 0 : 2;
        }
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    hw_t *hw = hw_open();
    if (!hw) {
        fprintf(stderr, "cannot open hardware\n");
        return 1;
    }
    printf("hardware backend: %s\n", hw_name(hw));
    app_t *app = app_create(hw);
    if (!app) {
        fprintf(stderr, "out of memory\n");
        hw_close(hw);
        return 1;
    }

    int rc = server_run(app, port, &quit);

    app_destroy(app);                 /* stops a running scan first */
    hw_close(hw);
    printf("bye\n");
    return rc ? 1 : 0;
}
