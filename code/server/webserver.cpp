#include "webserver.h"

using namespace std;

WebServer::WebServer(
    int port, int trigMode, int timeoutMS, bool OptLinger,
    int sqlPort, const char* sqlUser, const  char* sqlPwd,
    const char* dbName, int connPoolNum, int threadNum,
    bool openLog, int logLevel, int logQueSize):
    port_(port), openLinger_(OptLinger), timeoutMS_(timeoutMS), isClose_(false),
    timer_(new HeapTimer()), threadpool_(new ThreadPool(threadNum)), epoller_(new Epoller())
{
    srcDir_ = getcwd(nullptr, 256);         // 获取工作目录
    assert(srcDir_);
    strncat(srcDir_, "/resources/", 16);    // 添加路径
    HttpConn::userCount = 0;
    HttpConn::srcDir = srcDir_;
    SqlConnPool::Instance()->Init("localhost", sqlPort, sqlUser, sqlPwd, dbName, connPoolNum);

    InitEventMode_(trigMode);
    if(!InitSocket_()) isClose_ = true;

    if(openLog) {
        Log::Instance()->init(logLevel, "./log", ".log", logQueSize);
        if(isClose_) { LOG_ERROR("========== Server init error!=========="); }
        else {
            LOG_INFO("========== Server init ==========");
            LOG_INFO("Port:%d, OpenLinger: %s", port_, OptLinger? "true":"false");
            LOG_INFO("Listen Mode: %s, OpenConn Mode: %s",
                            (listenEvent_ & EPOLLET ? "ET": "LT"),
                            (connEvent_ & EPOLLET ? "ET": "LT"));
            LOG_INFO("LogSys level: %d", logLevel);
            LOG_INFO("srcDir: %s", HttpConn::srcDir);
            LOG_INFO("SqlConnPool num: %d, ThreadPool num: %d", connPoolNum, threadNum);
        }
    }
}

WebServer::~WebServer() {
    close(listenFd_);
    isClose_ = true;
    free(srcDir_);
    SqlConnPool::Instance()->ClosePool();
}

// 初始化事件模式 LT和ET
void WebServer::InitEventMode_(int trigMode){
    listenEvent_ = EPOLLRDHUP;              // 检测socket关闭
    connEvent_ = EPOLLONESHOT | EPOLLHUP;   // EPOLLONESHOT相同事件只由一个线程处理
    /* listenfd和connfd的模式组合 默认LT+LT
        0 LT + LT
        1 LT + ET
        2 ET + LT
        3 ET + ET
    */
    switch (trigMode){
        case 0:
            break;
        case 1:
            connEvent_ |= EPOLLET;          // 设为边缘触发模式
            break;
        case 2:
            listenEvent_ |= EPOLLET;
            break;
        case 3:
            listenEvent_ |= EPOLLET;
            connEvent_ |= EPOLLET;
            break;
        default:
            listenEvent_ |= EPOLLET;
            connEvent_ |= EPOLLET;
            break;
    }
    HttpConn::isET = (connEvent_ & EPOLLET);
}

void WebServer::Start() {
    int timeMS = -1;    /* epoll wait timeout == -1 无事件将阻塞 */
    if(!isClose_) { LOG_INFO("========== Server start =========="); }
    while(!isClose_) {                          // 主事件循环 服务器没关闭就一直执行
        if(timeoutMS_ > 0){                     // 设置了超时时间大于0
            timeMS = timer_->getNextTick();     // 获取下一次的超时等待时间
        }
        int eventCnt = epoller_->Wait(timeMS);  // 返回事件的数量
        for(int i = 0; i < eventCnt; i++){      // 处理事件
            int fd = epoller_->GetEventFd(i);   // 获取第i个事件的文件描述符
            uint32_t events = epoller_->GetEvent(i); // 第i个事件的具体类型
            if(fd == listenFd_){                // 如果文件描述符是监听套接字，处理监听事件
                 DealListen_();
            }else if(events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)){ // 处理连接中的异常事件，比如对端关闭连接（`EPOLLRDHUP`），连接发生错误（`EPOLLHUP` 或 `EPOLLERR`）
                assert(users_.count(fd) > 0);
                CloseConn_(&users_[fd]);
            }else if(events & EPOLLIN){         // 处理连接的读事件
                assert(users_.count(fd) > 0);
                DealRead_(&users_[fd]);
            }else if(events & EPOLLOUT){        // 处理连接的写事件
                assert(users_.count(fd) > 0);
                DealWrite_(&users_[fd]);
            }else{                              // 未知的事件类型
                LOG_ERROR("Unexpected event");
            } 
        }
    }
}

void WebServer::SendError_(int fd, const char* info){
    assert(fd > 0);
    int ret = send(fd, info, strlen(info), 0);
    if(ret < 0){
        LOG_WARN("send error to client[%d] error!", fd);
    }
    close(fd);
}

// 添加客户端连接
void WebServer::AddClient_(int fd, sockaddr_in addr){
    assert(fd > 0);
    users_[fd].init(fd, addr);
    if(timeoutMS_ > 0) {
        timer_->add(fd, timeoutMS_, std::bind(&WebServer::CloseConn_, this, &users_[fd]));
    }
    epoller_->AddFd(fd, EPOLLIN | connEvent_);
    SetFdNonblock(fd);
    LOG_INFO("Client[%d] in!", users_[fd].GetFd());
}

// 关闭客户端连接
void WebServer::CloseConn_(HttpConn* client){
    assert(client);
    LOG_INFO("Client[%d] quit!", client->GetFd());
    epoller_->DelFd(client->GetFd());
    client->Close();
}

