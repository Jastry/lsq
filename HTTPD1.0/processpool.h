#ifndef __PROCESSPOOL_H__
#define __PROCESSPOOL_H__

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/wait.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>

/*
 *  描述一个子进程的类，m_pid 是目标子进程的 PID，m_pipefd 是父进程和子进程通信用的管道
 */
class process {
public:
    process():m_pid(-1) {}

public:
    pid_t m_pid;
    int m_pipefd[2];
};

/*
 * 进程池类，将他定义为模板类是为了代码复用，其模板参数是处理逻辑任务的类 
 */
template <typename T>
class processpool {
private:
    /* 
     * 将构造函数定义为私有的，因为我们只能通过 create 静态函数创建 processpool 实例 
     */
    processpool(int listenfd, int process_number = 8);
public:
    static processpool< T > * create( int listenfd, int process_number = 8 )
    {
        if (!m_instance) {
            m_instance = new processpool< T >(listenfd, process_number);
        } 
        return m_instance;
    }

    ~processpool()
    {
        delete [] m_sub_process;
    }

    void run();
private:
    void setup_sig_pipe();
    void run_parent();
    void run_child();

private:
    /* 进程池允许最大子进程数量 */
    static const int MAX_PROCESS_NUMBER = 16;

    /* 每个字进程最多能处理客户数量 */
    static const int USER_PRE_PROCESS = 65535;

    /* epoll 最多能处理的事件数 */
    static const int MAX_EVENT_NUMBER = 10000;

    /* 进程池中的进程的总数 */
    int m_process_number;

    /* 子进程在池中的序号，从 0 开始 */
    int m_idx;

    /* 每个进程都有一个 epoll 内核事件, 用 m_epollfd 标识 */
    int m_epollfd;

    /* 监听 socket */
    int m_listenfd;

    /* 子进程通过 m_stop 来决定是否停止运行 */
    int m_stop;

    /* 保存所有子进程的描述信息 */
    process * m_sub_process;

    /* 进程池静态实例 */
    static processpool< T > * m_instance;

};
template < typename T >
processpool< T > * processpool< T >::m_instance = NULL;

/* 
 * 用于处理信号的管道，以实现统一信息源。后面称之为信号管道
 */

static int sig_pipefd[2];

/* 设置文件描述符非阻塞 */
static int setnoblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL); 
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

static void addfd(int epollfd, int fd)
{
    int ret = -1;
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLET;
    ret = epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnoblocking( fd );
}

/* 从 epollfd 标识的 epoll 内核事件中删除 fd 上所有注册的事件 */
static void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, NULL);
}

static void sig_headler(int sig)
{
    int save_errno = errno;
    int msg = sig;
    send(sig_pipefd[1], (char*)&msg, 1, 0);
    errno = save_errno;
}

static void addsig(int sig, void(handler)(int), bool restart = true)
{
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;
    if (restart) {
        sa.sa_flags | SA_RESTART;
    }
    sigfillset(&sa.sa_mask);
    /*_______________________________________*/
    assert( sigaction(sig, &sa, NULL) != -1);
}

/*
 * 进程池构造函数，参数 listenfd 是监听 socket，
 * 他必须要在创建进程池之前被创建，否则子进程无法直接用到它。
 * 参数 process_number 指定进程池中子进程的数量 
 */
template < typename T >
processpool< T >::processpool(int listenfd, int process_number)
    :m_listenfd(listenfd), m_process_number(process_number), m_idx(-1), m_stop(false)
{
    assert( (process_number > 0) && (process_number <= MAX_PROCESS_NUMBER) );

    m_sub_process = new process[ process_number ];
    assert(m_sub_process);

    /* 创建 process_number 个子进程，并让他门建立和父进程之间通信管道 */
    int i = 0;
    for (; i < process_number; ++i) {
        int ret = socketpair(PF_UNIX, SOCK_STREAM, 0, m_sub_process[i].m_pipefd);
        assert(0 == ret);
        
        m_sub_process[i].m_pid = fork();
        assert(m_sub_process[i].m_pid >= 0);
        if (m_sub_process[i].m_pid > 0) {
            close(m_sub_process[i].m_pipefd[1]);
            continue;
        } else {
            close(m_sub_process[i].m_pipefd[0]);
            m_idx = i;
            break;
        }
    }
}

