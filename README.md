# MyWebServer
​	根据项目https://github.com/markparticle/WebServer实现的c++服务器项目。同时参考JehanRio的博客https://blog.csdn.net/weixin_51322383/article/details/130464403，十分感谢博客提供的帮助，本文用于记录学习过程以及遇到的一些问题。

​	项目Github：https://github.com/ZhangFirst1/MyWebServer

### 0. 项目基本介绍

​	根据实现功能，将项目分为以下几个模块：

- **缓冲区：**基于Vector容器实现的自动增长缓冲区。
- **日志：**阻塞队列与单例模式实现异步日志功能。
- **线程池与数据库连接池：**基于生产者消费者模型实现线程池，RAII实现数据库连接池，可注册登录。
- **Epoll&IO复用：**利用IO复用、Epoll、线程池实现Reactor模型。
- **http连接：**有限状态机解析Http请求，生成响应消息，处理连接逻辑。
- **定时器：**基于小根堆实现定时器，处理定时超时连接。
- **Server：**进行总体设置，处理逻辑。

​	项目总体采用**Reactor多线程模型**，后续可能增加文件上传等功能，敬请期待。

### 1.缓冲区

​	使用muduo的缓冲区实现，参考博客：https://blog.csdn.net/wanggao_1990/article/details/119426351

**1.1为什么要实现缓冲区：**

​	在源项目muduo中，实现非阻塞IO，消息在TcpConnection过程中未必是一次性发送或读取完（与TCP协议有关），故需要实现缓冲区。在本项目中，在Http请求消息读取和Http响应消息生成时都需要先经过缓冲区，（为什么）

**1.2缓冲区具体实现：**

​	buffer分为三个区域，分别是**prependable**、**readable**、**writable**。

