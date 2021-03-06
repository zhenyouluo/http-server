#include "http-server/http-server.h"
#include <stdlib.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/types.h>
#include <netdb.h>          /* hostent struct, gethostbyname() */
#include <arpa/inet.h>      /* inet_ntoa() to format IP address */
#include <netinet/in.h>     /* in_addr structure */
#include <fcntl.h>
#include <unistd.h>
#include <assert.h>
#include <strings.h>
#include "event.h"
#include "build_config.h"

#if !defined(HTTP_SERVER_HAVE_SELECT)
#error "Unable to compile this file"
#endif


#ifndef SLIST_FOREACH_SAFE
#define SLIST_FOREACH_SAFE(var, head, field, tvar) \
for ((var) = SLIST_FIRST((head)); \
(var) && ((tvar) = SLIST_NEXT((var), field), 1); \
(var) = (tvar))
#endif

typedef struct
{
    // flags for clients
    int flags[64];
    http_server * srv;
} Http_server_event_handler;

static int _default_opensocket_function(void * clientp);
static int _default_closesocket_function(http_server_socket_t sock, void * clientp);
static int _default_socket_function(void * clientp, http_server_socket_t sock, int flags, void * socketp);

static int Http_server_select_event_loop_init(http_server * srv)
{
    // Create new default event handler
    Http_server_event_handler * ev = calloc(1, sizeof(Http_server_event_handler));
    ev->srv = srv;
    srv->socket_data = ev;
    srv->event_loop_data_ = ev;

    srv->sock_listen = HTTP_SERVER_INVALID_SOCKET;
    srv->sock_listen_data = ev;
    srv->opensocket_func = &_default_opensocket_function;
    srv->opensocket_data = ev;
    srv->closesocket_func = &_default_closesocket_function;
    srv->closesocket_data = ev;
    srv->socket_func = &_default_socket_function;

    return 0;
}

static void Http_server_select_event_loop_free(http_server * srv)
{
    Http_server_event_handler * ev = srv->event_loop_data_;
    assert(ev);
    free(ev);
}

static int _default_opensocket_function(void * clientp)
{
    Http_server_event_handler * ev = clientp;
    http_server * srv = ev->srv;
    int s;
    int optval;
    // create default ipv4 socket for listener
    s = socket(AF_INET, SOCK_STREAM, 0);
    http_server__debug(srv, 1, "open socket: %d\n", s);
    if (s == -1)
    {
        return HTTP_SERVER_INVALID_SOCKET;
    }
    // TODO: this part should be separated somehow
    // set SO_REUSEADDR on a socket to true (1):
    optval = 1;
    if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof optval) == -1)
    {
        perror("setsockopt");
        return HTTP_SERVER_INVALID_SOCKET;
    }

    int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in sin;
    sin.sin_port = htons(5000);
    sin.sin_addr.s_addr = 0;
    sin.sin_addr.s_addr = INADDR_ANY;
    sin.sin_family = AF_INET;

    if (bind(s, (struct sockaddr *)&sin,sizeof(struct sockaddr_in) ) == -1)
    {
        perror("bind");
        return HTTP_SERVER_INVALID_SOCKET;
    }

    if (listen(s, 128) < 0) {
        perror("listen");
        return HTTP_SERVER_INVALID_SOCKET;
    }
    return s;
}

static int _default_closesocket_function(http_server_socket_t sock, void * clientp)
{
    Http_server_event_handler * ev = clientp;
    http_server * srv = ev->srv;
    http_server__debug(srv, 1, "close(%d)\n", sock);
    ev->flags[sock] = 0;
    if (close(sock) == -1)
    {
        perror("close");
        abort();
    }
    return HTTP_SERVER_OK;
}

static int _default_socket_function(void * clientp, http_server_socket_t sock, int flags, void * socketp)
{
    // Data assigned to listening socket is a `fd_set`
    Http_server_event_handler * ev = clientp;
    if (flags & HTTP_SERVER_POLL_REMOVE)
    {
        ev->flags[sock] = 0;
    }
    else
    {
        ev->flags[sock] |= flags;
    }
    return HTTP_SERVER_OK;
}