/* 同一事件源 */
template < typename T>
void processpool< T >::setup_sig_pipe()
{
    /* 创建 epoll 事件监听表和信号管道 */
    m_epollfd = epoll_create(5);
    assert(m_epollfd != -1);
    int ret = socketpair(PF_UNIX, SOCK_STREAM, 0, sig_pipefd);
    assert( ret != -1 );
    
    setnoblocking(sig_pipefd[1]);
    addfd(m_epollfd, sig_pipefd[0]);

    /* 设置信号处理函数 */
    addsig(SIGCHLD, sig_headler);
    addsig(SIGTERM, sig_headler);
    addsig(SIGINT, sig_headler);
    addsig(SIGPIPE, sig_headler);
}

/* 
 * 父进程中 m_idx 为 -1， 子进程中 m_idx > 0 ~ 7, 
 * 我们据此判断接下来要运行的是父进程代码还是子进程代码
 */
template < typename T >
void processpool< T >::run()
{
    if (m_idx != -1) {
        run_child();
        return;
    } 
    run_parent();
}

template < typename T >
void processpool< T >::run_child()
{
    setup_sig_pipe();

    /* 每个子进程都通过在进程池中的序号 m_idx 找到与父进程通信的管道 */
    int pipefd = m_sub_process[m_idx].m_pipefd[1];
    
    /* 子进程需要监听管道文件描述符 pipefd， 因为父进程将通过它通知子进程 accept 新连接 */
    addfd(m_epollfd, pipefd);

    epoll_event events[ MAX_EVENT_NUMBER ];
    T * users = new T[ USER_PRE_PROCESS ];
    assert(users);
    int number = 0;
    int ret = -1;

    while (! m_stop) {
        number = epoll_wait(m_epollfd, events, MAX_EVENT_NUMBER, -1);
        if ( (number < 0) && (errno != EINTR) ) {
            printf("epoll failure\n");
            break;
        }
        int i = 0;
        for (i = 0; i < number; ++i) {
            int sockfd = events[i].data.fd;
            if ( (sockfd == pipefd) && ( events[i].events & EPOLLIN )) {
                int client = 0;
                /*
                 * 从父子进程之间的管道读取数据，并将结果保存在 client 中。
                 * 如果读取成功则表示有新用户连接 
                 */
                ret = recv( sockfd, ( char* )&client, sizeof(client), 0 );
                if ( ( ( ret < 0) && ( errno != EAGAIN )) || ret == 0) {
                    continue;
                } else {
                    struct sockaddr_in client_address;
                    bzero(&client_address, sizeof(client_address));
                    socklen_t addr_len = sizeof(client_address);
                    int connfd = accept(m_listenfd, (struct sockaddr*)&client_address, &addr_len);

                    printf("get a client ip %s, port is %d \n", inet_ntoa(client_address.sin_addr), ntohs(client_address.sin_port));
                    
                    if (connfd < 0) {
                        perror("accept");
                        continue;
                    }
                    addfd(m_epollfd, connfd);

                    /* 
                     * 模板类 T 必须事先 init 方法，
                     * 以初始化一个客户连接。我们直接用 connfd 
                     * 来索引逻辑处理对象（T 类型的对象),以提高程序效率
                     */
                    users[connfd].init( m_epollfd, connfd, client_address );
                }
            }
            /* 接收到子进程的信号 */
            if ( (sockfd == sig_pipefd[0]) && ( events[i].events & EPOLLIN ) ) {
                int sig;
                char signals[1024];
                ret = recv(sig_pipefd[0], signals, sizeof(signals), 0);
                if (ret <= 0) {
                    continue;
                } else {
                    int i = 0;
                    for (i = 0; i < ret; ++i) {
                        switch (signals[i]) {
                            case (SIGCHLD): 
                            {
                                pid_t pid;
                                int stat;
                                while ((pid = waitpid(-1, &stat, WNOHANG)) > 0) {
                                    continue;
                                }
                                break;
                            }
                            case (SIGTERM):
                            case (SIGINT):
                            {
                                m_stop = true;
                                break;
                            }
                            default:
                            break;
                        }
                    }
                }

            }
            /* 如果是其他可读事件一定是客户连接请求到来。调用逻辑处理对象的 process 方法处理之 */
            else if (events[i].events & EPOLLIN) {
                users[sockfd].process();
            } else 
                continue;
        }
    }
    delete [] users;
    users = NULL;
    close(m_listenfd);
}

