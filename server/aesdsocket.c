#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <syslog.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <sys/queue.h>
#include <time.h>

#define PORT "9000"
#define DATA_FILE "/var/tmp/aesdsocketdata"
#define CHUNK_SIZE 1024

int server_sockfd = -1;
volatile sig_atomic_t caught_sig = 0;

pthread_mutex_t data_mutex = PTHREAD_MUTEX_INITIALIZER;

struct thread_data {
    pthread_t thread_id;
    int client_sockfd;
    struct sockaddr_storage client_addr;
    bool is_complete;
    SLIST_ENTRY(thread_data) entries;
};

SLIST_HEAD(thread_list, thread_data);

void signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        caught_sig = 1;
    }
}

void *handle_connection(void *arg) {
    struct thread_data *tdata = (struct thread_data *)arg;
    char ipstr[INET6_ADDRSTRLEN];
    
    if (tdata->client_addr.ss_family == AF_INET) {
        struct sockaddr_in *s = (struct sockaddr_in *)&tdata->client_addr;
        inet_ntop(AF_INET, &s->sin_addr, ipstr, sizeof ipstr);
    } else {
        struct sockaddr_in6 *s = (struct sockaddr_in6 *)&tdata->client_addr;
        inet_ntop(AF_INET6, &s->sin6_addr, ipstr, sizeof ipstr);
    }
    
    syslog(LOG_INFO, "Accepted connection from %s", ipstr);
    
    char *rx_buffer = NULL;
    size_t rx_buffer_size = 0;
    char chunk[CHUNK_SIZE];
    ssize_t bytes_received;
    
    while ((bytes_received = recv(tdata->client_sockfd, chunk, sizeof(chunk), 0)) > 0) {
        char *new_buffer = realloc(rx_buffer, rx_buffer_size + bytes_received + 1);
        if (new_buffer == NULL) {
            syslog(LOG_ERR, "Memory allocation failed");
            break;
        }
        rx_buffer = new_buffer;
        
        memcpy(rx_buffer + rx_buffer_size, chunk, bytes_received);
        rx_buffer_size += bytes_received;
        rx_buffer[rx_buffer_size] = '\0';
        
        if (strchr(rx_buffer, '\n') != NULL) {
            pthread_mutex_lock(&data_mutex);
            
            int file_fd = open(DATA_FILE, O_CREAT | O_APPEND | O_WRONLY, 0644);
            if (file_fd != -1) {
                write(file_fd, rx_buffer, rx_buffer_size);
                close(file_fd);
            }
            
            file_fd = open(DATA_FILE, O_RDONLY);
            if (file_fd != -1) {
                char tx_buffer[CHUNK_SIZE];
                ssize_t bytes_read;
                while ((bytes_read = read(file_fd, tx_buffer, sizeof(tx_buffer))) > 0) {
                    send(tdata->client_sockfd, tx_buffer, bytes_read, 0);
                }
                close(file_fd);
            }
            
            pthread_mutex_unlock(&data_mutex);
            
            free(rx_buffer);
            rx_buffer = NULL;
            rx_buffer_size = 0;
        }
    }
    
    if (rx_buffer != NULL) {
        free(rx_buffer);
    }
    
    close(tdata->client_sockfd);
    syslog(LOG_INFO, "Closed connection from %s", ipstr);
    
    tdata->is_complete = true;
    return NULL;
}

void *timestamp_handler(void *arg) {
    int ticks = 0;
    while (!caught_sig) {
        usleep(100000); // Sleep 100ms
        ticks++;
        
        if (ticks >= 100) { // 10 seconds total
            ticks = 0;
            time_t t = time(NULL);
            struct tm *tmp = localtime(&t);
            char time_str[100];
            char outstr[200];
            
            strftime(time_str, sizeof(time_str), "%a, %d %b %Y %T %z", tmp);
            snprintf(outstr, sizeof(outstr), "timestamp:%s\n", time_str);
            
            pthread_mutex_lock(&data_mutex);
            int file_fd = open(DATA_FILE, O_CREAT | O_APPEND | O_WRONLY, 0644);
            if (file_fd != -1) {
                write(file_fd, outstr, strlen(outstr));
                close(file_fd);
            }
            pthread_mutex_unlock(&data_mutex);
        }
    }
    return NULL;
}

