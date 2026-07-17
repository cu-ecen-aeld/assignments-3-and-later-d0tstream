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

#define PORT "9000"
#define DATA_FILE "/var/tmp/aesdsocketdata"
#define CHUNK_SIZE 1024

int server_sockfd = -1;
int client_sockfd = -1;
volatile sig_atomic_t caught_sig = 0;

void signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        caught_sig = 1;
    }
}

int main(int argc, char *argv[]) {
    bool daemon_mode = false;
    struct addrinfo hints, *servinfo;
    struct sockaddr_storage client_addr;
    socklen_t addr_size;
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
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    if ((status = getaddrinfo(NULL, PORT, &hints, &servinfo)) != 0) {
        syslog(LOG_ERR, "getaddrinfo error: %s", gai_strerror(status));
        return -1;
    }

    server_sockfd = socket(servinfo->ai_family, servinfo->ai_socktype, servinfo->ai_protocol);
    if (server_sockfd == -1) {
        syslog(LOG_ERR, "socket creation failed");
        freeaddrinfo(servinfo);
        return -1;
    }

    if (setsockopt(server_sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) == -1) {
        syslog(LOG_ERR, "setsockopt failed");
        freeaddrinfo(servinfo);
        close(server_sockfd);
        return -1;
    }

    if (bind(server_sockfd, servinfo->ai_addr, servinfo->ai_addrlen) == -1) {
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
        syslog(LOG_ERR, "listen failed");
        close(server_sockfd);
        return -1;
    }

    while (!caught_sig) {
        addr_size = sizeof client_addr;
        client_sockfd = accept(server_sockfd, (struct sockaddr *)&client_addr, &addr_size);
        
        if (client_sockfd == -1) {
            if (errno == EINTR) break;
            syslog(LOG_ERR, "accept failed");
            continue;
        }

        char ipstr[INET6_ADDRSTRLEN];
        if (client_addr.ss_family == AF_INET) {
            struct sockaddr_in *s = (struct sockaddr_in *)&client_addr;
            inet_ntop(AF_INET, &s->sin_addr, ipstr, sizeof ipstr);
        } else {
            struct sockaddr_in6 *s = (struct sockaddr_in6 *)&client_addr;
            inet_ntop(AF_INET6, &s->sin6_addr, ipstr, sizeof ipstr);
        }
        
        syslog(LOG_INFO, "Accepted connection from %s", ipstr);

        char *rx_buffer = NULL;
        size_t rx_buffer_size = 0;
        char chunk[CHUNK_SIZE];
        ssize_t bytes_received;

        while ((bytes_received = recv(client_sockfd, chunk, sizeof(chunk), 0)) > 0) {
            rx_buffer = realloc(rx_buffer, rx_buffer_size + bytes_received + 1);
            if (rx_buffer == NULL) {
                syslog(LOG_ERR, "Memory allocation failed");
                break;
            }
            
            memcpy(rx_buffer + rx_buffer_size, chunk, bytes_received);
            rx_buffer_size += bytes_received;
            rx_buffer[rx_buffer_size] = '\0';

            if (strchr(rx_buffer, '\n') != NULL) {
                int file_fd = open(DATA_FILE, O_CREAT | O_APPEND | O_WRONLY, 0644);
                if (file_fd != -1) {
                    write(file_fd, rx_buffer, rx_buffer_size);
                    close(file_fd);
                } else {
                    syslog(LOG_ERR, "Failed to open data file for writing");
                }

                file_fd = open(DATA_FILE, O_RDONLY);
                if (file_fd != -1) {
                    char tx_buffer[CHUNK_SIZE];
                    ssize_t bytes_read;
                    while ((bytes_read = read(file_fd, tx_buffer, sizeof(tx_buffer))) > 0) {
                        send(client_sockfd, tx_buffer, bytes_read, 0);
                    }
                    close(file_fd);
                }

                free(rx_buffer);
                rx_buffer = NULL;
                rx_buffer_size = 0;
            }
        }

        if (rx_buffer != NULL) {
            free(rx_buffer);
        }

        close(client_sockfd);
        client_sockfd = -1;
        syslog(LOG_INFO, "Closed connection from %s", ipstr);
    }

    syslog(LOG_INFO, "Caught signal, exiting");
    
    if (server_sockfd != -1) {
        close(server_sockfd);
    }
    if (client_sockfd != -1) {
        close(client_sockfd);
    }
    
    remove(DATA_FILE);
    closelog();

    return 0;
}
