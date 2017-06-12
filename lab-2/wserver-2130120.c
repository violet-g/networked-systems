//
// wserver.c -- a minimal, not entirely spec compliant, web server
//
// Copyright (c) 2007-2016 University of Glasgow
// All rights reserved.

#include <arpa/inet.h>
#include <sys/errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netdb.h>
#include <netinet/in.h>
#include <fcntl.h>    // For open()
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>   // For malloc()
#include <signal.h>
#include <unistd.h>
#include <dirent.h>

#define BUFLEN      1500
#define NUM_THREADS   10

// There are strong restrictions on what a signal handler is allowed to do. 
// See the C11 standard, section 7.14.1.1 paragraph 5 for details (the full
// standard is available for purchase from ISO, but the final working draft
// is online at http://www.open-std.org/jtc1/sc22/WG14/www/docs/n1570.pdf).
static volatile sig_atomic_t shutdown_requested = 0;

static void
signal_handler(int sig)
{
  if (sig == SIGINT) {
    shutdown_requested = 1;
  }
}

// Work queue implementation:

struct work_queue_elem {
  int                      fd;
  struct work_queue_elem  *next;
};

struct work_queue {
  pthread_mutex_t          lock;
  struct work_queue_elem  *head;
  int                      should_exit;
  int                      worker_waiting;
  pthread_cond_t           worker_cv;
};

static struct work_queue *
wq_init(void)
{
  struct work_queue *wq = malloc(sizeof(struct work_queue));

  wq->head           = NULL;
  wq->should_exit    = 0;
  wq->worker_waiting = 0;

  pthread_mutex_init(&wq->lock, NULL);
  pthread_cond_init(&wq->worker_cv, NULL);

  return wq;
}

static void
wq_add(struct work_queue* wq, int connection_fd)
{
  pthread_mutex_lock(&wq->lock);
  if (!wq->should_exit) {
    struct work_queue_elem  *wqe = malloc(sizeof(struct work_queue_elem));

    wqe->fd   = connection_fd;
    wqe->next = wq->head;

    wq->head = wqe;

    if (wq->worker_waiting) {
      pthread_cond_signal(&wq->worker_cv);
    }
  }
  pthread_mutex_unlock(&wq->lock);
}

static int
wq_get(struct work_queue *wq)
{
  int                      fd;
  struct work_queue_elem  *wqe;

  pthread_mutex_lock(&wq->lock);

  while (wq->head == NULL) {
    if (wq->should_exit) {
      pthread_mutex_unlock(&wq->lock);
      return -1;
    }

    wq->worker_waiting++;
    pthread_cond_wait(&wq->worker_cv, &wq->lock);
    wq->worker_waiting--;
  }
  wqe      = wq->head;
  wq->head = wqe->next;

  pthread_mutex_unlock(&wq->lock);

  fd = wqe->fd;

  free(wqe);

  return fd;
}

static int
wq_should_exit(struct work_queue *wq)
{
  int should_exit;

  pthread_mutex_lock(&wq->lock);
  should_exit = wq->should_exit;
  pthread_mutex_unlock(&wq->lock);
  return should_exit;
}

static void 
wq_shutdown(struct work_queue *wq)
{
  pthread_mutex_lock(&wq->lock);
  wq->should_exit = 1;
  pthread_cond_broadcast(&wq->worker_cv);
  pthread_mutex_unlock(&wq->lock);
}

static int
create_socket(void)
{
  int                 fd;
  struct sockaddr_in  addr;

  fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd == -1) {
    perror("Unable to create socket");
    return -1;
  }

  addr.sin_family      = AF_INET;
  addr.sin_port        = htons(8080);
  addr.sin_addr.s_addr = INADDR_ANY;

  if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) == -1) {
    perror("Unable to bind to port");
    return -1;
  }

  if (listen(fd, 4) == -1) {
    perror("Unable to listen for connections");
    return -1;
  }

  return fd;
}

static int
send_response(int fd, const char *data, size_t datalen)
{
  size_t  offset = 0;
  ssize_t wrote  = 0;
#ifdef __APPLE__
  int flags = 0;  // macOS doesn't support MSG_NOSIGNAL
#else
  int flags = MSG_NOSIGNAL;
#endif

  do {
    if ((wrote = send(fd, data + offset, datalen - offset, flags)) == -1) {
      return -1;
    } else {
      offset += (size_t) wrote;
    }
  } while (offset < datalen);

  return 0;
}

