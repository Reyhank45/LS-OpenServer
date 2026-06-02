#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(void) {
  // Bring up the interface (em0)
  if (system("/System/bin/netadm up em0") != 0) {
    fprintf(stderr, "netinit: failed to bring up em0\n");
    return 1;
  }
  // Configure a static IP suitable for QEMU user networking
  // Typical QEMU guest IP is 10.0.2.15 with gateway 10.0.2.2
  if (system("/System/bin/netadm addr em0 10.0.2.15 255.255.255.0") != 0) {
    fprintf(stderr, "netinit: failed to set address on em0\n");
    return 1;
  }
  // Add default route (via gateway 10.0.2.2)
  if (system("/System/bin/netadm route add default 10.0.2.2") != 0) {
    // If route command not implemented, ignore silently
  }
  return 0;
}
