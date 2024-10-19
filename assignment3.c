#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>      
#include <errno.h>
#include <fcntl.h>       
#include <pthread.h>
#include <netinet/in.h>  
#include <arpa/inet.h>   
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/time.h>    
#include <signal.h>
#include <ctype.h>       // For tolower, isprint, isspace
#include <bits/getopt_core.h>

#define BUFFER_SIZE 1024
#define ANALYSIS_THREAD_COUNT 2

typedef struct node {
    char *line;
    int has_ptrn; // has pattern
    struct node *next; 
    struct node *next_book; 
    struct node *next_freq_search; // next frequency search 
} node_t;

typedef struct book {
    int con_order; //connection order
    char *title;
    node_t *head;
    node_t *tail;
    int search_count;
    struct book *next; 
} book_t;

typedef struct {
    int socket_fd;
    int con_order;
} client_info_t;

node_t *same_head = NULL;
node_t *same_tail = NULL;
pthread_mutex_t shared_list_mutex = PTHREAD_MUTEX_INITIALIZER;

book_t *books_list = NULL; // head of  books list
pthread_mutex_t books_list_mutex = PTHREAD_MUTEX_INITIALIZER;

char *search_ptrn = NULL;

void *client_handler(void *arg);
void *analysis_thread_func(void *arg);
int comp_books(const void *a, const void *b);
void remove_bom(char *str);
char *str_to_lower(const char *str);
void clean_line(char *line);

int main(int argc, char *argv[]) {
    int listen_port = 0;
    int opt;
    while((opt = getopt(argc, argv, "l:p:")) != -1) {
        switch (opt) {
            case 'l':
                listen_port = atoi(optarg);
                break;
            case 'p':
                search_ptrn = optarg;
                break;
            default:
                fprintf(stderr, "Usage: %s -l listen_port -p search_ptrn\n", argv[0]);
                exit(EXIT_FAILURE);
        }
    }

    if (listen_port == 0 || search_ptrn == NULL) {
        fprintf(stderr, "Usage: %s -l listen_port -p search_ptrn\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    int server_fd;
    struct sockaddr_in address;
    int addrlen = sizeof(address);

    //make the server socket
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    int optval = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval))) {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }

    // bind to port to the arg that the user gives hwen they run the program 
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY; // accept any incoming interface
    address.sin_port = htons(listen_port);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address))<0) {
        perror("bind failed");
        exit(EXIT_FAILURE);
    }

    //start listening
    if (listen(server_fd, 10) < 0) {
        perror("listen");
        exit(EXIT_FAILURE);
    }

    printf("Server listening on port: %d\n", listen_port);

    pthread_t analysis_threads[ANALYSIS_THREAD_COUNT];
    for (int i = 0; i < ANALYSIS_THREAD_COUNT; i++) {
        if (pthread_create(&analysis_threads[i], NULL, analysis_thread_func, NULL) != 0) {
            perror("pthread_create analysis_thread");
            exit(EXIT_FAILURE);
        }
    }

    int connection_counter = 0;

    while (1) {
        int new_socket;
        struct sockaddr_in client_address;
        socklen_t client_addrlen = sizeof(client_address);

        if ((new_socket = accept(server_fd, (struct sockaddr *)&client_address, &client_addrlen)) < 0) {
            perror("accept");
            continue;
        }

        connection_counter++;

        client_info_t *client_info = malloc(sizeof(client_info_t));
        client_info->socket_fd = new_socket;
        client_info->con_order = connection_counter;

        // new thread
        pthread_t thread_id;
        if (pthread_create(&thread_id, NULL, client_handler, (void *)client_info) != 0) {
            perror("pthread_create");
            close(new_socket);
            free(client_info);
            continue;
        }

        pthread_detach(thread_id);
    }

    close(server_fd);
    return 0;
}

void remove_bom(char *str) {
    unsigned char *ustr = (unsigned char *)str;
    if(ustr[0] == 0xEF && ustr[1] == 0xBB && ustr[2] == 0xBF) {
        // byte order magic over here. it fixed my case issue that i was running into. 
        memmove(str, str + 3, strlen(str + 3) + 1);
    }
}

char *str_to_lower(const char *str) {
    char *lower_str = strdup(str);
    if(lower_str == NULL) {
        perror("strdup");
        exit(EXIT_FAILURE);
    }
    for(char *p = lower_str; *p; ++p) {
        *p = tolower(*p);
    }
    return lower_str;
}

void clean_line(char *line) {
    char *src = line, *dst = line;
    while(*src) {
        if(isprint((unsigned char)*src) || isspace((unsigned char)*src)) {
            *dst++ = *src;
        }
        src++;
    }
    *dst = '\0';
}

