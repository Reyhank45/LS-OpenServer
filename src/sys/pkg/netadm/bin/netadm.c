/*
 * netadm — network management utility for LS-OpenServer
 *
 * Uses FreeBSD native Address Aliasing (SIOCAIFADDR) to assign IP
 * configurations. Handles BSD link-layer (AF_LINK) and network-layer (AF_INET)
 * separation cleanly.
 *
 * Usage:
 * netadm up   <iface>
 * netadm down <iface>
 * netadm addr <iface> <ip> [netmask]
 * netadm show
 */

#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h> /* Pulls in native struct ifreq and struct ifaliasreq */
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/sockio.h> /* Pulls in native BSD SIOCAIFADDR / SIOCGIFFLAGS definitions */
#include <sys/types.h>
#include <unistd.h>

/* ── CRITICAL OVERRIDES FOR LINUX-HOST BUILD POLLUTION ────────── */
#undef SIOCGIFCONF
#define SIOCGIFCONF 0xc0106924U

/* Missing standard BSD macro for link-layer casting natively in some
 * cross-toolchains */
#ifndef AF_LINK
#define AF_LINK 18
#endif

/* ── Helpers ─────────────────────────────────────────────────── */

static int open_sock(void) {
  int s = socket(AF_INET, SOCK_DGRAM, 0);
  if (s < 0) {
    perror("socket");
    exit(1);
  }
  return s;
}

static void set_sin(struct sockaddr_in *sin, const char *ip) {
  memset(sin, 0, sizeof(*sin));
  sin->sin_len = sizeof(struct sockaddr_in);
  sin->sin_family = AF_INET;
  if (inet_pton(AF_INET, ip, &sin->sin_addr) != 1) {
    fprintf(stderr, "netadm: invalid address format: %s\n", ip);
    exit(1);
  }
}

/* ── Commands ────────────────────────────────────────────────── */

static int cmd_up(const char *iface) {
  int s = open_sock();
  struct ifreq ifr;
  memset(&ifr, 0, sizeof(ifr));
  strncpy(ifr.ifr_name, iface, IFNAMSIZ - 1);

  if (ioctl(s, SIOCGIFFLAGS, &ifr) < 0) {
    perror("SIOCGIFFLAGS");
    close(s);
    return 1;
  }

  ifr.ifr_flags |= IFF_UP;
  if (ioctl(s, SIOCSIFFLAGS, &ifr) < 0) {
    perror("SIOCSIFFLAGS");
    close(s);
    return 1;
  }

  printf("netadm: %s operational state set to UP\n", iface);
  close(s);
  return 0;
}

static int cmd_down(const char *iface) {
  int s = open_sock();
  struct ifreq ifr;
  memset(&ifr, 0, sizeof(ifr));
  strncpy(ifr.ifr_name, iface, IFNAMSIZ - 1);

  if (ioctl(s, SIOCGIFFLAGS, &ifr) < 0) {
    perror("SIOCGIFFLAGS");
    close(s);
    return 1;
  }

  ifr.ifr_flags &= ~IFF_UP;
  if (ioctl(s, SIOCSIFFLAGS, &ifr) < 0) {
    perror("SIOCSIFFLAGS");
    close(s);
    return 1;
  }

  printf("netadm: %s operational state set to DOWN\n", iface);
  close(s);
  return 0;
}

static int cmd_addr(const char *iface, const char *ip, const char *mask) {
  int s = open_sock();
  struct ifaliasreq ifra;
  struct sockaddr_in sin_addr, sin_mask, sin_broad;

  memset(&ifra, 0, sizeof(ifra));
  strncpy(ifra.ifra_name, iface, IFNAMSIZ - 1);

  // 1. Format the core IP address mapping
  set_sin(&sin_addr, ip);
  memcpy(&ifra.ifra_addr, &sin_addr, sizeof(sin_addr));

  // 2. Format Netmask mapping (default to 255.255.255.0 if not supplied)
  set_sin(&sin_mask, mask ? mask : "255.255.255.0");
  memcpy(&ifra.ifra_mask, &sin_mask, sizeof(sin_mask));

  // 3. Calculate broadcast layout based on IP and Mask parameters
  memset(&sin_broad, 0, sizeof(sin_broad));
  sin_broad.sin_len = sizeof(struct sockaddr_in);
  sin_broad.sin_family = AF_INET;
  sin_broad.sin_addr.s_addr =
      sin_addr.sin_addr.s_addr | ~sin_mask.sin_addr.s_addr;
  memcpy(&ifra.ifra_broadaddr, &sin_broad, sizeof(sin_broad));

  // 4. Send the Native Alias request to the kernel
  if (ioctl(s, SIOCAIFADDR, &ifra) < 0) {
    perror("SIOCAIFADDR");
    close(s);
    return 1;
  }

  // 5. Explicitly force the device UP to activate the link state
  struct ifreq ifr;
  memset(&ifr, 0, sizeof(ifr));
  strncpy(ifr.ifr_name, iface, IFNAMSIZ - 1);
  if (ioctl(s, SIOCGIFFLAGS, &ifr) == 0) {
    ifr.ifr_flags |= IFF_UP;
    ioctl(s, SIOCSIFFLAGS, &ifr);
  }

  printf("netadm: %s mapped to %s netmask %s\n", iface, ip,
         mask ? mask : "255.255.255.0");
  close(s);
  return 0;
}

