/*
 * proxy.c 
 * Team Members:
 * ramyakrv - Ramya Krishnan 
 * tkudav - Tejal Kudav
 *
 * Proxy Implementation:
 *
 * Description: proxy.c acts as an intermediary between a client and server.
 * Using threads it is able to operate concurently and serve multiple clients.
 * It parses HTTP requests from them,connects to a requested remote server,
 * forwards the clients' requests to the server and forwards the response
 * from the server back to the client. It uses semaphores to prevent race 
 * conditions between threads.
 * note about csapp.c functions : didn't use open_clientfd; instead developed a
 * thread-safe version of the same called openclientfd_r.
 * handled unix_error etc to call pthread_exit instead of exit so that the
 * proxy doesn't die whenever something bad happens.
 */


#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "csapp.h"
#include "cache.h"

/* -------------------------- Basic constants ------------------------*/  
static const char *user_agent_hdr = "User-Agent: Mozilla/5.0 (X11; Linux \
    x86_64;rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\n";

static const char *accept_hdr = "Accept: accept_hdr/html,application/xhtml+xml,\
    application/xml;q=0.9,*/*;q=0.8\r\n";

static const char *accept_encoding_hdr = "Accept-Encoding: gzip, deflate\r\n";
static const char *connection_hdr = "Connection: close\r\n";
static const char *proxy_connection_hdr = "Proxy-Connection: close\r\n";

#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400
#define MAX_METHOD_LENGTH 10
#define MAX_VERSION_LENGTH 15


/* -------------------------- Global Variables ------------------------*/  
pthread_rwlock_t cache_lock;
sem_t mutex;


/* ------------Function prototype for internal helper routines-----------*/ 
void connection_handler(int client_fd);
void server_to_client(rio_t server, int client_fd, char** urii);
void *thread(void *vargp);
int openclientfd(char *hostname, int port);
void clean(char* path, char* hostname, char* url);
ssize_t Rio_readnb_w(rio_t *rp, void *usrbuf, size_t n);
ssize_t Rio_readlineb_w(rio_t *rp, void *usrbuf, size_t maxlen);
void Rio_writen_w(int fd, void *usrbuf, size_t n);
		 

rio_t proxy_to_server(char** hostnamee, int* port_num, char** pathh,
    void* client_buffer, rio_t client, int * server_fd, int client_fd);
    
int parse_url(char** urll, char** path, char** hostnamee,
    int* port_num);


void clienterror(int fd, char *cause, char *errnum, 
    char *shortmsg, char *longmsg);


/* -----------------------------Main Function---------------------------*/ 


/* Main function waits and accepts requests and creates threads to
 * handle them. Each thread is passed to the thread function
 * so it is detached properly and then passed on to the connection_handler
 */
int main(int argc, char** argv)
{
    int listen_fd, port, *client_fd;
    unsigned int clientlen;
    struct sockaddr_in clientaddr;
    pthread_t tid;

    /* Ignoring sigpipe signal */
    Signal(SIGPIPE, SIG_IGN);

    /* Check command line args */
    if (argc != 2)
    {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(1);
    }
    
    /* Getting port number if theres one */
    port = atoi(argv[1]);

    /* Exit if port number is out of bounds */
	if(port<1024 || port>65535)
	{
		fprintf(stderr, "Port number should be between \
			1024 and 65536.\n");
		exit(1);
	}
    
    
    /* Open a port for listening for the client */
    listen_fd = Open_listenfd(port);

    /* Initializing lock for cache and mutexes for threads */
    pthread_rwlock_init(&cache_lock, 0);
    sem_init(&mutex, 0, 1);

    /* While loop to continuously create threads */
    while(1)
    {
        clientlen = sizeof(clientaddr);
        client_fd = Malloc(sizeof(int));
        *client_fd = Accept(listen_fd, (SA *)&clientaddr, &clientlen);
        if(*client_fd < 0)
        {
            Free(client_fd);
            return -1;
        }
        else
            Pthread_create(&tid, NULL, thread, client_fd);
    }
    return 0;
}

/* -------------------------Thread Function-------------------------*/ 

/* Thread funtion which detaches each thread and frees the client_fd
 * which was malloced in th main functon. Each thread is then passed
 * on to the connection_handler function.
 */