void *client_handler(void *arg) {
    client_info_t *client_info = (client_info_t *)arg;
    int socket_fd = client_info->socket_fd;
    int con_order = client_info->con_order;
    free(client_info);

    // Set socket to non-blocking mode
    int flags = fcntl(socket_fd, F_GETFL, 0);
    fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK);

    char recv_buffer[BUFFER_SIZE];
    char line_buffer[BUFFER_SIZE * 10]; // Increase buffer size if needed
    int line_buffer_len = 0;

    memset(recv_buffer, 0, BUFFER_SIZE);
    memset(line_buffer, 0, sizeof(line_buffer));

    int bytes_read;
    char *book_title = NULL;
    int first_line = 1;

    book_t *current_book = NULL;

    // Read data from the socket
    while (1) {
        bytes_read = recv(socket_fd, recv_buffer, BUFFER_SIZE - 1, 0);
        if(bytes_read > 0) {
            int i;
            for(i = 0; i < bytes_read; i++) {
                char c = recv_buffer[i];

                if(c == '\n' || c == '\r') {
                    if (line_buffer_len == 0) {
                        continue; // 
                    }

                    line_buffer[line_buffer_len] = '\0';

                    clean_line(line_buffer);

                    if(first_line) {
                        remove_bom(line_buffer);
                    }

                    // new node
                    node_t *new_node = malloc(sizeof(node_t));
                    new_node->line = strdup(line_buffer);

                    // pattern matching (MIGHT BE WRONG. NOT SURE WHAT KIND OF PATTERN MATCHING IT WANTS!)
                    char *lower_line = str_to_lower(line_buffer);
                    char *lower_pattern = str_to_lower(search_ptrn);
                    new_node->has_ptrn = (strstr(lower_line, lower_pattern) != NULL);
                    free(lower_line);
                    free(lower_pattern);

                    new_node->next = NULL;
                    new_node->next_book = NULL;
                    new_node->next_freq_search = NULL;

                    pthread_mutex_lock(&shared_list_mutex);

                    if(same_tail == NULL) {
                        same_head = same_tail = new_node;
                    } else {
                        same_tail->next = new_node;
                        same_tail = new_node;
                    }

                    pthread_mutex_unlock(&shared_list_mutex);

                    if(first_line) {
                        book_title = strdup(line_buffer);
                        first_line = 0;

                        // a new book entry. addd it to the list
                        book_t *new_book = malloc(sizeof(book_t));
                        new_book->con_order = con_order;
                        new_book->title = strdup(book_title);
                        new_book->head = new_node;
                        new_book->tail = new_node;
                        new_book->search_count = 0;
                        new_book->next = NULL;

                        pthread_mutex_lock(&books_list_mutex);
                        new_book->next = books_list;
                        books_list = new_book;
                        pthread_mutex_unlock(&books_list_mutex);

                        current_book = new_book;

                    } else {
                        // Link the new node to the current book's linked list
                        pthread_mutex_lock(&books_list_mutex);
                        if (current_book->tail != NULL) {
                            current_book->tail->next_book = new_node;
                            current_book->tail = new_node;
                        } else {
                            fprintf(stderr, "Error: current_book->tail is NULL\n");
                        }
                        pthread_mutex_unlock(&books_list_mutex);
                    }

                    // debug prints
                    //printf("Line proccessed: %s\n", line_buffer);
                    //printf("Pattern '%s' found: %s\n", search_ptrn, new_node->has_ptrn ? "Yes" : "No");
                    //printf("Added node: %s\n", line_buffer);

                    line_buffer_len = 0;
                    memset(line_buffer, 0, sizeof(line_buffer));
                } else {
                    if(line_buffer_len < sizeof(line_buffer) - 1) {
                        line_buffer[line_buffer_len++] = c;
                    } else {
                        // line 2 long
                        fprintf(stderr, "Line too long, discarding\n");
                        line_buffer_len = 0;
                    }
                }
            }

        } else if (bytes_read == 0) {
            printf("Connection closed %d\n", con_order);
            break;
        } else {
            if (errno == EWOULDBLOCK || errno == EAGAIN) {
                usleep(100000); // 100 ms sleep
                continue;
            } else {
                perror("recv");
                break;
            }
        }
    }

    // connection is closed. output that book_xx.txt file. ORDERED
    char filename[256];
    snprintf(filename, sizeof(filename), "book_%02d.txt", con_order);

    FILE *fp = fopen(filename, "w");
    if(fp == NULL) {
        perror("fopen");
    } else {
        pthread_mutex_lock(&books_list_mutex);
        book_t *book = books_list;
        while (book != NULL) {
            if (book->con_order == con_order) {
                break;
            }
            book = book->next;
        }
        pthread_mutex_unlock(&books_list_mutex);

        if(book != NULL) {
            node_t *current = book->head;
            while (current != NULL) {
                fprintf(fp, "%s\n", current->line);
                current = current->next_book;
            }
        }
        fclose(fp);
    }

    close(socket_fd);
    return NULL;
}


void *analysis_thread_func(void *arg) {
    while (1) {
        sleep(5); ///////// can change.........

        // Lock!!!
        static pthread_mutex_t output_mutex = PTHREAD_MUTEX_INITIALIZER;
        if(pthread_mutex_trylock(&output_mutex) == 0) {
            pthread_mutex_lock(&books_list_mutex);
            book_t *book = books_list;
            while (book != NULL) {
                int count = 0;
                node_t *node = book->head;
                while (node != NULL) {
                    if (node->has_ptrn) {
                        count++;
                    }
                    node = node->next_book;
                }
                book->search_count = count;
                book = book->next;
            }
            pthread_mutex_unlock(&books_list_mutex);

            // count books
            int book_count = 0;
            pthread_mutex_lock(&books_list_mutex);
            book = books_list;
            while (book != NULL) {
                book_count++;
                book = book->next;
            }

            book_t **book_array = malloc(book_count * sizeof(book_t *));
            int index = 0;
            book = books_list;
            while (book != NULL) {
                book_array[index++] = book;
                book = book->next;
            }
            pthread_mutex_unlock(&books_list_mutex);

            // sort array
            qsort(book_array, book_count, sizeof(book_t *), comp_books);

            // print sorted books
            printf("\nBooks sorted by occurrences of '%s':\n", search_ptrn);
            for(int i = 0; i < book_count; i++) {
                printf("Book %02d: '%s' - %d occurrences\n", book_array[i]->con_order, book_array[i]->title, book_array[i]->search_count);
            }
            printf("\n");
            free(book_array);
            pthread_mutex_unlock(&output_mutex);
        } else {
            continue;
        }
    }
    return NULL;
}

int comp_books(const void *a, const void *b) {
    book_t *book_a = *(book_t **)a;
    book_t *book_b = *(book_t **)b;

    return (book_b->search_count - book_a->search_count);
}