![image-20231128202312266](https://img2023.cnblogs.com/blog/3304168/202311/3304168-20231128202313113-161119071.png)

​	三个区域通过指针readIndex和writeIndex划分，当向缓冲区内写入数据时，writeIndex指针会向后移动，从缓冲区读出数据时，readIndex指针向后移动。故readable区域大小表示缓冲区内数据大小，writable区域大小表示缓冲区剩余容量。注意：**这里的readable和writable是从外部程序的角度来看，外部程序读缓冲区中的数据，故readable；写入缓冲区，故writable**。而prependable是为了能以较小的代价在前部添加一部分数据。

​	**WriteFd函数：**将数据从缓冲区（readable区）读出。

​	**ReadFd函数：**将外来数据写入缓冲区（writable区），先在栈上开辟一个65536字节的char数组，利用readv()分散读来读取数据，若writable足够则直接都读取到writable区，超过则会读到栈上的char数组，随后再Append到Buffer中，避免了开大Buffer的性能浪费。（与直接调用Append扩容有何区别?）

```cpp
ssize_t Buffer::ReadFd(int fd, int* saveErron){
    char buff[65535];     // 在栈上开辟65536的空间
    /* iovec 是一个结构体 
        *iov_base记录buffer地址
        iov_len表示buffer大小 */
    struct iovec iov[2];
    const size_t writable = WritableBytes();
    // 分散读，保证数据全部读完
    iov[0].iov_base = BeginPtr_() + writePos_;
    iov[0].iov_len = writable;
    iov[1].iov_base = buff;
    iov[1].iov_len = sizeof(buff);

    // 将数据从fd读到分散的内存块中，即分散读
    const ssize_t len = readv(fd, iov, 2);
    if (len < 0){
        *saveErron = errno;
    }else if(static_cast<size_t>(len) <= writable){
        writePos_ += len;   // 内存充足 改变下标位置
    }else{
        writePos_ = buffer_.size();
        Append(buff, len - writable);
    }
    return len;
}
```

### 2.日志

​	使用阻塞队列与单例模式实现的异步日志系统。

​	参考文章：

​	https://mp.weixin.qq.com/s/IWAlPzVDkR2ZRI5iirEfCg

​	https://mp.weixin.qq.com/s/f-ujwFyCe1LZa3EB561ehA

**2.1单例模式：**

​	保证一个类只有一个实例，提供一个访问的全局访问点以获取实例，该实例被所有程序模块所共享。要实现此需要私有化构造与析构函数，防止外界创建单例外的对象；使用公有的静态方法获取实例；使用类的私有静态指针变量指向类的唯一实例。

​	有两种实现方法，**懒汉模式**和**饿汉模式**。

- **懒汉模式：**在第一次调用时才进行初始化，**在C++11后，使用局部静态变量法可以保证线程安全**。
- **饿汉模式：**在程序运行时即初始化，是线程安全的，但由于静态成员在不同编译单元的初始化顺序是不确定的，若在实例被初始化之前调用getInstance方法会返回未定义的实例。

**2.2阻塞队列：**

​	使用生产者消费者模型实现，容量为0时表示不使用阻塞队列，为同步日志。注意锁的使用，关闭时需要唤醒所有阻塞的生产者消费者线程。

```cpp
template<typename T>
void BlockDeque<T>::Close(){
    // 操作前上锁 清理队列中所有成员 唤醒所有阻塞中的生产者消费者线程
    {
        std::lock_guard<std::mutex> locker(mtx_);
        deq_.clear();
        isClose_ = true;
    }
    condProducer_.notify_all();
    condConsumer_.notify_all();
}
```

**2.3日志系统：**

​	日志系统分为同步日志和异步日志，当阻塞队列大小为0时为同步日志，大于0则为异步日志。首先使用单例模式获得实例；调用init()函数根据参数初始化日志系统；通过调用writeLog()函数写入日志，根据当前时间创建日志，再在buffer内生成日志消息(使用缓冲区减少直接访问设备的次数)，若同步则直接将缓冲区内容写入文件，异步则写入阻塞队列，等待写线程读取。

![image-20231129104353090](https://img2023.cnblogs.com/blog/3304168/202311/3304168-20231129104353549-1143362960.png)

- **同步日志：**日志写入与工作线程串行执行，由于日志存在IO操作，可能会阻塞整个流程，导致效率严重下降。
- **异步日志：**将日志信息写入阻塞队列，创建写线程从阻塞队列中读取内容写入日志。

​	不同的日志等级：

- **Debug：**调试代码时的输出，在系统实际运行时，一般不使用。
- **Warn：**这种警告与调试时终端的warning类似，同样是调试代码时使用。
- **Info：**报告系统当前的状态，当前执行的流程或接收的信息等。
- **Error：**输出系统的错误信息

​	使用到了可变参数宏，具体使用方法如下：

```cpp
write(int level, const char* format, ...){
    ...
	va_list vaList;
    va_start(vaList, format);   // 初始化变长参数列表
    int m = vsnprintf(buff_.BeginWrite(), buff_.WritableBytes(), format, vaList);
    va_end(vaList);             // 结束变长参数列表的访问
    ...
}

// 可变参数宏定义
#define LOG_BASE(level, format, ...) \
    do {\
        Log* log = Log::Instance();\
        if (log->IsOpen() && log->GetLevel() <= level) {\
            log->write(level, format, ##__VA_ARGS__); \
            log->flush();\
        }\
    } while(0);
#define LOG_DEBUG(format, ...) do {LOG_BASE(0, format, ##__VA_ARGS__)} while(0);
```

### 3.线程池与数据库连接池

**3.1为什么要用到线程池：**

​	限制应用中同时运行的线程数，减少线程创建销毁的效率占用。项目启动时创建好固定大小的线程池，当客户端请求连接时分配一个线程池中的线程，处理完连接后将线程放回池中，无需动态分配销毁。

**3.2线程池实现：**

​	线程池内是封装了互斥锁、条件变量、关闭状态、任务队列的一个结构体，通过共享指针管理。在构造函数中使用make_shared<Poll>()创建一个共享指针保证线程池中的数据结构在整个生命周期内都是有效的，创建多个线程，并使用**detach()**将子线程独立于主线程在后台继续执行，子线程中while(true)不断读取并执行线程队列中的任务（当没有任务来的时候会被条件变量等待在while循环中）。当服务器使用AddTask()向线程池中的任务队列（**注意：一个线程池中只有一个任务队列，但有多个线程，线程之间竞争任务的处理权**）添加任务，子线程被唤醒处理任务，每个线程执行任务前都会先解锁以提高并发性能。

​	下面给出类的全部代码。

```cpp
class ThreadPool{
public:
    explicit ThreadPool(size_t threadCount = 8): pool_(std::make_shared<Pool>()){
        assert(threadCount > 0);
        for(size_t i = 0; i < threadCount; i++){
            std::thread([pool = pool_] {	// 使用匿名函数
                std::unique_lock<std::mutex> locker(pool->mtx);
                while(true){
                    if(!pool->tasks.empty()){
                        auto task = std::move(pool->tasks.front());
                        pool->tasks.pop();
                        locker.unlock();    // 任务已取出 可提前解锁
                        task();             // 执行任务
                        locker.lock();      // 马上又要取任务 上锁
                    }
                    else if(pool->isClosed) break;
                    else pool->cond.wait(locker);   // 等待 任务来了就notify
                }
            }).detach();
        }
    }

    ThreadPool() = default;
    ThreadPool(ThreadPool&&) = default;

    ~ThreadPool(){
        if(static_cast<bool>(pool_)){
            {
                std::lock_guard<std::mutex> locker(pool_->mtx);
                pool_->isClosed = true;
            }
            pool_->cond.notify_all();       // 唤醒所有进程 处理剩下任务
        }
    }

    template<typename T>
    void AddTask(T&& task){
        {
            std::unique_lock<std::mutex> locker(pool_->mtx);
            pool_->tasks.emplace(std::forward<T>(task));
        }
        pool_->cond.notify_all();
    }

private:
    struct Pool{
        std::mutex mtx;
        std::condition_variable cond;
        bool isClosed;
        std::queue<std::function<void()>> tasks;    // 任务队列
    };
    std::shared_ptr<Pool> pool_;        
};
```

**3.3数据库连接池：**

​	由于服务器需要频繁访问数据库，在程序初始化时集中创建并管理多个数据库来连接，如线程池般在程序服务器需要时分配连接，使用结束后回收链接。

​	使用**单例模式和队列**创建数据库连接池，并使用**RAII机制释放数据库连接**。单例模式介绍请看上文日志部分，此处使用与日志系统相同的**局部静态变量法实现的懒汉模式**。

```cpp
static SqlConnPool* Instance();	// 单例模式
...
std::queue<MYSQL *> connQue_;	// 队列实现连接池，在Init()中创建多个数据库连接加入队列
std::mutex mtx_;				// 互斥锁
sem_t semId_;					// 信号量
```

​	**RAII**（Resource Acquisition Is Initialization）资源获取即初始化。资源的有效期与持有资源的对象的生命期严格绑定，即由对象的构造函数完成资源的分配获取），同时由析构函数完成资源的释放。在这种要求下，只要对象能正确地析构，就不会出现资源泄漏问题。C++库中如容器、智能指针等都使用了RAII方法。

```cpp
SqlConnRAII(MYSQL** sql, SqlConnPool *connpool){	// 构造函数中初始化
    assert(connpool);
    *sql = connpool->GetConn();
    sql_ = *sql;
    connpool_ = connpool;
}

~SqlConnRAII(){										// 析构函数中释放
    if(sql_ )   
        connpool_->FreeConn(sql_);
}
```

​	注意：信号量使用与条件变量不同，信号量先使用semI_wait(&semId_)再上锁，而条件变量先上锁再使用条件变量wait()。

### 4. EPOLL & IO复用

**4.1 服务器模型：**

​	**C/S模型：**服务器启动后创建一个或多个socket，调用bind函数绑定到指定端口，然后调用listen函数等待客户链接。客服可使用connect函数连接服务器。由于客户的连接请求时随机到达的异步事件，故需要某种**I/O模型**来监听这一事件。监听到后调用accept函数接受，并分配一个逻辑单元(子线程)服务连接。在处理一个请求同时页需要监听其他客户请求。

**4.2 I/O模型：**

​	参考文章https://zhuanlan.zhihu.com/p/115912936

​	有五种I/O模型，分别是**阻塞IO**、**非阻塞IO**、**IO复用**、**信号驱动IO**、**异步IO**，对于socket流而言，网络上的数据分组到达，然后被复制到内核的缓冲区，再把数据从内核缓冲区复制到应用缓冲区。

- **阻塞IO：**当应用发出读取数据申请时，在内核数据还没准备好之前，该应用会一直处于等待状态，直到数据准备好了交给应用后才结束。

  ![image-20231129211050889](https://img2023.cnblogs.com/blog/3304168/202311/3304168-20231129211050749-1435838748.png)

- **非阻塞IO：**应用请求读取数据后，内核会立即告诉应用还没准备号，并返回一个错误码。应用可以继续执行，但需要不断执行系统调用来查询IO是否完成（轮询）。

  ![image-20231129211006338](https://img2023.cnblogs.com/blog/3304168/202311/3304168-20231129211006279-1109360718.png)

- **IO复用：**系统提供一种函数可以同时监控多个fd，如此只需少量线程便可以完成数据状态询问操作，当有数据准备好了再分配对应的线程去读数据。常用有**select、poll、epoll**三种函数。三者本质都是同步IO。

  - **select:**调用后select函数会阻塞，直到有描述符就绪（有数据 可读、可写、或者有except），或者超时（timeout指定等待时间，如果立即返回设为null即可），函数返回。当select函数返回后，可以 通过**遍历**fdset，来找到就绪的描述符。存在单个进程监听fd的最大限制（linux为1024）.
  - **poll:**与select类似，返回后也需要轮询pollfd来获取就绪的描述符，但无最大数量限制（数量过大性能会下降）。
  - **epoll:**
    1. select和poll都是一个函数，而epoll是一组函数。
    2. select通过线性表描述fd集合，poll通过链表，epoll通过红黑树。
    3. select和poll通过将所有fd拷贝到内核态，每次调用都需拷贝，epoll将要监听的fd注册到红黑树上。
    4. select和poll遍历fd集合，判断哪个fd上状态改变，epoll处理建立红黑树用于存储fd外，还会建立一个list，存储准备就绪的事件，epoll_wait调用时观察list种有无数据即可。
    5. epoll是根据每个fd上面的回调函数(中断函数)判断，只有发生了事件的socket才会主动的去调用 callback函数，其他空闲状态socket则不会，若是就绪事件，插入list。
    6. select和poll只返回发生了事件的fd的个数，要知道是哪个事件仍需遍历，而epoll返回发生事件的个数和结构体数组，包含socket的信息，直接处理结构体即可。
    7. select和poll只能工作在相对低效的LT模型，epoll在LT和ET模式都可以。
  - 当监测的fd数量小且活跃，使用select或poll；数量大且单位时间内只有一定数量的fd就绪，使用epoll。
  - **LT和ET**
    - **LT（电平触发）：**类似`select`，LT会去遍历在epoll事件表中每个文件描述符，来观察是否有我们感兴趣的事件发生，如果有（触发了该文件描述符上的回调函数），`epoll_wait`就会以非阻塞的方式返回。若该epoll事件没有被处理完（没有返回`EWOULDBLOCK`），该事件还会被后续的`epoll_wait`再次触发。
    - **ET（边缘触发）：**ET在发现有我们感兴趣的事件发生后，立即返回，并且`sleep`这一事件的`epoll_wait`，不管该事件有没有结束。在使用ET时必须保证fd是**非阻塞的**，并且每次调用`read`和`write`时必须等到返回EWOULDBLOCK。

  ![image-20231129211737101](https://img2023.cnblogs.com/blog/3304168/202311/3304168-20231129211737011-662170221.png)

- **信号驱动IO：**应用进程使用 sigaction 系统调用，内核立即返回，应用进程可以继续执行，即等待数据阶段应用进程是非阻塞的。内核在数据到达时向应用进程发送 SIGIO 信号，应用进程收到之后在信号处理程序中调用 recvfrom 将数据从内核复制到应用进程中。如此可以避免大量不必要的轮询查询数据状态。

  

  ![image-20231129214408151](https://img2023.cnblogs.com/blog/3304168/202311/3304168-20231129214408077-1430073680.png)

- **异步IO：**应用程序发送一个read请求，随后立即返回程序继续执行，不会阻塞，内核当数据准备就绪后会主动把数据复制到用户空间，所有操作完成后通知应用。

  ![image-20231129214419596](https://img2023.cnblogs.com/blog/3304168/202311/3304168-20231129214419392-1951343006.png)

​	

​	**阻塞式IO：**发送请求后数据还没准备就绪时等待数据就绪，阻塞IO、非阻塞IO、信号IO与IO复用都是此类型。

​	**非阻塞时IO：**发送请求后数据还没准备就绪时立即返回，异步IO如此。

​	**同步IO：**发送数据到最后完成都需要进程自己进行，阻塞IO、非阻塞IO、信号IO与IO复用都是此类型。

​	**异步IO：**应用发送完指令后就不再参与过程了，只需要等待最终完成结果的通知，异步IO如此。

**4.3 事件处理模式：**

​	服务器程序通常需要处理三类事件：I/O事件，信号及定时事件。有两种事件处理模式：

- **Reactor模式：**主线程（I/O处理单元）只负责监听文件描述符上是否有事件发生（读、写），若有则立即通知工作线程（逻辑单元），将socket可读可写事件放入请求队列，交给工作线程处理。
- **Proactor模式：**将所有IO操作交给主线程和内核处理，工作线程仅仅负责业务逻辑。

​	通常使用同步I/O模型（如`epoll_wait`）实现Reactor，使用异步I/O（如`aio_read`和`aio_write`）实现Proactor。但在此项目中，使用的是**Reactor**事件处理模式。

![image-20231130163348117](https://img2023.cnblogs.com/blog/3304168/202311/3304168-20231130163349164-264565660.png)

​							（同步IO模拟的Proactor）

**4.4 Epoll API：**

```cpp
#include <sys/epoll.h>

// 创建一个epoll实例
int epoll_create(int size);
/*
	size -- 无意义，必须大于0
   	返回值：成功返回一个非负数（文件描述符），失败返回-1，原因需查看error
*/


//对epoll实例进行管理：添加文件描述符信息，删除信息，修改信息
int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event);
/*
	epfd epoll实例对应的文件描述符
	op 操作 
		EPOLL_CTL_ADD	添加 
		EPOLL_CTL_MOD	修改
		EPOLL_CTL_DEL	删除
	fd 指定的文件描述符
	event 关联描述fd文件描述符的结构体
	返回值： 成功返回0，失败返回-1并设置errno
*/

typedef union epoll_data {
    void        *ptr;
    int          fd;
    uint32_t     u32;
    uint64_t     u64;
} epoll_data_t;

struct epoll_event {
    uint32_t     events;      /* Epoll events */
    epoll_data_t data;        /* User data variable */
};

// 等待epoll事件
int epoll_wait(int epfd, struct epoll_event* events, int maxevents, int timeout);
/*
	events 保存了发生变化的文件描述符的信息
	maxevents events数组的大小
	timeout 阻塞时间
		0 不阻塞、-1 阻塞、>0阻塞的时长（ms）
	返回值：成功返回变化的文件描述符个数，正数；失败返回-1
*/
```

**4.5 epoller类：**

​	对epoll进行简单的封装，易于调用。

```cpp
bool AddFd(int fd, uint32_t events);
bool ModFd(int fd, uint32_t enents);
bool DelFd(int fd);
int Wait(int timeoutMs = -1);

int epollFd_;
std::vector<struct epoll_event> events_;
```

**4.6 socket API**

socket通信流程：整个过程类似三次握手过程。

![image-20231202145316207](https://img2023.cnblogs.com/blog/3304168/202312/3304168-20231202145316454-358004937.png)

```cpp
// 创建socket
int socket(int domain, int type, int protocol);
	domain:协议族，AF_INET、AF_INET6
    type：套接字类型，SOCKET_STREAM、SOCK_DGRAM
    protocol：协议类型，TCP或UDP都可指定为0
    返回值：成功返回文件描述符，失败返回-1
```

```cpp
// 设置socket 此项选项很多 请查阅资料 介绍项目中用到的几个
// https://blog.csdn.net/A493203176/article/details/65438182
int setsockopt(int sockfd, int level, int option_name, const void* optval, socklen_t optlen);
	level：被设置的选项级别，在套接字级别上设置需设为SOL_SOCKET
    option_name：准备设置的选项
    option_value：指向包含新选项值的缓冲
    optlen：选项的长度
        
// SO_LINGER 设置从容关闭
struct linger optLinger = {0};              // 用于设置tcp断开链接时的断开方式
    if(openLinger_) {
        // 优雅关闭 发送完剩余数据或超时后关闭
        optLinger.l_onoff = 1;
        optLinger.l_linger = 1;
    }
ret = setsockopt(listenFd_, SOL_SOCKET, SO_LINGER, &optLinger, sizeof(optLinger));

// 设置端口复用 closesocket后可以重用该socket 
int optval = 1;
ret = setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, (const void*)&optval, sizeof(int));
```



```cpp
// 绑定socket和端口号
// 服务器端需要在连接前绑定，而客户端不用，因为服务器有一个指定的固定地址+端口，而用户是在connect时系统自动分配的
int bind(int sockfd, const struct sockaddr* addr, socklen_t addrlen);
	sockfd:socket创建的套接字
	addr：绑定给socket的地址，sockaddr_in socketaddr_in6
	addrlen: 地址的长度
```

```cpp
// 监听连接
int listen(int sockfd, int backlog);
	backlog：挂起的连接队列的长度，linux默认128
```

 ```cpp
// 客户端向服务器发起连接
int connect(int sockfd, const struct sockaddr* addr, socklen_t addrlen);
	sockfd：客户端的socketfd
    addr：服务器socket的地址
    返回值：成功返回0，失败-1
 ```

```cpp
// 接受客户端的连接请求
int accept(int sockfd, struct sockaddr* addr, socklen_t addrlen);
	sockfd:服务器的socketfd
	addr：客户端的socket地址	
    返回值：成功返回客户端fd，失败返回-1
```

```cpp
// 从fd读数据，socket默认阻塞，对方没有写数据的话会一直阻塞
ssize_t read(int fd, void* buf, size_t count);
// 向fd写数据，也就是发送内容
ssize_t write(int fd, void* buf, size_t count);
```

```cpp
// 关闭socket
int close(int fd);
	socket标记为关闭，计数器-1，为0时发送终止连接请求。
```

### 5.HTTP连接

**5.1 request**

​	客户端向服务器发送request请求消息以获取资源，请求消息形式不再过多介绍。

![image-20231130194110658](https://img2023.cnblogs.com/blog/3304168/202311/3304168-20231130194110553-1266154918.png)

​	使用有限状态机解析request请求，先从buffer中逐行读取数据，并根据当前状态进行处理和状态转换，直到解析完请求数据或缓冲区中无数据可读时为FINISH状态完成解析。

```cpp
bool HttpRequest::parse(Buffer& buff) {
    const char CRLF[] = "\r\n";
    if(buff.ReadableBytes() <= 0) {
        return false;
    }
    while(buff.ReadableBytes() && state_ != FINISH) {
        const char* lineEnd = search(buff.Peek(), buff.BeginWriteConst(), CRLF, CRLF + 2);	// 逐行读取请求消息
        std::string line(buff.Peek(), lineEnd);
        switch(state_)
        {
        case REQUEST_LINE:		// 解析请求行
            if(!ParseRequestLine_(line)) {
                return false;
            }
            ParsePath_();
            break;    
        case HEADERS:			// 解析请求头
            ParseHeader_(line);
            if(buff.ReadableBytes() <= 2) {
                state_ = FINISH;
            }
            break;
        case BODY:				// 解析请求体（POST）
            ParseBody_(line);
            break;
        default:
            break;
        }
        if(lineEnd == buff.BeginWrite()) { break; }
        buff.RetrieveUntil(lineEnd + 2);
    }
    LOG_DEBUG("[%s], [%s], [%s]", method_.c_str(), path_.c_str(), version_.c_str());
    return true;
}
```

​	使用正则表达式匹配信息。如果是post请求（在这里是注册和登录提交的表单），通过UserVerify函数调用数据库来验证用户登录注册并向数据库查询或写入数据。

```cpp
regex patten("^([^ ]*) ([^ ]*) HTTP/([^ ]*)$");		// 匹配请求行
regex patten("^([^:]*): ?(.*)$");					// 匹配请求头
```

**5.2 response：**

​	服务器构造相应消息后发送给客户端。

![image-20231130202402533](https://img2023.cnblogs.com/blog/3304168/202311/3304168-20231130202402310-2042755309.png)

​	通过MakeResponse(Buffer& buff)函数判断请求文件的类型、访问权限，再调用AddLine、AddHeader、AddContent来生成完整的相应消息（向缓冲区内写入数据）。在生成响应体时可以通过建立文件映射（映射到请求的文件）来提高文件的访问速度。

```cpp
// 生成响应体
void HttpResponse::AddContent_(Buffer& buff) {
    int srcFd = open((srcDir_ + path_).data(), O_RDONLY);
    if(srcFd < 0) { 
        ErrorContent(buff, "File NotFound!");
        return; 
    }

    /* 将文件映射到内存提高文件的访问速度 
        MAP_PRIVATE 建立一个写入时拷贝的私有映射*/
    LOG_DEBUG("file path %s", (srcDir_ + path_).data());
    int* mmRet = (int*)mmap(0, mmFileStat_.st_size, PROT_READ, MAP_PRIVATE, srcFd, 0);
    if(*mmRet == -1) {
        ErrorContent(buff, "File NotFound!");
        return; 
    }
    mmFile_ = (char*)mmRet;
    close(srcFd);
    buff.Append("Content-length: " + to_string(mmFileStat_.st_size) + "\r\n\r\n");
}
```

**mmap：**将一个文件或其他对象映射到内存，提高文件的访问速度。

```cpp
void* mmap(void* start, size_t length, int prot, int flags, int fd, off_t offset);
	start: 映射区开始地址
	length: 映射区长度
	prot：期望的内存保护标志PROT_READ表示可以被读取
	flags：指定映射对象的类型 MAP_RPIVATE建立一个写入时拷贝的私有映射，不会影响源文件
	fd：文件描述符
	offset 被映射对象内容的起点 
int munmap(void* start, size_t length);
```

**stat:**用于取得指定文件的文件属性，存到stat结构体中

```cpp
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

// 获取文件属性，存在statbuf中
int stat(const cahr* pathname, struct stat* statbuf);

// 部分属性
struct stat{
	mode_t st_mode;	// 文件类型和权限
	off_t  st_size; // 文件大小，字节数
}
```

**5.3 HttpConn：**

​	`读缓冲区`用于读取http请求消息，`写缓冲区`用于存放生成的响应消息，`request_`用于在process()中将请求消息放入读缓冲区，`response_`用于放入写缓冲区。`sockaddr_in` 用于处理网络通信的地址，`iovec`用于分散读和聚集写。

​	process()调用httprequest和httpresponse的方法解析请求消息并生成相应消息，放在buff中。

​	提供``write``和``read``函数，read用于将消息放入读缓冲区，write用于向浏览器的fd写入写缓冲区中的消息，注意此时的读写都是在服务器的角度来说。

**调用过程：**

1. 调用client->read(&writeErrno)将消息读入读缓冲区(调用readBuff_ReadFd(fd_, saveErrno));注意此时要区别触发方式是ET还是LT，ET要一次性全部读出。
2. 调用process()
   - request_Init()
   - request_parse(readBuff_)
   - respone_.MakeResponse(writeBuff\_);
   - iov\_[0]代表响应报文，iov\_[1]代表请求映射的文件
3. 调用wirte向浏览器连接的fd中写入writeBuff\_中的响应消息。

**Q：**为什么要使用分散写而非直接从写缓冲区将数据写入浏览器？

A：生成响应消息时并未将消息体放入缓冲区（文件可能太大），而是建立了一个文件的映射，在write中分散写一部分从buff中写入，一部分映射的文件写入。

**Q：**为什么write是向fd中写入？

A：一个socket的句柄可以看作一个文件，在socket上收发数据相当于对文件进行读写操作。

```cpp
ssize_t HttpConn::write(int* saveErrno){
    ssize_t len = -1;
    do {
        len = writev(fd_, iov_, iovCnt_);        // 将iov的内容写到fd中
        if(len <= 0){
            *saveErrno = errno;
            break;
        }
        if(iov_[0].iov_len + iov_[1].iov_len == 0) break;       // 传输结束
        // 由于writev不会对成员做任何处理 需要手动处理了iov中的指针和长度
        else if(static_cast<size_t>(len) > iov_[0].iov_len){    // 读取的数据长度大于写缓冲区的长度 说明也从响应消息的文件里读取了
            iov_[1].iov_base = (uint8_t*)iov_[1].iov_base + (len - iov_[0].iov_len);    // 更新iov_[1]
            iov_[1].iov_len -= (len - iov_[0].iov_len);
            if(iov_[0].iov_len){            // 写缓冲区还有数据
                writeBuff_.RetrieveAll();
                iov_[0].iov_len = 0;
            }
        }else{                                                  // 只从缓冲区内读取了
            iov_[0].iov_base = (uint8_t*)iov_[0].iov_base + len;
            iov_[0].iov_len -= len;
            writeBuff_.Retrieve(len);
        }
    }while(isET || ToWriteBytes() > 10240);
    return len;
}
```

### 6. 定时器

​	用以处理定时事件，关闭长时间不动的连接。将事件封装成定时器，并用容器保存所有定时器，统一管理。时间堆的做法则是将所有定时器中超时时间最小的一个定时器的超时值作为心搏间隔，当超时时间到达时，处理超时事件，然后再次从剩余定时器中找出超时时间最小的一个，依次反复即可。使用**小根堆**实现。

```cpp
typedef std::function<void()> TimeoutCallBack;      // 回调函数
typedef std::chrono::high_resolution_clock Clock;   // now()获取当前时间
typedef std::chrono::milliseconds MS;               // 表示毫秒
typedef Clock::time_point TimeStamp;                // 时间点

// 定时器节点
struct TimerNode {
    int id;             // 标记定时器
    TimeStamp expires;  // 设置过期时间
    TimeoutCallBack cb; // 回调函数
    bool operator<(const TimerNode& t){
        return expires < t.expires;
    }
};
```

function用法请见：https://blog.csdn.net/wangshubo1989/article/details/49134235

chrono用法请见：https://zhuanlan.zhihu.com/p/373392670

小根堆实现较为简单，从略。

### 7. Server

​	整合了以上所有功能，封装到一个Server类中，通过初始化Server类和调用Start()方法启动服务器。

**7.1初始化**

1. 获取并添加工作目录，路径用于初始化HttpConn的srcDir -> 用于HttpResponse的srcDir -> 用于映射文件，构造相应消息体。
2. 初始化数据库，从SqlConnPoll::Instance()获取实例，并调用Init()初始化。
3. 初始化触发方式，listenfd和connfd的ET和LT组合。
4. 初始化Socket。详见4.6。除了初始化socket，还要把listen返回的fd添加到Epoll，以及设置文件为非阻塞。
5. 若启用日志，获取日志实例并初始化，向日志写入初始化的基本信息。

**设置文件为非阻塞**

​	使用文件控制函数fcntl，详细请见https://www.cnblogs.com/xuyh/p/3273082.html

```cpp
#include <unistd.h>
#include <fcntl.h>

int fcntl(int fd, int cmd);
int fcntl(int fd, int cmd, long arg);         
int fcntl(int fd, int cmd, struct flock *lock);

// 设置文件为非阻塞
int WebServer::SetFdNonblock(int fd) {
    assert(fd > 0);
    return fcntl(fd, F_SETFL, fcntl(fd, F_GETFD, 0) |  O_NONBLOCK);
}
F_SETFL:设置给arg描述符的状态标志，O_NONBLOCK表示非阻塞IO，如果read调用没有可读取的数据或write操作将要阻塞，则返回-1和EAGAIN错误。
F_GETFD:返回文件描述符标志（arg被忽略）
```

**7.2 Start**

​	服务器！启动！！

1. 设置epoll等待时间为-1（无事件到达将阻塞）并进入主事件循环。

2. 通过时间堆timer\_->getNextTick()清除超时节点并获取下一次等待超时等待时间。

3. 调用epoll_wait(timeMS)，记录事件数量。

4. 通过epoller获取事件的文件描述符和具体类型

5. 根据文件类型来调用相关方法处理事件。

   - **DealListen：**accept连接，调用AddClient(fd, addr)添加客户端连接，初始化HttpConn、在时间堆中加入连接（绑定事件为关闭连接）并在epoller中加入连接（fd, EPOLLIN | connEvent_），最后设置socket为非阻塞（使用ET必须保证文件为非阻塞）。
   - **CloseConn：**在epoller中删除事件，并关闭客户端连接。
   - **DealRead：**将OnRead加入线程池的任务队列``threadpool_->AddTask(std::bind(&WebServer::OnRead_, this, client));``在OnRead中，先用HttpConn::Read()将信息从socket中读到读缓冲区，在调用HttpConn::Process()进行逻辑处理，并根据返回值设置事件状态，成功修改监听事件为可写，等待OnWrite发送，失败则仍为可读。

   - **DealWrite：**将OnWrite加入线程池的任务队列（方式同上），OnWrite中，先用HttpConn::Write()将数据发送到socket，如果传输完成且连接未关闭则修改状态监听读事件，如果传输失败是因为缓冲区满则修改状态继续监听读事件，等待继续传输。若传输出现错误关闭连接。

```cpp
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
```