static int cmd_show(void) {
  int s = open_sock();
  struct ifconf ifc;
  char buf[4096];

  memset(buf, 0, sizeof(buf));
  ifc.ifc_len = sizeof(buf);
  ifc.ifc_buf = buf;

  if (ioctl(s, SIOCGIFCONF, &ifc) < 0) {
    perror("SIOCGIFCONF");
    close(s);
    return 1;
  }

  char *cp = ifc.ifc_buf;
  char *end = ifc.ifc_buf + ifc.ifc_len;

  printf("%-10s  %-12s  %-6s  %-18s\n", "INTERFACE", "FLAGS", "STATUS",
         "ADDRESS");
  printf("─────────────────────────────────────────────────────────────\n");

  while (cp < end) {
    struct ifreq *ifr = (struct ifreq *)cp;
    struct ifreq cur;

    memset(&cur, 0, sizeof(cur));
    strncpy(cur.ifr_name, ifr->ifr_name, IFNAMSIZ - 1);

    short flags = 0;
    if (ioctl(s, SIOCGIFFLAGS, &cur) == 0) {
      flags = cur.ifr_flags;
    }

    char addr_str[64] = "";
    struct sockaddr *sa = &ifr->ifr_addr;

    if (sa->sa_family == AF_INET) {
      struct sockaddr_in *sin = (struct sockaddr_in *)sa;
      inet_ntop(AF_INET, &sin->sin_addr, addr_str, sizeof(addr_str));
      snprintf(addr_str + strlen(addr_str), sizeof(addr_str) - strlen(addr_str),
               " (IPv4)");
    } else if (sa->sa_family == AF_LINK) {
      /* Clean extraction of physical link interface hardware addresses */
      unsigned char *ptr =
          (unsigned char *)sa->sa_data + 4; // Shift past metadata tags
      snprintf(addr_str, sizeof(addr_str),
               "%02x:%02x:%02x:%02x:%02x:%02x (Link)", ptr[0], ptr[1], ptr[2],
               ptr[3], ptr[4], ptr[5]);
    } else {
      snprintf(addr_str, sizeof(addr_str), "unknown family: %d", sa->sa_family);
    }

    /* Filter output to make the dashboard clear and crisp */
    if (sa->sa_family == AF_INET || sa->sa_family == AF_LINK) {
      printf("%-10s  0x%04x        %-6s  %s\n", ifr->ifr_name,
             (unsigned short)flags, (flags & IFF_UP) ? "UP" : "DOWN", addr_str);
    }

    int len = (sa->sa_len > sizeof(struct sockaddr)) ? sa->sa_len
                                                     : sizeof(struct sockaddr);
    cp += sizeof(ifr->ifr_name) + len;
  }

  close(s);
  return 0;
}

/* ── Main execution loop ─────────────────────────────────────── */

static void usage(void) {
  printf("Usage:\n");
  printf("  netadm up   <iface>\n");
  printf("  netadm down <iface>\n");
  printf("  netadm addr <iface> <ip> [netmask]\n");
  printf("  netadm show\n");
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    usage();
    return 1;
  }

  if (strcmp(argv[1], "up") == 0) {
    if (argc < 3) {
      usage();
      return 1;
    }
    return cmd_up(argv[2]);
  } else if (strcmp(argv[1], "down") == 0) {
    if (argc < 3) {
      usage();
      return 1;
    }
    return cmd_down(argv[2]);
  } else if (strcmp(argv[1], "addr") == 0) {
    if (argc < 4) {
      usage();
      return 1;
    }
    return cmd_addr(argv[2], argv[3], argc >= 5 ? argv[4] : NULL);
  } else if (strcmp(argv[1], "show") == 0) {
    return cmd_show();
  } else {
    fprintf(stderr, "netadm: unknown command: %s\n", argv[1]);
    usage();
    return 1;
  }
}