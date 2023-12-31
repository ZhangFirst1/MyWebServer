#ifndef HTTPREQUEST_H
#define HTTPREQUEST_H

#include <unordered_map>
#include <unordered_set>
#include <string>
#include <regex>
#include <error.h>
#include <mysql/mysql.h>

#include "../buffer/buffer.h"
#include "../log/log.h"
#include "../pool/sqlconnpool.h"
#include "../pool/sqlconnRAII.h"

class HttpRequest{
public:
    enum PARSE_STATE{
        REQUEST_LINE,
        HEADERS,
        BODY,
        FINISH,
    };

     enum HTTP_CODE {
        NO_REQUEST = 0,
        GET_REQUEST,
        BAD_REQUEST,
        NO_RESOURSE,
        FORBIDDENT_REQUEST,
        FILE_REQUEST,
        INTERNAL_ERROR,
        CLOSED_CONNECTION,
    };

    HttpRequest() { Init(); }
    ~HttpRequest() = default;

    void Init();
    bool parse(Buffer& buff);

    std::string path() const;
    std::string& path();
    std::string method() const;
    std::string version() const;
    std::string GetPost(const std::string& key) const;
    std::string GetPost(const char* key) const;

    bool IsKeepAlive() const;                   // 判断链接是否存在

private:
    bool ParseRequestLine_(const std::string& line);    // 处理请求行
    void ParseHeader_(const std::string& line);         // 处理请求头
    void ParseBody_(const std::string& line);           // 处理请求体

    void ParsePath_();                                  // 匹配路径
    void ParsePost_();                                  // 处理Post事件
    void ParseFromUrlencoded_();                        // 从rul解析编码

    static bool UserVerify(const std::string& name, const std::string& pwd, bool isLogin);  // 用户验证

    PARSE_STATE state_;                                     // 状态
    std::string method_, path_, version_, body_;            // 请求行的 请求方式、资源路径、HTTP版本 
    std::unordered_map<std::string, std::string> header_;   // 请求头
    std::unordered_map<std::string, std::string> post_;
    
    static const std::unordered_set<std::string> DEFAULT_HTML;
    static const std::unordered_map<std::string, int> DEFAULT_HTML_TAG;
    static int ConverHex(char ch);      // 16 -> 10 
};



#endif