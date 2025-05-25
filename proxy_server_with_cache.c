#include "proxy_parse.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/wait.h>
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <time.h>

#define MAX_BYTES 4096    // max allowed size of request/response
#define MAX_CLIENTS 400     // max number of client requests served at a time
#define MAX_SIZE 200*(1<<20)     // size of the cache (200MB)
#define MAX_ELEMENT_SIZE 10*(1<<20)     // max size of an element in cache (10MB)

typedef struct cache_element cache_element;

struct cache_element {
    char* data;         // data stores response
    int len;          // length of data i.e.. sizeof(data)...
    char* url;        // url stores the request
    time_t lru_time_track;    // lru_time_track stores the latest time the element is accessed
    cache_element* next;    // pointer to next element
};

cache_element* find(char* url);
int add_cache_element(char* data,int size,char* url);
void remove_cache_element();

int port_number = 8080;                // Default Port
int proxy_socketId;                    // socket descriptor of proxy server
pthread_t tid[MAX_CLIENTS];         // array to store the thread ids of clients
sem_t seamaphore;                    // semaphore to limit max clients
pthread_mutex_t lock;               // lock is used for locking the cache

cache_element* head;                // pointer to the cache
int cache_size;             // cache_size denotes the current size of the cache

int sendErrorMessage(int socket, int status_code)
{
    char str[1024];
    char currentTime[50];
    time_t now = time(0);

    struct tm data = *gmtime(&now);
    strftime(currentTime,sizeof(currentTime),"%a, %d %b %Y %H:%M:%S %Z", &data);

    switch(status_code)
    {
        case 400: snprintf(str, sizeof(str), "HTTP/1.1 400 Bad Request\r\nContent-Length: 95\r\nConnection: close\r\nContent-Type: text/html\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>400 Bad Request</TITLE></HEAD>\n<BODY><H1>400 Bad Request</H1>\n</BODY></HTML>", currentTime);
                  printf("400 Bad Request\n");
                  send(socket, str, strlen(str), 0);
                  break;

        case 403: snprintf(str, sizeof(str), "HTTP/1.1 403 Forbidden\r\nContent-Length: 112\r\nContent-Type: text/html\r\nConnection: close\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>403 Forbidden</TITLE></HEAD>\n<BODY><H1>403 Forbidden</H1><br>Permission Denied\n</BODY></HTML>", currentTime);
                  printf("403 Forbidden\n");
                  send(socket, str, strlen(str), 0);
                  break;

        case 404: snprintf(str, sizeof(str), "HTTP/1.1 404 Not Found\r\nContent-Length: 91\r\nContent-Type: text/html\r\nConnection: close\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>404 Not Found</TITLE></HEAD>\n<BODY><H1>404 Not Found</H1>\n</BODY></HTML>", currentTime);
                  printf("404 Not Found\n");
                  send(socket, str, strlen(str), 0);
                  break;

        case 500: snprintf(str, sizeof(str), "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 115\r\nConnection: close\r\nContent-Type: text/html\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>500 Internal Server Error</TITLE></HEAD>\n<BODY><H1>500 Internal Server Error</H1>\n</BODY></HTML>", currentTime);
                  printf("500 Internal Server Error\n");
                  send(socket, str, strlen(str), 0);
                  break;

        case 501: snprintf(str, sizeof(str), "HTTP/1.1 501 Not Implemented\r\nContent-Length: 103\r\nConnection: close\r\nContent-Type: text/html\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>501 Not Implemented</TITLE></HEAD>\n<BODY><H1>501 Not Implemented</H1>\n</BODY></HTML>", currentTime);
                  printf("501 Not Implemented\n");
                  send(socket, str, strlen(str), 0);
                  break;

        case 505: snprintf(str, sizeof(str), "HTTP/1.1 505 HTTP Version Not Supported\r\nContent-Length: 125\r\nConnection: close\r\nContent-Type: text/html\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>505 HTTP Version Not Supported</TITLE></HEAD>\n<BODY><H1>505 HTTP Version Not Supported</H1>\n</BODY></HTML>", currentTime);
                  printf("505 HTTP Version Not Supported\n");
                  send(socket, str, strlen(str), 0);
                  break;

        default:  return -1;

    }
    return 1;
}

