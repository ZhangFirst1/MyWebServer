#ifndef WEBSERVER_H
#define WEBSERVER_H

#include <unordered_map>
#include <fcntl.h>       // fcntl()
#include <unistd.h>      // close()
#include <assert.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "epoller.h"
#include "../timer/heaptimer.h"
#include "../log/log.h"
#include "../pool/sqlconnpool.h"
#include "../pool/threadpool.h"
#include "../http/httpconn.h"

class WebServer {
public:
    WebServer(
        int port, int trigMode, int timeoutMS, bool OptLinger,
        int sqlPort, const char* sqlUser, const char* sqlPwd,
        const char* dbName, int connPoolNum, int threadNum,
        bool openLog, int logLevel, int logQueSize
    );

    ~WebServer();
    void Start();

private:
    void InitEventMode_(int trigMode);          // 设置触发模式
    bool InitSocket_();                         // 初始化Socket
    void AddClient_(int fd, sockaddr_in addr);  // 添加客户端连接

    void DealListen_();                         // 处理监听套接字
    void DealWrite_(HttpConn* client);          // 处理写事件
    void DealRead_(HttpConn* client);           // 处理读事件

    void SendError_(int fd, const char* info);  // 发送错误信息
    void ExtentTime_(HttpConn* client);         // 调整定时事件
    void CloseConn_(HttpConn* client);          // 关闭客户端连接

    void OnRead_(HttpConn* client);
    void OnWrite_(HttpConn* clinet);
    void OnProcess_(HttpConn* clinet);

    static const int MAX_FD = 65536;            // 最大连接数

    static int SetFdNonblock(int fd);           // 设置文件非阻塞

    int port_;
    bool openLinger_;
    int timeoutMS_;
    bool isClose_;
    int listenFd_;
    char* srcDir_;

    uint32_t listenEvent_;  // 监听事件
    uint32_t connEvent_;    // 连接事件

    std::unique_ptr<HeapTimer> timer_;
    std::unique_ptr<ThreadPool> threadpool_;
    std::unique_ptr<Epoller> epoller_;
    std::unordered_map<int, HttpConn> users_;   // 客户端连接 fd conn
};

#endif