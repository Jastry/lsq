#include <stdio.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <strings.h>
#include <string.h>
#include <fcntl.h>
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/sendfile.h>
#include <sys/wait.h>

#define BUFF_SIZE 1024

void Usage(const char * arg);
void CreateWorker(int client_fd, struct sockaddr_in* client_addr); 
int get_line(int sockfd, char* line, int size); 
void ProcessRequest(int connfd, struct sockaddr_in* client_addr); 
void drop_header(int sockfd); 
void echo_www(int sockfd, const char* path, ssize_t size); 
int get_line(int sockfd, char* line, int size); 
void headers(int client); 
void not_found(int client); 

int main(int argc, char* argv[]) {
    if (argc != 3) {
        Usage(argv[0]);
        return 1;
    }
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr(argv[1]);
    addr.sin_port = htons(atoi(argv[2]));
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }
    int ret = bind(fd, (struct sockaddr*)&addr, sizeof(addr));
    if (ret < 0) {
        perror("bind");
        return 1;
    }
    ret = listen(fd, 10);
    if (ret < 0) {
        perror("listen");
        return 1;
    }
    for (;;) {
        struct sockaddr_in client_addr;
        socklen_t len = sizeof(client_addr);
        int client_fd = accept(fd, (struct sockaddr*)&client_addr, &len);
        if (client_fd < 0) {
            perror("accept");
            continue;
        }
        CreateWorker(client_fd, &client_addr);
    }
    return 0;
}

void Usage(const char * arg) {
    printf("usage: %s [ip] [port]\n", arg);
}

void CreateWorker(int client_fd, struct sockaddr_in* client_addr) {
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return;
    } else if (pid == 0) {
        // child
        if (fork() == 0) {
            // grand_child init 进程去收养
            ProcessRequest(client_fd, client_addr);
        }
        exit(0);
    } else {
        // father
        close(client_fd);
        waitpid(pid, NULL, 0);
    }
}

int get_line(int sockfd, char* line, int size) {
	/*
     * ret > 1,line != '\0' 表示读的是数据;
     * ret == 1 && line == '\n'表示空行;
     * ret <= 0 && line == '\0'读完了;
     * '\r' -> '\n'; '\r\n' -> '\n'
     */

	assert(line);
	int  len = 0;
	int  r = 0;
	char ch = '\0';

	while (ch != '\n' && len < size -1) {
		r = recv(sockfd, &ch, 1, 0);
		if (r > 0) {
			if (ch == '\r'){
				recv(sockfd, &ch, 1, MSG_PEEK);
				if (ch == '\n')
					recv(sockfd, &ch, 1, 0);
				ch = '\n';
			}
			line[len++] = ch;
		} else {
				ch = '\n';
		}
	}
	line[len] = '\0';
	return len;
}

void drop_header(int sockfd) {
	int ret = -1;
	char buf[BUFF_SIZE];
	do{
		ret = get_line(sockfd, buf, sizeof (buf));
	} while (ret > 0 && strcmp(buf, "\n") != 0);
}

void ProcessRequest(int connfd, struct sockaddr_in* client_addr) {
	printf("get a client\n ip [%s], port [%d]\n",
           inet_ntoa((*client_addr).sin_addr), 
           ntohs((*client_addr).sin_port));
    char line[BUFF_SIZE];
	char method[BUFF_SIZE/10];
	char path[BUFF_SIZE/10];
	char url[BUFF_SIZE/10];

	bzero(line, sizeof (line));
	bzero(method, sizeof (line));
	bzero(path, sizeof (line));
	bzero(url, sizeof (line));
	int cgi = 0;

	/* 从请求行中提取方法和路径 */
	int size = get_line(connfd, line, sizeof (line));
	if ( size <= 0 ){
        perror("read failed\n");
        close(connfd);
        return;
	}

	int i = 0;
	while (!isspace( line[i] ) && i < size){
		// get method
		method[i] = line[i];
		i++;
	}
	method[i] = 0;

	while (isspace(line[i])) // clear space
		i++;

	int j = 0;
	while (!isspace(line[i])){
		// get url
		url[j] = line[i];
		j++; i++;
	}
	url[j] = 0;

	char* query_string = NULL;
	//get
	if ( strcasecmp("GET", method) == 0 ) {	//get query string
		for (i = 0; i < j; ++i){
			if (url[i] == '?'){	/*GET 请求，参数与路径用 '?' 号隔开 */
				url[i] = 0;
				query_string = &url[++i];
				break;
			}
		}
		sprintf(path, "%s", url+1);
		if (path[strlen(path - 1)] == '/'){
			strcat(path, "index.html");
		}
	}else {
		//post
		sprintf(path,"%s", url+1);
	}

	if (query_string)
		cgi = 1;

	struct stat st;
	if (-1 == stat(path, &st)){
		//404
        not_found(connfd);
        perror("stat");
        close(connfd);
		return;
	}
	if ( S_ISDIR(st.st_mode) ) {	/* 该文件是目录，返回静态网页 */
		strcat(path, "/index.html");
	}else if ((st.st_mode & S_IXUSR) || (st.st_mode & S_IXGRP) || (st.st_mode & S_IXOTH)) {
		//printf("there is 224 ,path is %s\n", path);
		cgi = 1;
	} else {}	/*other file type*/

	if (cgi){
        printf("have to execute cgi\n");
	}else {
		drop_header(connfd);
		echo_www(connfd, path, st.st_size);
	}
}

void echo_www(int sockfd, const char* path, ssize_t size) {
	int fd = open(path, O_RDONLY);
	if (fd < 0) {
        not_found(sockfd);
		return;
	}
    headers(sockfd);
	if ( -1 == sendfile(sockfd, fd, NULL, size) ) {
        perror("sendfile");
	}
	close(fd);
}

void headers(int client) {
    char buf[1024];
    //(void)filename;  /* could use filename to determine file type */

    strcpy(buf, "HTTP/1.0 200 OK\r\n");
    send(client, buf, strlen(buf), 0);
    strcpy(buf, "Server: httpd/0.1.0\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "Content-Type: text/html\r\n");
    send(client, buf, strlen(buf), 0);
    strcpy(buf, "\r\n");
    send(client, buf, strlen(buf), 0);
}

void not_found(int client) {
    char buf[1024];

    sprintf(buf, "HTTP/1.0 404 NOT FOUND\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "Server: httpd/0.1.0\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "Content-Type: text/html\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "<HTML><TITLE>Not Found</TITLE>\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "<BODY><P>The server could not fulfill\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "your request because the resource specified\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "is unavailable or nonexistent.\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "</BODY></HTML>\r\n");
    send(client, buf, strlen(buf), 0);
}