int connectRemoteServer(char* host_addr, int port_num)
{
    // Creating Socket for remote server ---------------------------

    int remoteSocket = socket(AF_INET, SOCK_STREAM, 0);

    if( remoteSocket < 0)
    {
        printf("Error in Creating Socket.\n");
        return -1;
    }
    
    // Get host by the name or ip address provided

    struct hostent *host = gethostbyname(host_addr);    
    if(host == NULL)
    {
        fprintf(stderr, "No such host exists.\n");    
        return -1;
    }

    // inserts ip address and port number of host in struct `server_addr`
    struct sockaddr_in server_addr;

    bzero((char*)&server_addr, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port_num);

    bcopy((char *)host->h_addr,(char *)&server_addr.sin_addr.s_addr,host->h_length);

    // Connect to Remote server ----------------------------------------------------

    if( connect(remoteSocket, (struct sockaddr*)&server_addr, (socklen_t)sizeof(server_addr)) < 0 )
    {
        fprintf(stderr, "Error in connecting !\n"); 
        return -1;
    }
    return remoteSocket;
}

int handle_request(int clientSocket, ParsedRequest *request, char *tempReq)
{
    char *buf = (char*)malloc(sizeof(char)*MAX_BYTES);
    if (!buf) {
        fprintf(stderr, "Memory allocation failed\n");
        return -1;
    }

    // Compose first request line with the original HTTP version (any version)
    snprintf(buf, MAX_BYTES, "GET %s %s\r\n", request->path, request->version);

    size_t len = strlen(buf);

    // Set Connection header to close for simplicity
    if (ParsedHeader_set(request, "Connection", "close") < 0){
        printf("Set header key not working\n");
    }

    if(ParsedHeader_get(request, "Host") == NULL)
    {
        if(ParsedHeader_set(request, "Host", request->host) < 0){
            printf("Set \"Host\" header key not working\n");
        }
    }

    if (ParsedRequest_unparse_headers(request, buf + len, (size_t)MAX_BYTES - len) < 0) {
        printf("unparse failed\n");
        // Continue anyway
    }

    int server_port = 80;                // Default Remote Server Port
    if(request->port != NULL)
        server_port = atoi(request->port);

    int remoteSocketID = connectRemoteServer(request->host, server_port);

    if(remoteSocketID < 0) {
        free(buf);
        return -1;
    }

    int bytes_send = send(remoteSocketID, buf, strlen(buf), 0);
    if(bytes_send < 0) {
        perror("Error sending request to remote server");
        free(buf);
        close(remoteSocketID);
        return -1;
    }

    bzero(buf, MAX_BYTES);

    bytes_send = recv(remoteSocketID, buf, MAX_BYTES-1, 0);
    char *temp_buffer = (char*)malloc(sizeof(char)*MAX_BYTES); // temp buffer for cache
    if (!temp_buffer) {
        fprintf(stderr, "Memory allocation failed\n");
        free(buf);
        close(remoteSocketID);
        return -1;
    }
    int temp_buffer_size = MAX_BYTES;
    int temp_buffer_index = 0;

    while(bytes_send > 0)
    {
        int bytes_sent_to_client = send(clientSocket, buf, bytes_send, 0);

        if(bytes_sent_to_client < 0) {
            perror("Error in sending data to client socket.");
            break;
        }

        for(int i=0; i < bytes_send; i++){
            if(temp_buffer_index >= temp_buffer_size) {
                temp_buffer_size += MAX_BYTES;
                temp_buffer = (char*)realloc(temp_buffer, temp_buffer_size);
                if(!temp_buffer) {
                    fprintf(stderr, "Realloc failed\n");
                    break;
                }
            }
            temp_buffer[temp_buffer_index++] = buf[i];
        }

        bzero(buf, MAX_BYTES);

        bytes_send = recv(remoteSocketID, buf, MAX_BYTES-1, 0);
    }
    temp_buffer[temp_buffer_index] = '\0';

    add_cache_element(temp_buffer, temp_buffer_index, tempReq);

    printf("Done fetching from remote and caching response\n");

    free(buf);
    free(temp_buffer);

    close(remoteSocketID);
    return 0;
}

