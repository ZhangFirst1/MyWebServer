#include "buffer.h"

Buffer::Buffer(int initBuffsize) : buffer_(initBuffsize), readPos_(0), writePos_(0) {}

size_t Buffer::PrependableByters() const{
    return readPos_;
}

size_t Buffer::ReadableBytes() const{
    return writePos_ - readPos_;
}

size_t Buffer::WritableBytes() const{
    return buffer_.size() - writePos_;
}

const char* Buffer::Peek() const{
    return BeginPtr_() + readPos_;
}

void Buffer::EnsureWriteable(size_t len){
    // 如果写出缓冲区的空间小于需要分配的空间则调用MakeSpace扩容
    if(WritableBytes() < len){
        MakeSpace_(len);
    }
    assert(WritableBytes() >= len); // 写出缓冲区仍然 < len则触发断言  
}

void Buffer::HasWritten(size_t len){
    writePos_ += len;
}

void Buffer::Retrieve(size_t len){
    readPos_ += len;
}

void Buffer::RetrieveUntil(const char* end){
    assert(Peek() <= end);  // 开始地址 大于 end则断言
    Retrieve(end - Peek());
}

void Buffer::RetrieveAll(){
    // 将buffer清零
    bzero(&buffer_[0], buffer_.size());
    readPos_ = 0;
    writePos_ = 0;
}

std::string Buffer::RetrieveAllToStr() {
    std::string str(Peek(), ReadableBytes());
    RetrieveAll();
    return str;
}

char* Buffer::BeginWrite() {
    return BeginPtr_() + writePos_;
}

const char* Buffer::BeginWriteConst() const{
    return BeginPtr_() + writePos_;
}

void Buffer::Append(const std::string& str){
    Append(str.data(), str.length());
}

void Buffer::Append(const void* data, size_t len){
    assert(data);
    Append(static_cast<const char*>(data), len);
}

void Buffer::Append(const char* str, size_t len){
    assert(str);
    EnsureWriteable(len);                       // 扩容
    std::copy(str, str + len, BeginWrite());    // 复制到vector
    HasWritten(len);                            // 改变writePos_下标
}

void Buffer::Append(const Buffer& buff){
    Append(buff.Peek(), buff.ReadableBytes());
}

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

ssize_t Buffer::WriteFd(int fd, int* saveErrno){
    size_t readSize = ReadableBytes();
    ssize_t len = write(fd, Peek(), readSize);
    if (len < 0){
        *saveErrno = errno;
        return len;
    }
    readPos_ += len;
    return len;
}

char* Buffer::BeginPtr_() {
    // 返回的是vector值的地址
    return &*buffer_.begin();
}

const char* Buffer::BeginPtr_() const{
    return &*buffer_.begin();
}

// 小于总容量则整体移动到最前面 大于则resize vector
void Buffer::MakeSpace_(size_t len){
    if (WritableBytes() + PrependableByters() < len){
        buffer_.resize(writePos_ + len + 1);
    }else{
        // read区域整体复制到前面区域
        size_t readable = ReadableBytes();
        std::copy(BeginPtr_() + readPos_, BeginPtr_() + writePos_, BeginPtr_());
        readPos_ = 0;
        writePos_ = readPos_ + readable;
        assert(readable == ReadableBytes());
    }
}