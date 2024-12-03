// libraries required for files
#include <stdio.h>     
#include <stdlib.h>     
#include <string.h>    
#include <unistd.h>    
#include <fcntl.h>     
#include <sys/types.h>  
#include <sys/socket.h> 
#include <netinet/in.h> 
#include <sys/stat.h>  
#include <signal.h>    
#include <errno.h>     

// define size for the operations
#define BUFFER_SIZE 4096 

// cleans up termination for multiple processess
void handle_child_termination(int signal) {
    while (waitpid(-1, NULL, WNOHANG) > 0); 
}

// send http response to client
void send_response(int client_socket, const char *status_code, const char *content_type, const char *response_body) {
    // utilize initialized buffer size to hold
    char response[BUFFER_SIZE];
    // if user asks for len, calculate
    int body_length = response_body ? strlen(response_body) : 0; 

    // http response to display to user
    snprintf(response, sizeof(response),
             "HTTP/1.0 %s\r\n"          
             "Content-Type: %s\r\n"      
             "Content-Length: %d\r\n"    
             "\r\n%s",            
             status_code, content_type, body_length, response_body ? response_body : "");

    // send response to client w/ send function
    send(client_socket, response, strlen(response), 0);
}

// function sends request for content
void handle_static_file_request(int client_socket, const char *file_path, int include_file_content) {
    // strcuture defined to hold data
    struct stat file_stats; 
    // check if file exists
    if (stat(file_path, &file_stats) != 0) {
        // return error if no file
        send_response(client_socket, "File Not Found", "text/html", "<html><body>404 Not Found</body></html>");
        return;
    }
    // check if file is readable
    if (!S_ISREG(file_stats.st_mode) || access(file_path, R_OK) != 0) {
        // return file is not accessiblee
        send_response(client_socket, "File Not Accessible", "text/html", "<html><body>File Not Accessible</body></html>");
        return;
    }
    if (include_file_content) {
        // if get request, handle and open file in read only mode
        int file_descriptor = open(file_path, O_RDONLY);
        if (file_descriptor < 0) {
            // if script is unable to open file
            send_response(client_socket, "Server Error", "text/html", "<html><body>Server Error</body></html>");
            return;
        }
        // send https display
        char header[BUFFER_SIZE];
        snprintf(header, sizeof(header),
                 "HTTP/1.0 200 OK\r\n"       
                 "Content-Type: text/html\r\n" 
                 "Content-Length: %lld\r\n" 
                 "\r\n",
                 (long long)file_stats.st_size);
        send(client_socket, header, strlen(header), 0);

        // send file content utilizing buffer to read content
        char buffer[BUFFER_SIZE];
        ssize_t bytes_read;
        while ((bytes_read = read(file_descriptor, buffer, sizeof(buffer))) > 0) {
            // send part of code to client
            send(client_socket, buffer, bytes_read, 0); 
        }
        // close file 
        close(file_descriptor); 
    } else {
        // handles HEAD, sends headers
        char header[BUFFER_SIZE];
        snprintf(header, sizeof(header),
                 "HTTP/1.0 200 OK\r\n"
                 "Content-Type: text/html\r\n"
                 "Content-Length: %lld\r\n"
                 "\r\n",
                 (long long)file_stats.st_size);
        send(client_socket, header, strlen(header), 0);
    }
}