// This function no longer used because we send the version from original request intact
// int checkHTTPversion(char *msg) {
//     // Removed, no longer used
//     return 1;
// }

void* thread_fn(void* socketNew)
{
    sem_wait(&seamaphore); 
    int p;
    sem_getvalue(&seamaphore, &p);
    printf("Semaphore value: %d\n", p);

    int* t = (int*)(socketNew);
    int socket = *t;
    free(t);

    char *buffer = (char*)calloc(MAX_BYTES, sizeof(char));
    bzero(buffer, MAX_BYTES);
    int bytes_recv_client = recv(socket, buffer, MAX_BYTES, 0);

    while (bytes_recv_client > 0)
    {
        int len = strlen(buffer);
        if (strstr(buffer, "\r\n\r\n") == NULL)
        {    
            bytes_recv_client = recv(socket, buffer + len, MAX_BYTES - len, 0);
        }
        else {
            break;
        }
    }

    if (bytes_recv_client <= 0) {
        free(buffer);
        close(socket);
        sem_post(&seamaphore);
        pthread_exit(NULL);
    }

    ParsedRequest* req = ParsedRequest_create();
    if (ParsedRequest_parse(req, buffer, strlen(buffer)) < 0) {
        sendErrorMessage(socket, 400);
        ParsedRequest_destroy(req);
        free(buffer);
        close(socket);
        sem_post(&seamaphore);
        pthread_exit(NULL);
    }

    // Construct a normalized cache key (raw full request string)
    char* tempReq = strdup(buffer);

    // CACHE CHECK
    pthread_mutex_lock(&lock);
    cache_element* found = find(tempReq);
    pthread_mutex_unlock(&lock);

    if(found != NULL)
    {
        printf("Cache hit: sending cached response for %s\n", tempReq);
        send(socket, found->data, found->len, 0);
        free(tempReq);
        ParsedRequest_destroy(req);
        free(buffer);
        close(socket);
        sem_post(&seamaphore);
        pthread_exit(NULL);
    }
    else
    {
        printf("Cache miss: forwarding request for %s\n", tempReq);
        if (handle_request(socket, req, tempReq) < 0) {
            sendErrorMessage(socket, 500);
        }
        free(tempReq);
    }

    ParsedRequest_destroy(req);
    free(buffer);
    close(socket);
    sem_post(&seamaphore);
    pthread_exit(NULL);
}

void print_cache_status() {
    printf("Cache Status: Total size = %d bytes\n", cache_size);
    cache_element* curr = head;
    int count = 0;
    while(curr) {
        printf("  Entry %d: URL='%s', Size=%d bytes, LastUsed=%ld\n",
            count++, curr->url, curr->len, (long)curr->lru_time_track);
        curr = curr->next;
    }
    printf("\n");
}

cache_element* find(char* url)
{
    cache_element* p = head;

    while(p)
    {
        if(strcmp(p->url, url) == 0)
        {
            p->lru_time_track = time(NULL);
            printf("Cache found for URL='%s', Size=%d bytes\n", url, p->len);
            print_cache_status();
            return p;
        }
        p = p->next;
    }
    printf("Cache not found for URL='%s'\n", url);
    print_cache_status();
    return NULL;
}

