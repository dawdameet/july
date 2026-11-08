#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netinet/in.h>
#include "task.h"

#define PORT 8080
#define BUFFER_SIZE 1024

int extract_body(char *buffer, char **body, size_t *body_len)
{
    // we take the raw buffer
    // we are assuming VALID request
    /*
      "POST /task HTTP/1.1\r\n"
      "Host: localhost:8080\r\n"
      "Content-Type: application/octet-stream\r\n"
      "Content-Length: 24\r\n"
      "\r\n"
      "<binary task payload>"
     */
    // first 4 bytes are POST (guarenteed)
    /* while (strcmp(buffer, ' ')!= 0){buffer++}; //we encountered first space
     buffer++;// consume space
     while (strcmp(buffer, ' ')!= 0){buffer++}; //we encountered second space
     while (strcmp(buffer, ' ')!= 0){buffer++}; //we encountered third space (after host)
     fuck it */

    char *header_end = strstr(buffer, "\r\n\r\n");
    // got there bitchh
    if (!header_end)
    {
        return -1;
    }
    *body = header_end + 4; // consume \r\n\r\n
    char *cl_header = strstr(buffer, "Content-Length");
    if (cl_header && cl_header < header_end)
    {
        *body_len = (size_t)strtoull(cl_header + 15, NULL, 10);
    }
    else
    {
        *body_len = 0;
    }
    return 0;
}
int parse_task(const char *body, size_t body_len, Task *task)
{
    if (body_len < sizeof(TaskHeader))
    {
        return -1;
    }
    memcpy(&task->header, body, sizeof(TaskHeader));
    uint32_t id_len = task->header.taskIdLen;
    uint32_t command_len = task->header.taskCommandLen;
    size_t payload_len = (size_t)id_len + (size_t)command_len;
    if (body_len - sizeof(TaskHeader) < payload_len)
    {
        return -1;
    }
    const char *curr = body + sizeof(TaskHeader);
    task->taskId = malloc(id_len + 1);
    task->taskCommand = malloc(command_len + 1);
    if (!task->taskId || !task->taskCommand)
    {
        free(task->taskId);
        free(task->taskCommand);
        return -1;
    }
    memcpy(task->taskId, curr, id_len);
    task->taskId[id_len] = '\0';
    curr += id_len;
    memcpy(task->taskCommand, curr, command_len);
    task->taskCommand[command_len] = '\0';
    return 0;
}

int main(void)
{
    int server_fd, client_fd;
    struct sockaddr_in address;
    socklen_t addr_len = sizeof(address);
    char buffer[BUFFER_SIZE] = {0};

    const char *response_200 =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 13\r\n"
        "\r\n"
        "Hello, World!";

    const char *response_404 =
        "HTTP/1.1 404 Not Found\r\n"
        "Content-Length: 0\r\n"
        "\r\n";

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0)
    {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);
    address.sin_family = AF_INET;

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0)
    {
        perror("bind failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }
    if (listen(server_fd, 5) < 0)
    {
        perror("listen failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    printf("Server running on http://localhost:%d\n", PORT);
    while (1)
    {
        client_fd = accept(server_fd, (struct sockaddr *)&address, &addr_len);
        if (client_fd < 0)
        {
            perror("accept failed");
            continue;
        }

        memset(buffer, 0, BUFFER_SIZE);
        read(client_fd, buffer, BUFFER_SIZE - 1);
        if (strncmp(buffer, "GET / ", 6) == 0)
        {
            write(client_fd, response_200, strlen(response_200));
        }
        else
        {
            write(client_fd, response_404, strlen(response_404));
        }

        close(client_fd);
    }

    close(server_fd);
    return 0;
}