int main(int argc, char *argv[]) {
    bool daemon_mode = false;
    struct addrinfo hints, *servinfo;
    int status;
    int yes = 1;
    
    if (argc == 2 && strcmp(argv[1], "-d") == 0) {
        daemon_mode = true;
    }
    
    openlog("aesdsocket", LOG_PID, LOG_USER);
    
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    
    if ((status = getaddrinfo(NULL, PORT, &hints, &servinfo)) != 0) {
        fprintf(stderr, "getaddrinfo error: %s\n", gai_strerror(status));
        syslog(LOG_ERR, "getaddrinfo error: %s", gai_strerror(status));
        return -1;
    }
    
    server_sockfd = socket(servinfo->ai_family, servinfo->ai_socktype, servinfo->ai_protocol);
    if (server_sockfd == -1) {
        fprintf(stderr, "socket creation failed: %s\n", strerror(errno));
        syslog(LOG_ERR, "socket creation failed");
        freeaddrinfo(servinfo);
        return -1;
    }
    
    if (setsockopt(server_sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) == -1) {
        fprintf(stderr, "setsockopt failed: %s\n", strerror(errno));
        syslog(LOG_ERR, "setsockopt failed");
        freeaddrinfo(servinfo);
        close(server_sockfd);
        return -1;
    }
    
    if (bind(server_sockfd, servinfo->ai_addr, servinfo->ai_addrlen) == -1) {
        fprintf(stderr, "bind failed: %s\n", strerror(errno));
        syslog(LOG_ERR, "bind failed");
        freeaddrinfo(servinfo);
        close(server_sockfd);
        return -1;
    }
    freeaddrinfo(servinfo);
    
    if (daemon_mode) {
        pid_t pid = fork();
        if (pid < 0) {
            syslog(LOG_ERR, "Fork failed");
            close(server_sockfd);
            return -1;
        }
        if (pid > 0) {
            exit(0);
        }
        setsid();
        chdir("/");
        int devnull_fd = open("/dev/null", O_RDWR);
        if (devnull_fd != -1) {
            dup2(devnull_fd, STDIN_FILENO);
            dup2(devnull_fd, STDOUT_FILENO);
            dup2(devnull_fd, STDERR_FILENO);
            close(devnull_fd);
        }
    }
    
    if (listen(server_sockfd, 10) == -1) {
        fprintf(stderr, "listen failed: %s\n", strerror(errno));
        syslog(LOG_ERR, "listen failed");
        close(server_sockfd);
        return -1;
    }

    struct thread_list head;
    SLIST_INIT(&head);
    
    pthread_t timestamp_thread;
    pthread_create(&timestamp_thread, NULL, timestamp_handler, NULL);
    
    while (!caught_sig) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(server_sockfd, &readfds);
        
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 100000; // 100ms timeout
        
        int ret = select(server_sockfd + 1, &readfds, NULL, NULL, &tv);
        
        if (ret == -1) {
            if (errno == EINTR) break; 
            syslog(LOG_ERR, "select failed");
            break;
        } else if (ret > 0) {
            struct sockaddr_storage client_addr;
            socklen_t addr_size = sizeof client_addr;
            int client_sockfd = accept(server_sockfd, (struct sockaddr *)&client_addr, &addr_size);
            
            if (client_sockfd == -1) {
                if (errno != EINTR) {
                    syslog(LOG_ERR, "accept failed");
                }
            } else {
                struct thread_data *new_thread = malloc(sizeof(struct thread_data));
                if (!new_thread) {
                    syslog(LOG_ERR, "Memory allocation failed for thread_data");
                    close(client_sockfd);
                } else {
                    new_thread->client_sockfd = client_sockfd;
                    new_thread->client_addr = client_addr;
                    new_thread->is_complete = false;
                    
                    if (pthread_create(&new_thread->thread_id, NULL, handle_connection, new_thread) == 0) {
                        SLIST_INSERT_HEAD(&head, new_thread, entries);
                    } else {
                        syslog(LOG_ERR, "Thread creation failed");
                        free(new_thread);
                        close(client_sockfd);
                    }
                }
            }
        }
        
        // Safely clean up completed threads every loop iteration
        struct thread_data *tdata = SLIST_FIRST(&head);
        while (tdata != NULL) {
            struct thread_data *next = SLIST_NEXT(tdata, entries);
            if (tdata->is_complete) {
                pthread_join(tdata->thread_id, NULL);
                SLIST_REMOVE(&head, tdata, thread_data, entries);
                free(tdata);
            }
            tdata = next;
        }
    }
    
    pthread_join(timestamp_thread, NULL);
    
    struct thread_data *tdata = SLIST_FIRST(&head);
    while (tdata != NULL) {
        struct thread_data *next = SLIST_NEXT(tdata, entries);
        shutdown(tdata->client_sockfd, SHUT_RDWR);
        pthread_join(tdata->thread_id, NULL);
        SLIST_REMOVE(&head, tdata, thread_data, entries);
        free(tdata);
        tdata = next;
    }
    
    pthread_mutex_destroy(&data_mutex);
    
    syslog(LOG_INFO, "Caught signal, exiting");
    
    if (server_sockfd != -1) {
        close(server_sockfd);
    }
    
    remove(DATA_FILE);
    closelog();
    
    return 0;
}