void *thread(void *vargp)
{
    int client_fd = *((int *)vargp);
    Free(vargp);
    Pthread_detach(pthread_self());
    connection_handler(client_fd);
    return NULL;
}



/* -----------------Connection Handler Function---------------------*/ 

/* connection_handler function reads the client buffer, parses the url
 * through the parse_url function, creates the request to send the server
 * with the proxy_to_server function, and finally sends the data back to
 * the client with the server_to_client function
 */
void connection_handler(int client_fd)
{
    rio_t client, server;
    char client_buffer[MAXBUF], method[MAX_METHOD_LENGTH];
    char version[MAX_VERSION_LENGTH];
    char* url = Malloc(sizeof(char) * MAXLINE);
    int server_fd;

    memset(client_buffer, 0, MAXLINE);
    memset(method, 0, MAX_METHOD_LENGTH);
    memset(url, 0, MAXLINE);
    memset(version, 0, MAX_VERSION_LENGTH);

    /* Initializing rio with client_fd */
    Rio_readinitb(&client, client_fd);

    /* Associating buffer with client */
    Rio_readlineb_w(&client, client_buffer, MAXLINE);

    /* Splits line into GET, the url and version number */
    sscanf(client_buffer, "%s %s", method, url);
    strcpy(version, "HTTP/1.0");

    /* It isn't a get request */
    if(strcmp(method, "GET"))
    {
        clienterror(client_fd, method, "501", "Not Implemented", 
                "Tiny does not implement this method");     
        Free(url);
        Close(client_fd);
        return;
    }

    /* It is a get request */
    if(!strcmp(method, "GET"))
    {
        char* path = Malloc(sizeof(char)*MAXBUF);
        char* hostname = Malloc(sizeof(char)*MAXBUF);
        int port_num;

        memset(path, 0, MAXBUF);
        memset(hostname, 0, MAXBUF);

        int checkUrl = parse_url(&url, &path, &hostname, &port_num);

        /* It wasn't able to parse url */
        if(checkUrl == -1)
        {
            Close(client_fd);
            clean(path, hostname, url);
            return;
        }

        /* Uri is hostname + path, so that we can refer to cache objects */
        char* uri = Malloc(sizeof(char)*MAXBUF);
        uri = strcpy(uri, hostname);
        uri = strcat(uri, path);

        pthread_rwlock_rdlock(&cache_lock);
        cacheLine* cline = readFromCache(uri);
        /* Found uri in the cache, so retrieve data and pass to client buffer*/
        if(cline != NULL){
            rio_writen(client_fd, cline->data, cline->size);
            pthread_rwlock_unlock(&cache_lock);
            clean(path, hostname, url);
            Free(uri);
            Close(client_fd);
            return;
        }
        pthread_rwlock_unlock(&cache_lock);

        /* Open server connection and send the information to server */
        server = proxy_to_server(&hostname, &port_num, &path, &client_buffer,
                                 client, &server_fd, client_fd);

        if (errno == EPIPE || errno == EBADRQC)
        {
            clean(path, hostname, url);
            Close(client_fd);
            if(server_fd > 0)
                Close(server_fd);
            return;
        }

        /* Send the server response to client again */
        server_to_client(server, client_fd, &uri);
        clean(path, hostname, url);
        Close(client_fd);
        if(server_fd > 0)
            Close(server_fd);
    }
}



/* -------------------------Clean Function-------------------------*/ 
/* clean - frees all the variables that we malloced in connection_handler
*/
void clean(char* path, char* hostname, char* url)
{
    Free(path);
    Free(hostname);
    Free(url);
}




/* --------------------Server to client Function-------------------------*/ 
void server_to_client(rio_t server, int client_fd, char** urii)
{
    char new_buffer[MAX_OBJECT_SIZE];
    /* We pass pointers to the earlier malloced string and dereference here*/
    char* uri = *urii;
    char* data = Malloc(sizeof(char)*MAX_CACHE_SIZE);
    memset(data, 0, MAX_CACHE_SIZE);
    memset(new_buffer, 0, MAXBUF);
    int line;
    int totalLength = 0;

    while((line =  Rio_readnb_w(&server, new_buffer, MAX_OBJECT_SIZE)) > 0)
    {
        Rio_writen_w(client_fd, new_buffer, line);
        memcpy(data, new_buffer, MAX_OBJECT_SIZE);
        totalLength += line;
        memset(new_buffer, 0, MAX_OBJECT_SIZE);
        if(errno == EPIPE || errno == ECONNRESET)
        {
            Free(uri);
            Free(data);
            return;
        }
    }

    if(totalLength > MAX_OBJECT_SIZE)
    {
        Free(data);
        Free(uri);
        return;
    }

    else
    {
        pthread_rwlock_wrlock(&cache_lock);
        writeToCache(uri, data, totalLength);
        pthread_rwlock_unlock(&cache_lock);
    }
}