void remove_cache_element(){
    cache_element * p ;  	// Cache_element Pointer (Prev. Pointer)
	cache_element * q ;		// Cache_element Pointer (Next Pointer)
	cache_element * temp;	// Cache element to remove
    int temp_lock_val = pthread_mutex_lock(&lock);
	printf("Remove Cache Lock Acquired %d\n",temp_lock_val); 
	if( head != NULL) {
		for (q = head, p = head, temp =head ; q -> next != NULL; 
			q = q -> next) {
			if(( (q -> next) -> lru_time_track) < (temp -> lru_time_track)) {
				temp = q -> next;
				p = q;
			}
		}
		if(temp == head) { 
			head = head -> next;
		} else {
			p->next = temp->next;	
		}
		cache_size = cache_size - (temp -> len) - sizeof(cache_element) - strlen(temp -> url) - 1;
		printf("Removing cache entry: URL='%s', Size=%d bytes\n", temp->url, temp->len);
		free(temp->data);
		free(temp->url);
		free(temp);
        print_cache_status();
	} 
	temp_lock_val = pthread_mutex_unlock(&lock);
	printf("Remove Cache Lock Unlocked %d\n",temp_lock_val); 
}

int add_cache_element(char* data,int size,char* url){
    int temp_lock_val = pthread_mutex_lock(&lock);
	printf("Add Cache Lock Acquired %d\n", temp_lock_val);
    int element_size=size+1+strlen(url)+sizeof(cache_element);
    if(element_size>MAX_ELEMENT_SIZE){
        temp_lock_val = pthread_mutex_unlock(&lock);
		printf("Add Cache Lock Unlocked %d\n", temp_lock_val);
        return 0;
    }
    else
    {   
        while(cache_size+element_size>MAX_SIZE){
            remove_cache_element();
        }
        cache_element* element = (cache_element*) malloc(sizeof(cache_element));
        element->data= (char*)malloc(size+1);
		memcpy(element->data,data,size);
        element->data[size] = '\0'; // null terminate
        element->url = strdup(url);
		element->lru_time_track=time(NULL);
        element->next=head; 
        element->len=size;
        head=element;
        cache_size+=element_size;
        printf("Added cache entry: URL='%s', Size=%d bytes\n", url, size);
        print_cache_status();
        temp_lock_val = pthread_mutex_unlock(&lock);
		printf("Add Cache Lock Unlocked %d\n", temp_lock_val);
        return 1;
    }
    return 0;
}

int main(int argc, char* argv[])
{
    // Init the cache and other variables
    head = NULL;
    cache_size = 0;

    pthread_mutex_init(&lock, NULL);
    sem_init(&seamaphore, 0, MAX_CLIENTS);

    if(argc == 2)
        port_number = atoi(argv[1]);

    printf("Starting proxy on port %d\n", port_number);

    proxy_socketId = socket(AF_INET, SOCK_STREAM, 0);

    if(proxy_socketId < 0)
    {
        perror("Failed to create socket");
        return 1;
    }

    struct sockaddr_in server_addr;

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port_number);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if(bind(proxy_socketId, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0)
    {
        perror("Bind failed");
        return 1;
    }

    if(listen(proxy_socketId, MAX_CLIENTS) < 0)
    {
        perror("Listen failed");
        return 1;
    }

    printf("Proxy started and listening...\n");

    int count = 0;
    while(1)
    {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int *client_socket =(int*)malloc(sizeof(int));
        *client_socket = accept(proxy_socketId, (struct sockaddr*)&client_addr, &client_len);
        if(*client_socket < 0)
        {
            perror("Accept failed");
            free(client_socket);
            continue;
        }
        printf("Accepted connection %d\n", count++);

        pthread_create(&tid[count%MAX_CLIENTS], NULL, thread_fn, client_socket);
    }

    pthread_mutex_destroy(&lock);
    sem_destroy(&seamaphore);
    close(proxy_socketId);

    return 0;
}
