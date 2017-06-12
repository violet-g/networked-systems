#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netdb.h>

#define BUFLEN 1500

int main(int argc, char *argv[]) {
  int i; 
  ssize_t wlen;
  int fd;
  char *buf = malloc(BUFLEN);
  struct addrinfo hints, *ai, *ai0;
  int flags = MSG_NOSIGNAL;

  if (argc != 2) {
    printf("Usage: %s <hostname>\n", argv[0]);
    return 1;
  }

  // Look up the IP address of the hostname specified on the command line
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = PF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  if ((i = getaddrinfo(argv[1], "5000", &hints, &ai0)) != 0) {
    printf("Unable to look up IP address: %s", gai_strerror(i));
    return 2;
  }

  // Try to connect to each address in turn
  for (ai = ai0; ai != NULL; ai = ai->ai_next) {
    // Create a TCP/IP socket
    fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd == -1) {
      continue;       // Try the next address
    }

    // Connect to the server
    if (connect(fd, ai->ai_addr, ai->ai_addrlen) == -1) {
      close(fd);
      continue;       // Try the next address
    }

    // Send "Hello, world!"
    sprintf(buf, "Hello, world!");
    wlen = send(fd, buf, strlen(buf), flags);
    if (wlen == -1) {
      perror("Unable to send request");
      close(fd);
      return 2;
    }

    // Close the connection and exit
    freeaddrinfo(ai0);
    close(fd);
    free(buf);
    return 0;
  }

  // Couldn't connect
  printf("Unable to connect\n");
  return 1;
}
