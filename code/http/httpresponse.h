#ifndef HTTP_RESPONSE_H
#define HTTP_RESPONSE_H

#include <unordered_map>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>

#include "../buffer/buffer.h"
#include "../log/log.h"

class HttpResponse {
public:
    HttpResponse();
    ~HttpResponse();

    void Init(const std::string& srcDir, std::string& path, bool isKeepAlive = false, int code = -1);
    void MakeResponse(Buffer& buff);                        // 生成状态码 调用Add创建响应消息
    void UnmapFile();                                       // 取消内存映射
    char* File();                                           // 返回内存映射
    size_t FileLen() const;                                 // 文件长度
    void ErrorContent(Buffer& buff, std::string message);   // 错误时页面
    int Code() const { return code_; }

private:
    void AddStateLine_(Buffer& buff);                       // 添加响应行
    void AddHeader_(Buffer& buff);                          // 添加响应头
    void AddContent_(Buffer& buff);                         // 添加响应体

    void ErrorHtml_();                                      // 定向到错误页面
    std::string GetFileType_();                             // 判断文件类型

    int code_;
    bool isKeepAlive_;

    std::string path_;
    std::string srcDir_;

    char* mmFile_;                  // 内存映射
    struct stat mmFileStat_;        // 文件属性结构体 用于文件操作

    static const std::unordered_map<std::string, std::string> SUFFIX_TYPE;      // 后缀类型集
    static const std::unordered_map<int ,std::string> CODE_STATUS;              // 状态类型集
    static const std::unordered_map<int ,std::string> CODE_PATH;                // 状态路径集
};

# endif