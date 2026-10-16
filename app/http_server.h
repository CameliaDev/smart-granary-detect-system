#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#ifndef HTTP_SERVER_PORT
#define HTTP_SERVER_PORT 8081
#endif

void *http_server_thread(void *argument);

#endif
