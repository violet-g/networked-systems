#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <stdio.h>
#include <unistd.h>

#define BUFLEN 1500

int main(void) {
  int fd, conn_fd; 
  ssize_t rlen;
  struct sockaddr_in addr, conn_addr;
  socklen_t conn_addr_len;
  char buf[BUFLEN];
  int flags = 0;

  // Create a TCP/IP socket
  fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd == -1) {
    perror("Unable to create socket");
    return 1;
  }

  // Bind to port 5000
  addr.sin_family = AF_INET;
  addr.sin_port = htons(5000);
  addr.sin_addr.s_addr = INADDR_ANY;

  if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) == -1) {
    perror("Unable to bind to port");
    return 1;
  }

  // Listen for a connection
  if (listen(fd, 1) == -1) {
    perror("Unable to listen for connection");
    return 1;
  }

  // Accept the first connection
  conn_addr_len= sizeof(struct sockaddr);
  if ((conn_fd = accept(fd, (struct sockaddr *) &conn_addr, &conn_addr_len)) == -1){
    perror("Unable to accept connection");
    return 1;
  }

  // Retrieve the data
  while ((rlen = recv(conn_fd, buf, BUFLEN, flags)) > 0) {
    int i;
    for (i = 0; i < rlen; i++) {
      printf("%c", buf[i]);
    }
    printf("\n");
  }

  // Close the connection, and the underlying listening socket
  close(conn_fd);
  close(fd);

  return 0;
}