static int
send_response_200(int fd, char *filename, int inf, int id)
{
  // File exists, send OK response:
  struct stat  fs;
  char        *extn;
  char         headers[BUFLEN];
  char         buf[BUFLEN];
  ssize_t      rlen;

  // Find file size, and generate Content-Length:
  fstat(inf, &fs);
  sprintf(buf, "Content-Length: %d\r\n", (int) fs.st_size);

  // Generate and send Content-Type: based on the extension
  extn = strrchr(filename, '.');
  if (extn == NULL) {
    // No extension on the requested filename
    sprintf(headers, "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n%s\r\n", buf);
  } else if (strcmp(extn, ".html") == 0) {
    sprintf(headers, "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n%s\r\n", buf);
  } else if (strcmp(extn, ".htm") == 0) {
    sprintf(headers, "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n%s\r\n", buf);
  } else if (strcmp(extn, ".css") == 0) {
    sprintf(headers, "HTTP/1.1 200 OK\r\nContent-Type: text/css\r\n%s\r\n", buf);
  } else if (strcmp(extn, ".txt") == 0) {
    sprintf(headers, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n%s\r\n", buf);
  } else if (strcmp(extn, ".jpg") == 0) {
    sprintf(headers, "HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\n%s\r\n", buf);
  } else if (strcmp(extn, ".jpeg") == 0) {
    sprintf(headers, "HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\n%s\r\n", buf);
  } else {
    // Unknown extension
    sprintf(headers, "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n%s\r\n", buf);
  }

  if (send_response(fd, headers, strlen(headers)) == -1) {
    return -1;
  }

  // Send the requested file
  while ((rlen = read(inf, buf, BUFLEN)) > 0) {
    // The cast from ssize_t to size_t is safe, since the previous line
    // demonstrates that the value is non-negative.
    // MINOR FIX: Use the var rlen
    if (send_response(fd, buf, rlen) == -1) {
      return -1;
    }
  }

  printf("responder %d: 200 %s (%d bytes)\n", id, filename, (int) fs.st_size);
  return 0;
}

// EXTENSION
static int
send_response_200_listing(int fd, DIR *dir, char *filename, int id)
{

  // Print listing of the contents of the directory
  char *buffer = NULL;
  char *content = NULL;
  int filename_size = strlen(filename);
  int d_entry_size = 0;
  int line_size = 0;
  int total_size = 1;
  struct dirent *entry;

  // Read dir entry by entry
  while ((entry = readdir(dir)) != NULL) {
    d_entry_size = strlen(entry->d_name);
    // Calculate the size of each list element
    line_size = 27 + filename_size + d_entry_size*2;
    // Increase the total size of the content
    total_size += line_size;
    if ((content = realloc(content, total_size)) == NULL) {
      return -1;
    }
    // Add the new list element to the content
    sprintf(content + total_size - line_size - 1,
            "<li><a href=\"%s/%s\">%s</a></li>\r\n",
            filename, entry->d_name, entry->d_name);
  }

  if ((buffer = (char*)malloc(total_size + 96 + 62 + 5)) == NULL) {
    return -1;
  }

  printf("responder %d: 200 %s\n", id, filename);

  // Add content to buffer
  sprintf(buffer, "HTTP/1.1 200 OK\r\n"
                  "Content-Length: %d\r\n"
                  "Content-Type: text/html\r\n"
                  "\r\n"
                  "<html>\r\n"                                     //   8
                  "<head>\r\n"                                     //   8
                  "<title>Directory Listings</title>\r\n"          //   35
                  "</head>\r\n"                                    //   9
                  "<body>\r\n"                                     //   8
                  "<ul>%s</ul>\r\n"                                //   11 + total_size - 1
                  "</body>\r\n"                                    //   9
                  "</html>\r\n",                                   //   9
          96 + total_size, content);                               // total: 96 + total_size
  
  free(content);

  // Send the generated response
  if (send_response(fd, buffer, strlen(buffer)) == -1) {
    free(buffer);
    return -1;
  }

  free(buffer);

  return 0;
}

// EXTENSION
static int
send_response_307(int fd, char *filename, int id)
{
  
  // Redirect to index.html
  char buffer[65535];
  
  printf("responder %d: 307 %s\n", id, filename);

  sprintf(buffer, "HTTP/1.1 307 Temporary Redirect\r\n"
                  "Location: %s\r\n"
                  "Content-Length: 143\r\n"
                  "Content-Type: text/html\r\n"
                  "\r\n"
                  "<html>\r\n"                                     //   8
                  "<head>\r\n"                                     //   8
                  "<title>Redirected</title>\r\n"                  //  27
                  "</head>\r\n"                                    //   9
                  "<body>\r\n"                                     //   8
                  "<p>Redirecting ...</p>\r\n"                     //  24
                  "</body>\r\n"                                    //   9
                  "</html>\r\n",                                   //   9
         filename);                                                // total: 102

  return send_response(fd, buffer, strlen(buffer));
}

static int
send_response_404(int fd, char *filename, int id)
{
  // Requested file doesn't exist, send an error
  char buffer[65535];
  
  printf("responder %d: 404 %s\n", id, filename);

  sprintf(buffer, "HTTP/1.1 404 File Not Found\r\n"
                  "Content-Type: text/html\r\n"
                  "Content-Length: 113\r\n"
                  "\r\n"
                  "<html>\r\n"                              //  8
                  "<head>\r\n"                              //  8
                  "<title> 404 File Not Found </title>\r\n" // 37
                  "</head>\r\n"                             //  9
                  "<body>\r\n"                              //  8
                  "<p> File not found </p>\r\n"             // 25
                  "</body>\r\n"                             //  9
                  "</html>\r\n"                             //  9
         );                                                 // 113 total
 
  return send_response(fd, buffer, strlen(buffer));
}

static int
send_response_500(int fd, char *filename, int id)
{
  // Internal server error, sent whenever something unexpected is received.
  char buffer[65535];

  printf("responder %d: 500 %s\n", id, filename);

  sprintf(buffer, "HTTP/1.1 500 Internal Server Error\r\n"
                  "Content-Type: text/html\r\n"
                  "Content-Length: 120\r\n"
                  "Connection: close\r\n"
                  "\r\n"
                  "<html>\r\n"                                     //   8
                  "<head>\r\n"                                     //   8
                  "<title> 500 Internal Server Error </title>\r\n" //  44
                  "</head>\r\n"                                    //   9
                  "<body>\r\n"                                     //   8
                  "<p> Internal Error </p>\r\n"                    //  25
                  "</body>\r\n"                                    //   9
                  "</html>\r\n"                                    //   9
         );                                                        // total: 120 

  return send_response(fd, buffer, strlen(buffer));
}

static int
hostname_matches(char *headers)
{
  char  *host; 
  char  *colonpos;
  char   hostname[256];
  char   myhostname[256];
  char   domainname[256];

  // Parse the HTTP headers, to find and validate the "Host:" header.
  // Note that we search for a newline followed by "Host:", to avoid
  // matching other headers that end in "Host:".
  host = strstr(headers, "\nHost:");
  if ((host == NULL) || (sscanf(host, "\nHost: %255s\n", hostname) != 1)) {
    printf("Cannot parse HTTP Host: Header\n");
    return 0;
  }

  // When running on a non-standard port, browsers include a colon 
  // and the port number in the "Host:" header. Strip this out.
  if ((colonpos = strchr(hostname, ':')) != NULL) {
    *colonpos = '\0';
  }

  gethostname(myhostname, 256);
  if (strcmp(hostname, myhostname) != 0) {
    // The hostname in the request didn't match the return from gethostname().
    // There are three possible reasons for this:
    // 1) The hostname in the request doesn't match our hostname
    // 2) The hostname in the request doesn't include the domain name, but
    //    gethostname() on this machine does (gethostname() works this way
    //    on MacOS X)
    // 3) The hostname in the request might include the full domain name, 
    //    while gethostname() on this machine returns only the host part
    //    (this is how gethostname() works on Linux)
    // Cases (2) and (3) are okay, and should be accepted, so we check for
    // these now.
    char  myNameDom[512];
    char  reNameDom[512];

    getdomainname(domainname, 256);
    sprintf(myNameDom, "%s.%s", myhostname, domainname);
    sprintf(reNameDom, "%s.%s", hostname, domainname);

    if ((strcmp(hostname, myNameDom) != 0) && 
       (strcmp(reNameDom, myhostname) == 0))
    {
      return 0;
    }
  }
  return 1;
}

static char *
read_headers(int fd)
{
  char     buf[BUFLEN];
  char    *headers   = malloc(1);
  size_t   headerLen = 0;
  ssize_t  rlen;

  headers[0] = '\0';
  while (strstr(headers, "\r\n\r\n") == NULL) {
    rlen = recv(fd, buf, BUFLEN, 0);
    if (rlen ==  0) { 
      // Connection closed by client
      free(headers);
      return NULL;
    } else if (rlen < 0)  {
      free(headers);
      perror("Cannot read HTTP request");
      return NULL;
    } else {
      // The cast is safe, since we've checked rlen is non-negative
      headerLen += (size_t) rlen;
      headers = realloc(headers, headerLen + 1);
      strncat(headers, buf, (size_t) rlen);
    }

    if (shutdown_requested) {
      printf("shutdown requested\n");
      return NULL;
    }
  }

  return headers;
}

struct response_params {
  struct work_queue *wq;
  int                id;
};

static void *
response_thread(void *arg)
{
  struct response_params *params = (struct response_params *) arg;
  struct work_queue      *wq = params->wq;
  int                     id = params->id;
  int                     fd;

  printf("responder %d: created\n", id);

  while ((fd = wq_get(wq)) != -1) {
    printf("responder %d: connection opened\n", id);
    while (1) {
      char    *headers = NULL;
      char     basename[1024];
      char     filename[1024+8];
      int      inf;
      DIR     *dir;

      // Retrieve the request
      if ((headers = read_headers(fd)) == NULL) {
        break;
      }

      // Parse the HTTP request, to determine the requested filename.
      // Note that we specify a maximum field width, to avoid buffer
      // overflow attacks when parsing long filenames.
      if (sscanf(headers, "GET %1023s HTTP/1.1", basename) != 1) {
        printf("Cannot parse HTTP GET request\n");
        send_response_500(fd, basename, id);
        free(headers);
        break;
      }

      if (!hostname_matches(headers)) {
        send_response_404(fd, basename, id);
        free(headers);
        break;
      }

      sprintf(filename, "website%s", basename);
      
      // EXTENSION
      if ((dir = opendir(filename)) != NULL) {
        // Filename represents a directory
        char tempFilename[1024];
        sprintf(tempFilename, "%s/index.html", filename);
        if ((inf = open(tempFilename, O_RDONLY, 0)) != -1) {
          // Index.html exists
          if (basename[strlen(basename) - 1] == '/') {
            sprintf(tempFilename, "%sindex.html", basename);
          } else {
            sprintf(tempFilename, "%s/index.html", basename);
          }
          // Redirect to index.html
          if (send_response_307(fd, tempFilename, id) == -1) {
            free(headers);
            break;
          }
        } else {
          // Index.html was not found in the directory
          if (basename[strlen(basename) - 1] == '/') {
            basename[strlen(basename) - 1] = '\0';
          }
          // Print directory listings dynamically
          if (send_response_200_listing(fd, dir, basename, id) == -1) {
            free(headers);
            break;
          }
        }
        closedir(dir);
      }

      if ((inf = open(filename, O_RDONLY, 0)) == -1) {
        if (send_response_404(fd, filename, id) == -1) {
          free(headers);
          break;
        }
      } else {
        if (send_response_200(fd, filename, inf, id) == -1) {
          free(headers);
          break;
        }
      }
     
      close(inf);
      free(headers);

    };

    if (shutdown_requested) {
      printf("responder %d: shutdown requested\n", id);
      wq_shutdown(wq);
    }

    close(fd);
    printf("responder %d: connection closed\n", id);
  };

  printf("responder %d: exit\n", id);
  return NULL;
}

static void 
process_connections(struct work_queue *wq)
{
  int                sfd;
  int                cfd;
  struct sockaddr_in caddr;
  socklen_t          caddr_len = sizeof(caddr);

  printf("listener: start\n");

  if ((sfd = create_socket()) == -1) {
    printf("listener: unable to bind socket, exit\n");
    return;
  }

  while (!wq_should_exit(wq)) {
    if ((cfd = accept(sfd, (struct sockaddr *) &caddr, &caddr_len)) == -1) {
      perror("listener: unable to accept connection");
      break;
    } else {
#ifdef __APPLE__ 
      // The MSG_NOSIGNAL flag to send() isn't supported on macOS, so set 
      // the SO_NOSIGPIPE option on the socket as a workaround.
      int opt = 1;
      if (setsockopt(cfd, SOL_SOCKET, SO_NOSIGPIPE, &opt, sizeof(opt)) == -1) {
        perror("listener: warning - cannot set SO_NOSIGPIPE");
      }
#endif 

      wq_add(wq, cfd);
    }
  }

  close(sfd);

  printf("listener: done\n");
}

int 
main(void)
{
  int                id;
  struct work_queue *wq = wq_init();
  pthread_t          threads[NUM_THREADS];

  // Catch SIGINT (ctrl-c) and signal main loop to exit
  signal(SIGINT, signal_handler);

  for (id = 0; id < NUM_THREADS; id++) {
    struct response_params *p = malloc(sizeof(struct response_params));

    p->wq = wq;
    p->id = id;;

    pthread_create(&threads[id], NULL, response_thread, p);
  }

  process_connections(wq);

  for (id = 0; id < NUM_THREADS; id++) {
    printf("listener: waiting for responder %d to exit... ", id);
    fflush(stdout);
    pthread_join(threads[id], NULL);
    printf("done\n");
  }

  printf("listener: exit\n");

  free(wq);

  return 0;
}

// vim: set ts=2 sw=2 tw=0 et ai:
