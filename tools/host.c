/* Native build: protocol test host. Not required to operate the PS3 app. */
#include "orbit.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
static volatile sig_atomic_t stopped;
static void stop(int n) {
    (void)n;
    stopped = 1;
}
int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "Usage: orbit-test-host DATAFILE KEYFILE\n");
        return 2;
    }
    static OrbitStore store;
    uint8_t key[32];
    if (orbit_store_open(&store, argv[1]) || orbit_keyfile(argv[2], key))
        return 3;
    OrbitServer *server = orbit_server_start(&store, key, ORBIT_PORT);
    if (!server)
        return 4;
    signal(SIGINT, stop);
    signal(SIGTERM, stop);
    puts("Orbit test host ready on port 31413");
    fflush(stdout);
    while (!stopped)
        usleep(100000);
    orbit_server_stop(server);
    orbit_store_close(&store);
    return 0;
}
