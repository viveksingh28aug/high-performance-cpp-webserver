#include "http_request.h"
#include <sstream>
#include <algorithm>
#include <cctype>

HttpRequest HttpRequest::parse(const std::string& raw_request) {
    HttpRequest request;
    
    if (raw_request.empty()) {
        return request;
    }
    
    // Locate boundary between HTTP header block and payload body
    size_t header_end = raw_request.find("\r\n\r\n");
    size_t header_delim_len = 4;
    if (header_end == std::string::npos) {
        header_end = raw_request.find("\n\n");
        header_delim_len = 2;
    }
    
    std::string header_block;
    if (header_end != std::string::npos) {
        header_block = raw_request.substr(0, header_end);
        request.body_ = raw_request.substr(header_end + header_delim_len);
    } else {
        header_block = raw_request;
    }
    
    std::istringstream stream(header_block);
    std::string line;
    bool first_line = true;
    
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        
        if (first_line) {
            request.parse_request_line(line);
            first_line = false;
        } else {
            request.parse_header_line(line);
        }
    }
    
    // If Content-Length is specified, enforce body sizing
    if (request.has_header("content-length")) {
        try {
            size_t content_length = std::stoull(request.get_header("content-length"));
            if (request.body_.size() > content_length) {
                request.body_.resize(content_length);
            }
        } catch (const std::exception&) {
            // Invalid content-length
        }
    }
    
    request.parse_query_string();
    request.valid_ = request.is_request_valid();
    
    return request;
}

void HttpRequest::parse_request_line(const std::string& line) {
    std::istringstream stream(line);
    std::string method_str, path_with_query, version;
    
    if (!(stream >> method_str >> path_with_query >> version)) {
        return;
    }
    
    method_ = string_to_method(method_str);
    version_ = version;
    
    // Split path and query string
    size_t query_pos = path_with_query.find('?');
    if (query_pos != std::string::npos) {
        path_ = path_with_query.substr(0, query_pos);
        query_string_ = path_with_query.substr(query_pos + 1);
    } else {
        path_ = path_with_query;
    }
    
    // URL decode the path safely
    path_ = url_decode(path_);
}

void HttpRequest::parse_header_line(const std::string& line) {
    size_t colon_pos = line.find(':');
    if (colon_pos == std::string::npos) {
        return;
    }
    
    std::string name = line.substr(0, colon_pos);
    std::string value = line.substr(colon_pos + 1);
    
    // Trim leading and trailing whitespace
    name.erase(name.find_last_not_of(" \t") + 1);
    name.erase(0, name.find_first_not_of(" \t"));
    
    size_t val_first = value.find_first_not_of(" \t");
    if (val_first != std::string::npos) {
        size_t val_last = value.find_last_not_of(" \t");
        value = value.substr(val_first, val_last - val_first + 1);
    } else {
        value.clear();
    }
    
    // Convert header name to lowercase for case-insensitive lookup
    std::transform(name.begin(), name.end(), name.begin(), ::tolower);
    
    headers_[name] = value;
}

void HttpRequest::parse_query_string() {
    if (query_string_.empty()) {
        return;
    }
    
    std::istringstream stream(query_string_);
    std::string pair;
    
    while (std::getline(stream, pair, '&')) {
        size_t eq_pos = pair.find('=');
        if (eq_pos != std::string::npos) {
            std::string key = url_decode(pair.substr(0, eq_pos));
            std::string value = url_decode(pair.substr(eq_pos + 1));
            query_params_[key] = value;
        } else {
            query_params_[url_decode(pair)] = "";
        }
    }
}

std::string HttpRequest::url_decode(const std::string& str) {
    std::string result;
    result.reserve(str.length());
    
    for (size_t i = 0; i < str.length(); ++i) {
        if (str[i] == '%' && i + 2 < str.length()) {
            char hex1 = str[i + 1];
            char hex2 = str[i + 2];
            if (std::isxdigit(hex1) && std::isxdigit(hex2)) {
                int h1 = (hex1 >= 'a') ? (hex1 - 'a' + 10) : ((hex1 >= 'A') ? (hex1 - 'A' + 10) : (hex1 - '0'));
                int h2 = (hex2 >= 'a') ? (hex2 - 'a' + 10) : ((hex2 >= 'A') ? (hex2 - 'A' + 10) : (hex2 - '0'));
                result += static_cast<char>((h1 << 4) | h2);
                i += 2;
            } else {
                result += str[i];
            }
        } else if (str[i] == '+') {
            result += ' ';
        } else {
            result += str[i];
        }
    }
    
    return result;
}

std::string HttpRequest::get_header(const std::string& name) const {
    std::string lower_name = name;
    std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::tolower);
    
    auto it = headers_.find(lower_name);
    return (it != headers_.end()) ? it->second : "";
}

bool HttpRequest::has_header(const std::string& name) const {
    std::string lower_name = name;
    std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::tolower);
    
    return headers_.find(lower_name) != headers_.end();
}

std::string HttpRequest::get_query_param(const std::string& name) const {
    auto it = query_params_.find(name);
    return (it != query_params_.end()) ? it->second : "";
}

bool HttpRequest::is_keep_alive() const {
    std::string connection = get_header("connection");
    std::transform(connection.begin(), connection.end(), connection.begin(), ::tolower);
    
    if (version_ == "HTTP/1.1") {
        return connection != "close";
    } else {
        return connection == "keep-alive";
    }
}

std::string HttpRequest::method_to_string(HttpMethod method) {
    switch (method) {
        case HttpMethod::GET:     return "GET";
        case HttpMethod::POST:    return "POST";
        case HttpMethod::PUT:     return "PUT";
        case HttpMethod::DELETE:  return "DELETE";
        case HttpMethod::HEAD:    return "HEAD";
        case HttpMethod::OPTIONS: return "OPTIONS";
        default:                  return "UNKNOWN";
    }
}

HttpMethod HttpRequest::string_to_method(const std::string& method_str) {
    if (method_str == "GET")     return HttpMethod::GET;
    if (method_str == "POST")    return HttpMethod::POST;
    if (method_str == "PUT")     return HttpMethod::PUT;
    if (method_str == "DELETE")  return HttpMethod::DELETE;
    if (method_str == "HEAD")    return HttpMethod::HEAD;
    if (method_str == "OPTIONS") return HttpMethod::OPTIONS;
    
    return HttpMethod::UNKNOWN;
}

bool HttpRequest::is_request_valid() const {
    if (method_ == HttpMethod::UNKNOWN) {
        return false;
    }
    
    if (path_.empty() || path_[0] != '/') {
        return false;
    }
    
    if (version_ != "HTTP/1.0" && version_ != "HTTP/1.1") {
        return false;
    }
    
    // RFC 7230 §5.4: HTTP/1.1 requests MUST include a Host header field
    if (version_ == "HTTP/1.1" && !has_header("host")) {
        return false;
    }
    
    for (char c : path_) {
        if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) == 0x7F) {
            return false;
        }
    }
    
    if (method_ == HttpMethod::POST || method_ == HttpMethod::PUT) {
        auto content_length_header = get_header("content-length");
        if (!content_length_header.empty()) {
            try {
                size_t content_length = std::stoull(content_length_header);
                if (body_.size() < content_length) {
                    return false;
                }
            } catch (const std::exception&) {
                return false;
            }
        }
    }
    
    return true;
}
