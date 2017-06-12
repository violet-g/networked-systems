#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netdb.h>

#define IPV4LEN 32
#define IPV6LEN 128

int main(int argc, const char* argv[]) {
  
  int arg, i;
  char *address;
  struct addrinfo hints, *ai, *ai0;

  if (argc < 2) {
    printf("You must provide at least one domain name.");
    return 1;
  }

  memset(&hints, 0, sizeof(hints));
  hints.ai_family = PF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  for (arg = 1; arg < argc; arg++) {
    if ((i = getaddrinfo(argv[arg], "5000", &hints, &ai0)) != 0) {
      printf("Unable to look up IP address: %s", gai_strerror(i));
      return 2;
    }
   
    for (ai = ai0; ai != NULL; ai = ai->ai_next) {
      switch(ai->ai_family) {
        case AF_INET:
          // IPv4
          if ((address = malloc(IPV4LEN)) != NULL) {
            inet_ntop(AF_INET, &((struct sockaddr_in *)ai->ai_addr)->sin_addr, address, IPV4LEN);
            printf("%s IPv4 %s\n", argv[arg], address);
          }
          break;
        case AF_INET6:
          // IPv6
          if ((address = malloc(IPV6LEN)) != NULL) {
            inet_ntop(AF_INET6, &((struct sockaddr_in6 *)ai->ai_addr)->sin6_addr, address, IPV6LEN);
            printf("%s IPv6 %s\n", argv[arg], address);
          }
          break;
        default:
          printf("Cannot recognise address type.");
      }
      
      if (address != NULL) {
        free(address);
      }
    }
  }

  freeaddrinfo(ai0);

  return 0;
}
