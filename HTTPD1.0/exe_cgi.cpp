#include <sys/types.h>
#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <signal.h>
#include <sys/stat.h>
#include "processpool.h"
#include <string>
#include <iostream>
#include <sys/sendfile.h>

#define SERVER_STRING "Server: httpd/0.1.0\r\n"

enum Method_t {
    GET = 1,
    POST,
    OTHER
};
typedef enum Method_t Method;

enum FileType_t {
    HTML = 1,
    CSS,
    JS
};
typedef enum FileType_t FileType;

static int get_line(int sockfd, char* line, int size)
{
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

	while (ch != '\n' && len < size -1){
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

void drop_header(int sockfd)
{
	int ret = -1;
	char buf[1024];
	do{
		ret = get_line(sockfd, buf, sizeof (buf));
	} while (ret > 0 && strcmp(buf, "\n") != 0);
}

void not_found(int client)
{
    char buf[1024];

    sprintf(buf, "HTTP/1.0 404 NOT FOUND\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, SERVER_STRING);
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

void cannot_execute(int client)
{
    char buf[1024];

    sprintf(buf, "HTTP/1.0 500 Internal Server Error\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "Content-type: text/html\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "<P>Error prohibited CGI execution.\r\n");
    send(client, buf, strlen(buf), 0);
}

void bad_request(int client)
{
    char buf[1024];

    sprintf(buf, "HTTP/1.0 400 BAD REQUEST\r\n");
    send(client, buf, sizeof(buf), 0);
    sprintf(buf, "Content-type: text/html\r\n");
    send(client, buf, sizeof(buf), 0);
    sprintf(buf, "\r\n");
    send(client, buf, sizeof(buf), 0);
    sprintf(buf, "<P>Your browser sent a bad request, ");
    send(client, buf, sizeof(buf), 0);
    sprintf(buf, "such as a POST without a Content-Length.\r\n");
    send(client, buf, sizeof(buf), 0);
}

void headers(int client, const char * file)
{
    char buf[1024];
    FileType type = HTML;
    //(void)filename;  /* could use filename to determine file type */
    
    strcpy(buf, "HTTP/1.0 200 OK\r\n");
    send(client, buf, strlen(buf), 0);
    strcpy(buf, SERVER_STRING);
#if 1
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "Content-Type: text/html\r\n");
#endif 
    send(client, buf, strlen(buf), 0);
    strcpy(buf, "\r\n");
    send(client, buf, strlen(buf), 0);
}

/* 判断文件类型发送出去 */


class cgi_conn {
public:
    cgi_conn(){}
    ~cgi_conn(){}

    /* 初始化客户端连接，清空读缓冲区 */
    void init( int epollfd, int sockfd, const struct sockaddr_in& client )
    {
        m_epollfd = epollfd;
        m_sockfd = sockfd;
        m_address = client;
        memset(m_buf, '\0', sizeof(m_buf));
        m_read_idx = 0;
        m_cgi = false;
    }

    void process()
    {
        int idx = 0;
        int ret = -1;
        /* 循环读取和分析客户数据 */
        while (true) {
            idx = m_read_idx;
            ret = recv(m_sockfd, m_buf + idx, sizeof(m_buf), 0);
            if (ret < 0) {
                if (errno != EAGAIN) {
                    removefd(m_epollfd, m_sockfd);
                }
                break;
            }
            /* 如果对方关闭连接，则服务器也关闭连接 */
            else if (ret == 0) {
               removefd( m_epollfd, m_sockfd );
               break;
            } else {
                m_read_idx += ret;
                //printf("user content is %s\n", m_buf);
                /* 如果遇到字符 "\r\n",则开始处理客户请求 */
                for (; idx < m_read_idx; ++idx) {
                    if ( (idx >= 1) && (m_buf[idx - 1] == '\r') && (m_buf[idx] == '\n') ) {
                        break;
                    }
                }

                if ( idx == m_read_idx ) {
                    continue;
                }
                m_buf[idx - 1] = '\0';
                
                std::string file_name = m_buf;
                std::cout << file_name << std::endl;

                /* 通过第一行获取 method & 文件 */
                get_method(file_name);
 
                struct stat st;
                if ( -1 == stat(m_file.c_str(), &st) ) {
                    not_found(m_sockfd);
                    std::cout << "文件 " << m_file << "没有找到" << std::endl;
                    break;
                }
                if ( S_ISDIR(st.st_mode) ) {
                    m_file.append("/blb.html");
                    //std::cout << "要返回的文件是 " << m_file << std::endl;
                } else if ( (st.st_mode & S_IXUSR) || (st.st_mode & S_IXGRP) || (st.st_mode & S_IXOTH) ) {
                    printf(" 需要执行 exe_cgi 的文件是 ： %s\n", m_file.c_str());
                    m_cgi = false;
                    std::cout << "POST 方法" << std::endl;
                } else {
                    printf("echo 的文件为 %s\n", m_file.c_str());
                }

                if (m_cgi) {
                    printf("do post\n");
                    exec_cgi(m_file.c_str());
                } else {
                    drop_header(m_sockfd);
                    echo_www(m_file.c_str(), st.st_size);
                    close(m_sockfd);
                }
            }
        }
    }

private:

    void echo_www(const char * file, size_t size)
    {
        int fd = open(file, O_RDONLY);
        if (fd < 0) {
            printf("不能打开此文件：%s\n", file);
            not_found(m_sockfd);
            close(fd);
            return;
        } 
        //发送 html css js 文件
        
        headers(m_sockfd, file);
        if (-1 == sendfile(m_sockfd, fd, NULL, size)) {
            std::cout << "发送失败\n";
            cannot_execute(m_sockfd);
        }
        printf("发送成功\n");
        close(fd);
        m_file.clear();
    }

    void exec_cgi(const char* path) {
    	char method_env[255];
    	char query_string_env[255];
    	char content_len_env[255];
    	bzero(method_env, sizeof (method_env));
    	bzero(query_string_env, sizeof (query_string_env));
    	bzero(content_len_env, sizeof (content_len_env));
    	const int BUFF_SIZE = 1024;
    	int input[2];
    	int output[2];
    	int content_len = -1;
    
    	//if (strcasecmp(method, "GET") == 0){	//clear request socket, get query string
        if (m_method == GET) {
    		drop_header(m_sockfd);
    	}
    	
    	char buf[BUFF_SIZE/10];
    	//if (strcasecmp(method, "POST") == 0){	//get Content-Length send msg for child
        if (m_method == POST) {
    		int ret = -1;
    		do {
    			bzero(buf, sizeof (buf));
    			ret = get_line(m_sockfd, buf, sizeof (buf));
    			if ( !strncasecmp(m_buf, "Content-Length: ", 16) ){
    				content_len = atoi(&buf[16]);
    			}
    		} while ( ret > 0 && strcmp(buf, "\n") );
    		if (-1 == content_len) {
    			bad_request(m_sockfd);
                return;
    		}
    	}
    
    	if (-1 == pipe(input) || -1 == pipe(output)) {
            cannot_execute(m_sockfd);
            return;
    	}
    
    	pid_t id = fork();
    	if (id < 0) {
            cannot_execute(m_sockfd);
    		return;
    	}
    
        /* response to server */	
        const char* msg = "HTTP/1.0 200 OK\r\n";
        send(m_sockfd, msg, strlen(msg), 0);
        
        if (id == 0) {//child
    	
            close(input[1]);
    		close(output[0]);
    		/* 程序替换后子进程通过往 fd 0 给父进程发送消息，从 fd 1 读取父进程发送过来的消息 */
    		
    		dup2(input[0], 0);
    		dup2(output[1],1);

            if (m_method == GET) {
		        sprintf(method_env, "METHOD=%s", "GET");
    			sprintf(query_string_env, "QUERY_STRING_ENV=%s", m_querystring.c_str());
    			putenv(query_string_env);
            } else if (m_method == POST) {
		        sprintf(method_env, "METHOD=%s", "POST");
    			sprintf(content_len_env, "CONTENT_LEN_ENV=%d", content_len);
    			putenv(content_len_env);
    		} else {
    			//other method
		        sprintf(method_env, "METHOD=%s", "OTHER_METHOD");
    			printf("other method is %s\n", "other method");
    		}
    		putenv(method_env);
            execl(path, path, NULL);
            exit(0);
        } else {
    		//father
    		close(input[0]);
    		close(output[1]);
    
    		char c = '\0';
    		int ret = -1;
    		//if ( strcasecmp("POST", method) == 0){	//method is post,send msg to child
            if (m_method == POST) {
    			int i = 0;
    			for (; i < content_len; ++i) {
    				recv(m_sockfd, &c, 1, 0);
    				write(input[1], &c, sizeof (char));
    			}
            }
    		while ( read(output[0], &c, 1) > 0 ){	//send child data to server
    			send(m_sockfd, &c, 1, 0);
    		}
    		waitpid(id, NULL, 0);
    		close(input[1]);
    		close(output[0]);
        }
    }
   
    void get_method(std::string& str)
    {
        if ((str.find( "GET" ) != std::string::npos) || (str.find( "get" ) != std::string::npos))
            m_method = GET;
        else if ((str.find( "POST" ) != std::string::npos) || (str.find("post") != std::string::npos))
            m_method = POST;
        else 
            m_method = OTHER;
        int file_pos = -1;
        bool flag = false;
        if ((file_pos = str.find("/")) != std::string::npos){
            for (int i = file_pos+1; i < str.size() && (!isspace(str[i])); ++i) {
                if (str[i] == '?') {
                    flag = true;
                    i++;
                }
                if (flag)
                    m_querystring += str[i];
                else 
                    m_file += str[i];
            }
        } else {
            m_cgi = false;
        }
    }

private:
    /* 读缓冲区大小 */
    Method m_method;
    std::string m_file;
    static const int BUFFER_SIZE = 1024;
    std::string m_querystring;
    int m_epollfd;
    int m_sockfd;
    sockaddr_in m_address;
    char m_buf[BUFFER_SIZE];
    bool m_cgi;
    /* 标记读缓冲中已经读入客户数据的最后一个字节的下一个位置 */
    int m_read_idx;
};

void usage(const char * arg)
{
    printf("usage : \r\n%s [ip] [port]\n", arg);
}

int start_up(const char * ip, int port)
{
    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    assert(listenfd >= 0);

    int ret = 0;
    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons( port );
    inet_pton( AF_INET, ip, &address.sin_addr );

    ret = bind( listenfd, (struct sockaddr*)&address, sizeof(address) );
    assert( ret != -1 );
    return listenfd;
}

int main( int argc, char * argv[] )
{
    if (argc != 3) {
        usage(argv[0]);
        return 1;
    }

    const char * ip = argv[1];
    int port = atoi( argv[2] );
    int listenfd = start_up(ip, port);
    int ret = -1;
    ret = listen( listenfd, 5 );
    assert( ret != -1 );

    processpool< cgi_conn >* pool = processpool< cgi_conn >::create( listenfd );
    if (pool) {
        pool->run();
        delete pool;
    }
    close( listenfd );
    return 0;
}

# if 0
locker lock;

/*
 * 子线程运行的函数，他首先获得互斥锁，然后暂停 5s，再释放该互斥锁 
 */
void *anther(void *arg) 
{
    printf("in child thread, lock the mutex\n");
    lock.lock();
    sleep(5);
    lock.unlock();
}

void prepare()
{
    lock.lock();
}

void parent()
{
    lock.unlock();
}

void child()
{
    lock.unlock();
}

int main()
{
    pthread_t tid;
    pthread_create(&tid, NULL, anther, NULL);
   
    /*
     * 父进程中的主线成现暂停 1s 确保在执行 fork 之前， 
     * 子线程已经开始运行并且拿到了互斥锁 
     */

    sleep(1);
    pthread_atfork(prepare, parent, child);
    int pid = fork();
    if ( pid < 0 ) {
        pthread_join( pid, NULL );
        return 1;
    } else if ( 0 == pid ) {
        printf(" I'm child, want to get the lock\n");
        /*
         * 子进程从父进程中继承了互斥锁的状态，
         * 该互斥锁处于锁住状态，这是由父进程中的子线程引起的，
         * 因此子进程想要获取锁就会一直阻塞，尽管从逻辑上他不应该被阻塞
         */
        sleep(1);
        lock.lock();
        printf("I am child  get lock \n");
        sleep(2);
        lock.unlock();
        std::cout << "I'm child I relese the lock\n";
        exit(0);
    } else {
        std::cout << "I'm father I want get lock\n";
        lock.lock();
        std::cout << "I'm father I get lock\n";

        sleep(3);
        std::cout << "I'm father I relese the lock\n";
        lock.unlock();
        wait(NULL);
    }

    pthread_join(tid, NULL);
    return 0;
}
#endif