// 处理监听套接字 accept新的套接字 并加入timer和epoller中
void WebServer::DealListen_(){
    struct sockaddr_in addr;
    socklen_t len = sizeof addr;
    do{
        int fd = accept(listenFd_, (struct sockaddr *)&addr, &len);
        if(fd <= 0) return;
        else if(HttpConn::userCount >= MAX_FD){
            SendError_(fd, "Server busy!");
            LOG_WARN("Clients is full!");
            return;
        }
        AddClient_(fd, addr);
    }while(listenEvent_ & EPOLLET);
}

// 处理读事件 将OnRead加入线程池的任务队列
void WebServer::DealRead_(HttpConn* client) {
    assert(client);
    ExtentTime_(client);
    threadpool_->AddTask(std::bind(&WebServer::OnRead_, this, client));
}

// 处理写事件 将OnWrite加入线程池的任务队列
void WebServer::DealWrite_(HttpConn* client) {
    assert(client);
    ExtentTime_(client);
    threadpool_->AddTask(std::bind(&WebServer::OnWrite_, this, client));
}

// 定时事件处理
void WebServer::ExtentTime_(HttpConn* client) {
    assert(client);
    if(timeoutMS_ > 0){
        timer_->adjust(client->GetFd(), timeoutMS_);
    }
}

void WebServer::OnRead_(HttpConn* client) {
    assert(client);
    int ret = -1;
    int readErrno = 0;
    ret = client->read(&readErrno);         // 读取客户端套接字的数据，读到httpconn的读缓冲区
    if(ret <= 0 && readErrno != EAGAIN){    // 读异常 关闭客户端
        CloseConn_(client);
        return;
    }
    // 业务逻辑处理 先读后处理
    OnProcess_(client);
}

void WebServer::OnWrite_(HttpConn* client) {
    assert(client);
    int ret = -1;
    int writeErrno = 0;
    ret = client->write(&writeErrno);
    if(client->ToWriteBytes() == 0){    
        // 传输完成
        if(client->IsKeepAlive()) {
            epoller_->ModFd(client->GetFd(), connEvent_ | EPOLLIN); // 继续监听读事件
            return;
        }
    }else if(ret < 0){
        if(writeErrno == EAGAIN){   // 缓冲区满
            // 继续传输
            epoller_->ModFd(client->GetFd(), connEvent_ | EPOLLOUT);
            return;
        }
    }
    CloseConn_(client);
}

void WebServer::OnProcess_(HttpConn* client) {
    // 调用process()进行逻辑处理
    if(client->process()){
        // 根据返回的信息将fd重新设置为EPOLOUT（写）或EPOLLIN（读）
        // 读完事件告诉内核可以写
        epoller_->ModFd(client->GetFd(), connEvent_ | EPOLLOUT);    // 响应成功，修改监听事件为写，等待OnWrite_()发送
    }else{
        // 写完事件告诉内核可以读
        epoller_->ModFd(client->GetFd(), connEvent_ | EPOLLIN);
    }
}

bool WebServer::InitSocket_() {
    int ret;
    struct sockaddr_in addr;
    if(port_ > 65535 || port_ < 1024){
        LOG_ERROR("Port:%d error!", port_);
        return false;
    }
    addr.sin_family = AF_INET;                  // UNIX地址族
    addr.sin_addr.s_addr = htonl(INADDR_ANY);   // 主机字节序转换为网络字节序
    addr.sin_port = htons(port_);
    struct linger optLinger = {0};              // 用于设置tcp断开链接时的断开方式
    if(openLinger_) {
        // 优雅关闭 发送完剩余数据或超时后关闭
        optLinger.l_onoff = 1;
        optLinger.l_linger = 1;
    }

    listenFd_ = socket(AF_INET, SOCK_STREAM, 0);// 创建socket
    if(listenFd_ < 0) {
        LOG_ERROR("Create socket error!", port_);
        return false;
    }

    // socket设置
    ret = setsockopt(listenFd_, SOL_SOCKET, SO_LINGER, &optLinger, sizeof(optLinger));
    if(ret < 0) {
        close(listenFd_);
        LOG_ERROR("Init linger error!", port_);
        return false;
    }

    int optval = 1;
    // 端口复用
    // 只有最后一个套接字会正常接受数据 
    ret = setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, (const void*)&optval, sizeof(int));
    if (ret == -1){
        LOG_ERROR("set socket setsockopt error!");
        close(listenFd_);
        return false;
    }
    // 绑定
    ret = bind(listenFd_, (struct sockaddr*)&addr, sizeof addr);
    if(ret < 0){
        LOG_ERROR("Bind Port:%d error!", port_);
        close(listenFd_);
        return false;
    }
    // 监听
    ret = listen(listenFd_, 6);
    if(ret < 0) {
        LOG_ERROR("Listen port:%d error!", port_);
        close(listenFd_);
        return false;
    }
    ret = epoller_->AddFd(listenFd_, listenEvent_ | EPOLLIN);
    if(ret == 0){
        LOG_ERROR("Add listen error!");
        close(listenFd_);
        return false;
    }
    SetFdNonblock(listenFd_);
    LOG_INFO("Server port:%d", port_);
    return true;
}

// 设置文件为非阻塞
int WebServer::SetFdNonblock(int fd) {
    assert(fd > 0);
    return fcntl(fd, F_SETFL, fcntl(fd, F_GETFD, 0) |  O_NONBLOCK);
}