static int Http_server_select_event_loop_run(http_server * srv)
{
    int r;
    Http_server_event_handler * ev = srv->sock_listen_data;
    do
    {
        fd_set rd, wr;
        FD_ZERO(&rd);
        FD_ZERO(&wr);

        int nsock = 0;
        // Check if client exists on the list
        http_server_client * it = NULL;
        SLIST_FOREACH(it, &srv->clients, next)
        {
            assert(it->sock > -1);
            if (ev->flags[it->sock] & HTTP_SERVER_POLL_IN)
            {
                http_server__debug(srv, 1, "rd=%d\n", it->sock);
                FD_SET(it->sock, &rd);
                if (it->sock > nsock)
                {
                    nsock = it->sock;
                }
            }
            if (ev->flags[it->sock] & HTTP_SERVER_POLL_OUT)
            {
                http_server__debug(srv, 1, "wr=%d\n", it->sock);
                FD_SET(it->sock, &wr);
                if (it->sock > nsock)
                {
                    nsock = it->sock;
                }
            }
        }
        if (ev->flags[srv->sock_listen] & HTTP_SERVER_POLL_IN)
        {
            http_server__debug(srv, 1, "rd sock listen %d\n", srv->sock_listen);
            FD_SET(srv->sock_listen, &rd);
            if (srv->sock_listen > nsock)
            {
                nsock = srv->sock_listen;
            }
        }

        if (nsock == 0)
        {
            http_server__debug(srv, 1, "no more events..\n");
            break;
        }
                
        
        // This will block
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        http_server__debug(srv, 1, "select(%d, {%d}, ...)\n", nsock + 1, srv->sock_listen);
        r = select(nsock + 1, &rd, &wr, 0, &tv);
        http_server__debug(srv, 1, "r=%d\n", r);
        if (r == -1)
        {
            perror("select");
            abort();
        }
        else
        {
            http_server__debug(srv, 1, "select result=%d\n", r);
        }
        if (r == 0)
        {
            // Nothing to do...
            continue;
        }
        
        // Check if client exists on the list
        it = NULL;
        http_server_client * it_temp;
        SLIST_FOREACH_SAFE(it, &srv->clients, next, it_temp)
        {
            assert(it);
            if (FD_ISSET(it->sock, &rd))
            {
                assert(ev->flags[it->sock] & HTTP_SERVER_POLL_IN);
                ev->flags[it->sock] ^= HTTP_SERVER_POLL_IN;
                http_server__debug(srv, 1, "client data is available fd=%d\n", it->sock);
                int action_result = http_server_socket_action(srv, it->sock, HTTP_SERVER_POLL_IN);
                if (action_result != HTTP_SERVER_OK)
                {
                    if (action_result != HTTP_SERVER_CLIENT_EOF)
                    {
                        http_server__debug(srv, 1, "failed to do socket action on fd=%d\n", it->sock);
                    }
                    continue;
                }
            }
            if (FD_ISSET(it->sock, &wr))
            {
                http_server__debug(srv, 1, "send outgoing data=%d\n", it->sock);
                // Send outgoing data
                assert(ev->flags[it->sock] & HTTP_SERVER_POLL_OUT);
                ev->flags[it->sock] ^= HTTP_SERVER_POLL_OUT;
                if (http_server_socket_action(srv, it->sock, HTTP_SERVER_POLL_OUT) != HTTP_SERVER_OK)
                {
                    continue;
                }
                http_server__debug(srv, 1, "sent success!\n");
            }
        }

        if (FD_ISSET(srv->sock_listen, &rd))
        {
            http_server__debug(srv, 1, "action on sock listen\n");
            // Check for new connection
            // This will create new client structure and it will be
            // saved in list.
            assert(ev->flags[srv->sock_listen] & HTTP_SERVER_POLL_IN);
            ev->flags[srv->sock_listen] ^= HTTP_SERVER_POLL_IN;
            if (http_server_socket_action(srv, srv->sock_listen, HTTP_SERVER_POLL_IN) != HTTP_SERVER_OK)
            {
                http_server__debug(srv, 1, "unable to accept new client\n");
                continue;
            }
            continue;
        }
    }
    while (r != -1);
    return HTTP_SERVER_OK;
}

struct Http_server_event_loop Http_server_event_loop_select = {
    .init_fn = &Http_server_select_event_loop_init,
    .free_fn = &Http_server_select_event_loop_free,
    .run_fn = &Http_server_select_event_loop_run
};