/* --------------------proxy_to_server Function-------------------------*/ 

/* proxy_to_server function puts together the request to send to the server
 * and sends it. It takes the values from the parse_url function and
 * gets any remaining values from the client_buffer to put into the server
 * request
 */
rio_t proxy_to_server(char** hostnamee, int* port_numm, char** pathh,
                      void* client_buffer, rio_t client, int * server_fd,
                      int client_fd)
{
    rio_t server;
    int length;
    char transfer_path[MAXLINE], transfer_host[MAXLINE], transfer[MAXLINE];
    /* We pass pointers to earlier malloced strings and dereference here*/
    char *hostname = *hostnamee;
    char *path = *pathh;
    int port_num = *port_numm;
    int line;
    int flag = 1;

    memset(transfer_path, 0, MAXLINE);
    memset(transfer_host, 0, MAXLINE);
    memset(transfer, 0, MAXLINE);

    *server_fd = openclientfd(hostname, port_num);

    /* Bad request */
    if(*server_fd < 0)
    {
        /* Redirect to self-defined 404 page */
        memset(hostname, 0, MAXLINE);
        memset(path, 0, MAXLINE);
        hostname = "i.huffpost.com";
        path = "/gadgets/slideshows/4829/slide_4829_66794_large.jpg";
    }

    Rio_readinitb(&server, *server_fd);

    /* Writing the path to the transfer string */
    length = strlen("GET ") + strlen(path) + strlen(" HTTP/1.0");
    strcpy(transfer_path, "GET ");
    strcat(transfer_path, path);
    strcat(transfer_path, " HTTP/1.0\r\n");
    strcpy(transfer, transfer_path);

    /* Writing the hostname to the tranfer string */
    length = strlen(hostname) + strlen("Host: ");
    strcpy(transfer_host, "Host: ");
    strcat(transfer_host, hostname);
    strcat(transfer_host, "\r\n");
    strcat(transfer, transfer_host);

    /* Writing all the static variables to the transfer string */
    strcat(transfer, user_agent_hdr);
    strcat(transfer, accept_hdr);
    strcat(transfer, accept_encoding_hdr);
    strcat(transfer, connection_hdr);
    strcat(transfer, proxy_connection_hdr);

    /* Writing any of the remaining things in the 
     * client buffer to the transfer */
    while((line = Rio_readlineb_w(&client, client_buffer, MAXLINE))
        && line > 2)
    {
        if(!strncmp(client_buffer, "GET", 3) ||
           !strncmp(client_buffer, "Host:", 5) ||
           !strncmp(client_buffer, "User-Agent:", 11) ||
           !strncmp(client_buffer, "Accept:", 7) ||
           !strncmp(client_buffer, "Accept-Encoding", 15) ||
           !strncmp(client_buffer, "Proxy-Connection", 16) ||
           !strncmp(client_buffer, "Connection", 10))
        {
            flag = 0;
        }
        else
            flag = 1;

        if(flag)
            strcat(transfer, client_buffer);
    }

    strcat(transfer, "\r\n\0");

    /* Wrting the transfer string to the server */
    Rio_writen_w(*server_fd, (void *)transfer, strlen(transfer));

    if (errno == EPIPE)
        printf("broken pipe");
    return server;
}



/* --------------------------parse_url Function-------------------------*/ 

/* parse_url function splits the url into the hostname, port number and
 * the path, if the url is not valid it returns -1 otherwise if it is valid
 * it returns 0
 */