// function for cgi prompts
void handle_cgi_request(int client_socket, const char *script_path, const char *arguments) {
    // check if script is in correct directory
    if (strncmp(script_path, "/cgi-like/", 10) != 0) {
        send_response(client_socket, "Unable to Process Request", "text/html", "<html><body>Unable to Process Request</body></html>");
        return;
    }
    // construct path with strdup removing leading /
    char *full_script_path = strdup(script_path + 1);
    if (!full_script_path) {
        // return server error
        send_response(client_socket, "Server Error", "text/html", "<html><body>Server Error</body></html>");
        return;
    }
    // store script output with temp using buffer
    char temporary_file_path[BUFFER_SIZE];
    snprintf(temporary_file_path, sizeof(temporary_file_path), "/tmp/cgi_output_%d", getpid());
    // use fork for child process for execution
    pid_t process_id = fork();
    if (process_id < 0) {
        free(full_script_path);
        send_response(client_socket, "Server Error", "text/html", "<html><body>Server Error</body></html>");
        return;
    } else if (process_id == 0) {
        // execute script via child processsee
        int temp_file_descriptor = open(temporary_file_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        // exit if error
        if (temp_file_descriptor < 0) exit(1); 
        // change output to temporary file defined to store
        dup2(temp_file_descriptor, STDOUT_FILENO); 
        close(temp_file_descriptor);
        // execute script taking in args
        char *script_args[] = {full_script_path, arguments ? (char *)arguments : NULL, NULL};
        // use execvp for script execution
        execvp(full_script_path, script_args); 
        exit(1);
    }
    // parent process uses waitpd for child process
    waitpid(process_id, NULL, 0);
    // open file in read to check the temporary store in file
    FILE *temp_file = fopen(temporary_file_path, "r");
    if (!temp_file) {
        free(full_script_path);
        // if unable to open or error
        send_response(client_socket, "Server Error", "text/html", "<html><body>Server Error</body></html>");
        return;
    }
    // calculate file size
    fseek(temp_file, 0, SEEK_END); 
    long output_size = ftell(temp_file);
    // set file pointer to start
    rewind(temp_file); 
    // allocate using malloc fofr output
    char *output_content = malloc(output_size + 1);
    // read file content
    fread(output_content, 1, output_size, temp_file); 
    fclose(temp_file);
    // remove temporary storage file
    unlink(temporary_file_path);
    // null terminate output
    output_content[output_size] = '\0'; 
    // send response - the script output
    send_response(client_socket, "200 OK", "text/html", output_content);
    // free memory and script path
    free(output_content);
    free(full_script_path); 
}

// function to process http requests
void process_http_request(int client_socket) {
    // use initialized buffer to hold request
    char request_buffer[BUFFER_SIZE]; 
    ssize_t bytes_received = recv(client_socket, request_buffer, sizeof(request_buffer) - 1, 0);
    if (bytes_received <= 0) {
        // close socket after data
        close(client_socket);
        return;
    }
    // null terminate request
    request_buffer[bytes_received] = '\0';
    char method[BUFFER_SIZE], resource[BUFFER_SIZE], http_version[BUFFER_SIZE];
    // sccanf to parse the request
    sscanf(request_buffer, "%s %s %s", method, resource, http_version); 
    // based on the http method, route
    if (strstr(resource, "..")) {
        // based on specifications reject traversal
        send_response(client_socket, "Unable to Process", "text/html", "<html><body>Unable to Process</body></html>");
    } else if (strcmp(method, "HEAD") == 0) {
        // Handle head req
        handle_static_file_request(client_socket, resource + 1, 0); 
    } else if (strcmp(method, "GET") == 0) {
        if (strncmp(resource, "/cgi-like/", 10) == 0) {
            // handles cgi args
            char *arguments = strchr(resource, '?');
            if (arguments) {
                // split 
                *arguments = '\0';
                arguments++;
            }
            handle_cgi_request(client_socket, resource, arguments);
        } else {
            // handles get requests for static files
            handle_static_file_request(client_socket, resource + 1, 1); 
        }
    } else {
        // if method is unrecognized
        send_response(client_socket, "Not Recognized", "text/html", "<html><body>Not Recognized</body></html>");
    }
    // close client connection
    close(client_socket); 
}

// starts sserver and checks connection
int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]); 
        exit(EXIT_FAILURE);
    }
    // converts string to int (port)
    int port = atoi(argv[1]); 
    // crearte socket
    int server_socket = socket(AF_INET, SOCK_STREAM, 0); 
    if (server_socket < 0) {
        // error if unable to create
        perror("Not able to Create");
        // exit
        exit(EXIT_FAILURE);
    }
    // struct
    struct sockaddr_in server_address = {
        .sin_family = AF_INET,     
        .sin_port = htons(port),      
        .sin_addr.s_addr = INADDR_ANY
    };
    // set socket to port set by user
    if (bind(server_socket, (struct sockaddr *)&server_address, sizeof(server_address)) < 0) {
        perror("Error with Socket");
        close(server_socket);
        // exit
        exit(EXIT_FAILURE);
    }
    // check for new req/connections
    if (listen(server_socket, 10) < 0) {
        perror("Error checking for connections"); 
        close(server_socket);
        // exit
        exit(EXIT_FAILURE);
    }
    // clean up processes for termination after execution
    signal(SIGCHLD, handle_child_termination);

    printf("HTTP server: %d \n", port);

    while (1) {
        // get connected to client
        int client_socket = accept(server_socket, NULL, NULL);
        if (client_socket < 0) {
            // ignore errors
            if (errno == EINTR) continue;
            perror("Error");
            break;
        }
        // fork child process for request
        pid_t process_id = fork();
        if (process_id < 0) {
            perror("Error Forking");
            close(client_socket);
        } else if (process_id == 0) {
            // child process set to handle new request
            // close server socket
            close(server_socket); 
            process_http_request(client_socket);
            // exit child process
            exit(0); 
        }
        close(client_socket); 
    }
    // close server
    close(server_socket); // Close the server socket when shutting down
    return 0;
}
