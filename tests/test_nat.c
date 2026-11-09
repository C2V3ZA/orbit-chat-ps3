/* Automatic router opening against tests/fake_router.py (run by tools/test_nat.sh). */
#include "orbit.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(int argc, char **argv) {
    assert(argc == 2);
    setvbuf(stdout, NULL, _IONBF, 0);
    OrbitNat n;
    int rc = orbit_nat_open("192.168.77.5", "127.0.0.1", 31413, &n);
    printf("mode=%s rc=%d ok=%d method=%s external=%s port=%d cgnat=%d reason=%s\n", argv[1], rc,
           n.ok, n.method, n.external, n.port, n.cgnat, n.reason);
    if (!strcmp(argv[1], "upnp")) {
        /* 31413 is taken by "another console": the next port is mapped instead. */
        assert(!rc && n.ok && !strcmp(n.method, "UPnP") && !strcmp(n.external, "93.184.216.34") &&
               n.port == 31414);
        orbit_nat_close(&n, "127.0.0.1");
        puts("PASS: UPnP IGD discovery, external address, port conflict -> next port, removal");
    } else if (!strcmp(argv[1], "pcp")) {
        assert(!rc && n.ok && !strcmp(n.method, "PCP") && !strcmp(n.external, "93.184.216.35") &&
               n.port == 40001);
        puts("PASS: PCP mapping on the default gateway when UPnP is absent");
    } else if (!strcmp(argv[1], "fritz")) {
        assert(rc && !n.ok && n.fritz && strstr(n.reason, "FRITZ!Box") &&
               strstr(n.reason, "Selbstständige Portfreigaben"));
        puts("PASS: FRITZ!Box refusal detected, exact one-time setting shown to the user");
    } else {
        assert(rc && !n.ok && n.cgnat);
        puts("PASS: CGNAT/DS-Lite detected and reported instead of a broken invitation");
    }
    return 0;
}