int parse_url(char** urll, char** pathh, char** hostnamee, int* port_num)
{
    int i = 7; /* Starting the traversal at 7 to account for http:// */
    char check[7];
    int curr_i = 7;

    /* We pass pointers to the malloced string and dereference here. */
    char * url = *urll;
    char * hostname = *hostnamee;
    char * path = *pathh;

    memset(hostname, 0, sizeof(hostname));
    memset(path, 0, sizeof(path));
    memset(check, 0, sizeof(check));

    /* Getting the hostname */
    while(url[i] && url[i] != '/' && url[i] != ':')
    {
        hostname[i-curr_i] = url[i];
        i++;
    }
    hostname[i-curr_i] = '\0';

    /* Getting the port */
    if(url[i] == ':')
        sscanf(&url[i+1], "%d%s", port_num, path);

    /* Default port if there is no port */
    else
    {
        sscanf(&url[i], "%s", path);
        *port_num = 80;
    }
   
    
    return 0;
}


/* --------------------------openclientfd Function-------------------------*/ 

/*
 * openclientfd - Thread safe version of the given open_clientfd
 * open connection to server at <hostname, port>
 * and return a socket descriptor ready for reading and writing.
 * Returns -1 and sets errno on Unix error.
 * Returns -2 and sets h_errno on DNS (gethostbyname) error.
 */
 
int openclientfd(char *hostname, int port)
{
    int clientfd;
    struct hostent *hp;
    struct sockaddr_in serveraddr;
    struct hostent ret;
    if ((clientfd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
    {
        return -1; /* check errno for cause of error */
    }
    /* Fill in the server's IP address and port */
    char buffer[MAXLINE];
    int errno;
    if ((gethostbyname_r(hostname, &ret, buffer, MAXLINE, &hp, &errno)) != 0)
        return errno; /* check h_errno for cause of error */
    bzero((char *) &serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    if(!hp)
        return -1;
    bcopy((char *)hp->h_addr_list[0],
        (char *)&serveraddr.sin_addr.s_addr, hp->h_length);
    serveraddr.sin_port = htons(port);
    
    /* Establish a connection with the server */
    if (connect(clientfd, (SA *) &serveraddr, sizeof(serveraddr)) < 0)
        return -1;
    return clientfd;
}



/* --------------------------clienterror Function-------------------------*/ 
/*
 * clienterror - returns an error message to the client
 */

void clienterror(int fd, char *cause, char *errnum, 
		 char *shortmsg, char *longmsg) 
{
    char buf[MAXLINE], body[MAXBUF];

    /* Build the HTTP response body */
    sprintf(body, "<html><title>Tiny Error</title>");
    sprintf(body, "%s<body bgcolor=""ffffff"">\r\n", body);
    sprintf(body, "%s%s: %s\r\n", body, errnum, shortmsg);
    sprintf(body, "%s<p>%s: %s\r\n", body, longmsg, cause);
    sprintf(body, "%s<hr><em>The Tiny Web server</em>\r\n", body);

    /* Print the HTTP response */
    sprintf(buf, "HTTP/1.0 %s %s\r\n", errnum, shortmsg);
    Rio_writen_w(fd, buf, strlen(buf));
    sprintf(buf, "Content-type: text/html\r\n");
    Rio_writen_w(fd, buf, strlen(buf));
    sprintf(buf, "Content-length: %d\r\n\r\n", (int)strlen(body));
    Rio_writen_w(fd, buf, strlen(buf));
    Rio_writen_w(fd, body, strlen(body));
}


/* -------Wrapper for Rio_readnb_w that doesn't exit the program-------*/
ssize_t Rio_readnb_w(rio_t *rp, void *usrbuf, size_t n) {
    ssize_t rc;

    if ((rc = rio_readnb(rp, usrbuf, n)) < 0) 
     {
        printf("ERROR: Rio_readnb_w failed!\n");
        return rc;
     }
    return rc;
}


/* -------Wrapper for Rio_readlineb_w that doesn't exit the program-----*/
ssize_t Rio_readlineb_w(rio_t *rp, void *usrbuf, size_t maxlen) {
    ssize_t rc;

    if ((rc = rio_readlineb(rp, usrbuf, maxlen)) < 0) 
        {
        printf("ERROR: Rio_readlineb_w failed!\n");
        return rc;
        }
    return rc;
}



/* -------Wrapper for Rio_writen_w that doesn't exit the program-----*/
void Rio_writen_w(int fd, void *usrbuf, size_t n) {
    if (rio_writen(fd, usrbuf, n) != n) 
        {
        printf("ERROR: Rio_writen_w failed!\n");
        return;
        }
}