template < typename T >
void processpool< T >::run_parent()
{
    setup_sig_pipe();

    /* 父进程监听 m_listenfd */
    addfd(m_epollfd, m_listenfd);

    epoll_event events[ MAX_EVENT_NUMBER ];
    int sub_process_counter = 0;
    int new_conn = 1;
    int number = 0;
    int ret = -1;
    while ( ! m_stop ) {
        number = epoll_wait(m_epollfd, events, MAX_EVENT_NUMBER, -1);
        if ( ( number < 0 ) && (errno != EINTR) ) {
            printf("epoll failure\n");
            break;
        }

        for (int i = 0; i < number; ++i) {
            int sockfd = events[i].data.fd;
            if ( sockfd == m_listenfd ) {
                int i = sub_process_counter;
                /* 随机从进程池找一个孩子让其服务该连接 */
                do {
                    if ( m_sub_process[i].m_pid != -1 ) {
                        break;
                    }
                    i = (i+1) % m_process_number;
                } while (i != sub_process_counter );

                if ( m_sub_process[i].m_pid == -1 ) {
                    m_stop = true;
                    break;
                }
                sub_process_counter = (i+1) % m_process_number;
                send( m_sub_process[i].m_pipefd[0],
                      (char*)&new_conn, sizeof(new_conn), 0);
                printf("send request to child %d\n", i);
            }

            /* 下面处理父进程接收到信号 */
            else if ( (sockfd == sig_pipefd[0]) && (events[i].events & EPOLLIN) ) {
                int sig;
                char signals[1024];
                ret = recv(sig_pipefd[0], signals, sizeof(signals), 0);
                if (ret <= 0) {
                    continue;
                } else {
                    for (int i = 0; i < ret; ++i) {
                        switch (signals[i]) {
                            case (SIGCHLD):{
                                /* 处理僵尸进程 */
                                pid_t pid;
                                int stat;
                                while ( (pid = waitpid(-1, &stat, WNOHANG)) > 0 ) {
                                    for ( int i = 0; i < m_process_number; ++i ) {
                                        /* 
                                         * 如果进程池中第 i 个子进程退出了，
                                         * 则主进程关闭相应的管道，
                                         * 并设置相应的 m_pid 为 -1，
                                         * 以标记子进程已经推出 
                                         */
                                        if ( m_sub_process[i].m_pid == pid ) {
                                            printf("child %d join\n", i);
                                            close(m_sub_process[i].m_pipefd[0]);
                                            m_sub_process[i].m_pid = -1;
                                        }
                                    }
                                }
                                /* 如果所有子进程退出父进程也退出 */
                                m_stop = true;
                                for (int i = 0; i < m_process_number; ++i) {
                                    if ( m_sub_process[i].m_pid != -1 ) {
                                        m_stop = false;
                                    }
                                }
                                break;
                            }
                            case (SIGTERM):
                            case (SIGINT): {
                                /* 
                                 * 如果父进程接收到终止信号，
                                 * 呢么就杀死所有子进程，并等待他们全部结束
                                 * 当然，通知父子进程结束的更好的方法是向父、子进程
                                 * 之间的通信管道发送特殊数据,自己实现
                                 */
                                printf("kill all the child now\n");
                                for (int i = 0; i < m_process_number; ++i) {
                                    int pid = m_sub_process[i].m_pid;
                                    if (pid != -1) {
                                        kill(pid, SIGTERM);
                                    }
                                }
                                break;
                            }
                            default:
                                break;
                        }
                    }
                }
            }
            else 
                continue;
        }
    }
    close( m_epollfd );
}

#endif
