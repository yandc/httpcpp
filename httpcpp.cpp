#include <errno.h>
#include <fcntl.h>
#include <regex.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <sys/socket.h>

#include <sstream>
#include <iostream>
#include <stdexcept>

#include "httpcpp.h"

// HttpRequest

HttpRequest* HttpRequest::from_sequence(const string& sequence) {
    size_t p0 = sequence.find("\r\n\r\n");
    if (p0 != string::npos) {
        p0 += 4;
        size_t p1 = sequence.find(" ");
        string method = sequence.substr(0, p1);
        if (method.compare("GET") == 0) {
            size_t p2 = sequence.find(" ", ++p1);
            string path = sequence.substr(p1, p2 - p1);
            return new HttpRequest(method, path);
        } else if (method.compare("POST") == 0) {
            size_t p3 = sequence.find("Content-Length:") + 15; 
            size_t p4 = sequence.find("\r\n", p3);
            int length = atoi(sequence.substr(p3, p4 - p3).data());
            if (sequence.size() >= p0 + length) {
                size_t p2 = sequence.find(" ", ++p1);
                string path = sequence.substr(p1, p2 - p1);
                string body = sequence.substr(p0, length);
                return new HttpRequest(method, path, body);
            }   
        } 
    }   
    return NULL;
}

HttpRequest::HttpRequest(const string& method, const string& path, 
                         const string& body) {
    this->method = method;
    this->path = path;
    this->body = body;
}

const string& HttpRequest::get_method() {
    return this->method;
}

const string& HttpRequest::get_path() {
    return this->path;
}

const string& HttpRequest::get_body() {
    return this->body;
}

// HttpResponse

const string HttpResponse::to_sequence() {
    stringstream packet;
    if (this->code == 200) {
        packet << "HTTP/1.0 200 OK\r\n";
    } else if (this->code == 204) {
        packet << "HTTP/1.0 204 No Content\r\n";
    } else if (this->code == 404) {
        packet << "HTTP/1.0 404 Not Found\r\n";
    } else if (this->code == 405) {
        packet << "HTTP/1.0 405 Method Not Allowed\r\n";
    } else {
        packet << "HTTP/1.0 500 Internal Server Error\r\n";
    }
    packet << "Content-Length: " << this->body.size() << "\r\n\r\n";
    packet << body;
    return packet.str();
}

HttpResponse* HttpResponse::from_sequence(const string& sequence) {
    size_t p0 = sequence.find("\r\n\r\n");
    if (p0 != string::npos) {
        p0 += 4;
        size_t p1 = sequence.find("Content-Length:") + 15; 
        size_t p2 = sequence.find("\r\n", p1);
        int length = atoi(sequence.substr(p1, p2 - p1).data());
        if (sequence.size() >= p0 + length) {
            size_t p1 = sequence.find(" ");
            size_t p2 = sequence.find(" ", ++p1);
            int code = atoi(sequence.substr(p1, p2 - p1).data());
            string body = sequence.substr(p0, length);
            return new HttpResponse(code, body);
        }
    }
    return NULL;
}

HttpResponse::HttpResponse(const int& code, const string& body) {
    this->code = code;
    this->body = body;
}

const int& HttpResponse::get_code() {
    return this->code;
}

const string& HttpResponse::get_body() {
    return this->body;
}

// HttpRequestHandler

HttpResponse* HttpRequestHandler::get(HttpRequest* const request,
                                      const vector<string>& args) {
    return new HttpResponse(405);
}

HttpResponse* HttpRequestHandler::post(HttpRequest* const request,
                                       const vector<string>& args) {
    return new HttpResponse(405);
}

// AsyncHttpClient

void AsyncHttpClient::on_read(int& fd) {
    char buffer[BUFFER_SIZE];
    bool done = false;
    while (true) {
        ssize_t n = read(fd, buffer, BUFFER_SIZE);
        if (n > 0) {     
            this->read_buffers[fd].append(buffer);
        } else if (n == 0) {    
            // somehow always get n==0 but EAGAIN, no idea why
            HttpResponse* response = 
                HttpResponse::from_sequence(this->read_buffers[fd]);
            if (response != NULL) {
                this->handlers[fd]->on_receive(response);
                delete response;
                done = true;
                break;
            }
        } else {
            if (errno != EAGAIN) {
                done = true;
            } 
            break;
        }
    }
    if (done) {
        this->read_buffers.erase(fd);
        this->write_buffers.erase(fd);
        this->handlers.erase(fd);
        close(fd);
    }
}

void AsyncHttpClient::on_write(int& fd) {
    bool error = false;
    while (true) {
        size_t size = this->write_buffers[fd].size();
        ssize_t n = write(fd, this->write_buffers[fd].data(), size);
        if (n > 0) {
            this->write_buffers[fd] = this->write_buffers[fd].substr(n);
        } else {
            // somehow always get n==0 but EAGAIN, no idea why
            if (errno == EAGAIN) {

            } else {
                if (this->write_buffers[fd].size() == 0) {
                    this->loop->set_handler(fd, this, true);
                    this->read_buffers[fd] = string();
                } else {
                    error = true;
                }
            }
            break;
        }
    }
    if (error) {
        this->on_error(fd);
    }
}

void AsyncHttpClient::on_error(int& fd) {
    this->read_buffers.erase(fd);
    this->write_buffers.erase(fd);
    this->handlers.erase(fd);
    close(fd);
}

AsyncHttpClient::AsyncHttpClient(IOLoop* const loop) {
    // set the IO loop
    if (loop == NULL) {
        this->loop = IOLoop::instance();
    } else {
        this->loop = loop;
    }
}

void AsyncHttpClient::fetch(const string& host, const int& port, 
                            const string& method, const string& path, 
                            const string& body, 
                            HttpResponseHandler* const handler) {
    int fd;
    struct sockaddr_in addr;
    if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        throw runtime_error(strerror(errno));
    }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_aton(host.data(), &addr.sin_addr) <= 0) {
        throw runtime_error(strerror(errno));
    }
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        throw runtime_error(strerror(errno));
    }
    stringstream packet;
    packet << method << " " << path << " HTTP/1.0\r\n" <<
        "Content-Length: " << body.size() << "\r\n\r\n" << body;
    // set the write buffer and the handler
    this->write_buffers[fd] = packet.str();
    this->handlers[fd] = handler;
    this->loop->set_handler(fd, this, false);
}

// AsyncHttpServer

HttpRequestHandler* AsyncHttpServer::find_handler(const string& path) {
    vector<pair<string, HttpRequestHandler*> >::iterator it;
    for (it = this->handlers.begin(); it != this->handlers.end(); it++) {
        regex_t preg;
        if (regcomp(&preg, (*it).first.data(), REG_EXTENDED | REG_NOSUB) == 0) {
            if (regexec(&preg, path.data(), 0, NULL, 0) == 0) {
                regfree(&preg);
                return (*it).second;
            }
            regfree(&preg);
        }
    }
    return NULL;
}

vector<string> AsyncHttpServer::get_arguments(const string& path) {
    vector<string> args;
    vector<pair<string, HttpRequestHandler*> >::iterator it;
    for (it = this->handlers.begin(); it != this->handlers.end(); it++) {
        regex_t preg;
        if (regcomp(&preg, (*it).first.data(), REG_EXTENDED) == 0) {
            size_t nmatch = MAX_NMATCH;
            regmatch_t pmatch[nmatch];
            if (regexec(&preg, path.data(), nmatch, pmatch, 0) == 0) {
                for (int i = 1; i < nmatch; i++) {
                    if (pmatch[i].rm_so == -1) {
                        break;
                    }
                    int n = pmatch[i].rm_eo - pmatch[i].rm_so;
                    args.push_back(string(path.data() + pmatch[i].rm_so, n));
                }
                regfree(&preg);
                break;
            }
            regfree(&preg);
        }
    }
    return args;
}

void AsyncHttpServer::on_read(int& fd) {
    if (fd == this->fd) {   
        // read on listening socket, keep accepting
        while (true) {
            struct sockaddr_in addr;
            socklen_t addr_len = sizeof(addr);
            int cfd = accept(fd, (struct sockaddr*)&addr, &addr_len);
            if (cfd < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;  
                } else {
                    throw runtime_error(strerror(errno));
                }
            } else {
                // set up the buffer for each accepted socket and register 
                // the server for read events from the IO loop
                this->read_buffers[fd] = string();
                this->loop->set_handler(cfd, this, true);
            }
        }

    } else {                
        // read on existing socket, keep reading until EAGAIN
        char buffer[BUFFER_SIZE];
        bool error = false;
        while (true) {
            ssize_t n = read(fd, buffer, BUFFER_SIZE);
            if (n > 0) {            
                this->read_buffers[fd].append(buffer);
            } else if (n == 0) {    
                // socket close 
                error = true;
                break;
            } else {                
                if (errno != EAGAIN) {
                    error = true;
                } else {
                    // no more data this round, try if request is available
                    HttpRequest* request = 
                        HttpRequest::from_sequence(this->read_buffers[fd]);
                    if (request != NULL) {
                        // find a handler to handle it
                        HttpResponse* response = NULL;
                        HttpRequestHandler* handler = 
                            this->find_handler(request->path);
                        if (handler != NULL) {
                            vector<string> args = this->get_arguments(request->path);
                            if (request->method.compare("GET") == 0) {
                                response = handler->get(request, args);
                            } else if (request->method.compare("POST") == 0) {
                                response = handler->post(request, args);
                            } else {
                                throw runtime_error("Invalid HTTP method");
                            }
                        } else {
                            response = new HttpResponse(404);
                        }
                        delete request;
                        // write the response back to the client
                        if (response == NULL) {
                            response = new HttpResponse(500);
                        }
                        this->write_buffers[fd] = response->to_sequence();
                        this->loop->set_handler(fd, this, false); 
                        delete response;
                        break;
                    }
                }
                break;
            }
        }
        if (error) {
            this->read_buffers.erase(fd);
            this->write_buffers.erase(fd);
            this->loop->unset_handler(fd);
            close(fd);
        }
    }
}

void AsyncHttpServer::on_write(int& fd) {
    bool done = false;
    while (true) {
        size_t size = this->write_buffers[fd].size();
        ssize_t n = write(fd, this->write_buffers[fd].data(), size);
        if (n > 0) {
            this->write_buffers[fd] = this->write_buffers[fd].substr(n);
        } else {
            // somehow n == 0 also works event if it is not EAGAIN
            if (errno != EAGAIN) {
                done = true;
            } else {
                if (this->write_buffers[fd].size() == 0) {
                    done = true;
                }
            }
            break;
        }
    }
    if (done) {
        // not really an error but want to share the code in on_error() and note
        // that at the moment the server always close the socket after write
        this->on_error(fd);
    }
}

void AsyncHttpServer::on_error(int& fd) {
    this->read_buffers.erase(fd);
    this->write_buffers.erase(fd);
    this->loop->unset_handler(fd);
    close(fd);
}

AsyncHttpServer::AsyncHttpServer(const int& port, IOLoop* const loop) {
    // set the IO loop
    if (loop == NULL) {
        this->loop = IOLoop::instance();
    } else {
        this->loop = loop;
    }
    // create a socket, bind and listen on the port
    if ((this->fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        throw runtime_error("Create socket error for HTTP server");
    }
    int opt = 1;
    if (setsockopt(this->fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        throw runtime_error("Set socket option error for the HTTP server");
    }
    struct sockaddr_in s_addr;
    memset(&s_addr, 0, sizeof(s_addr));
    s_addr.sin_family = AF_INET;
    s_addr.sin_port = htons(port);
    s_addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(this->fd, (struct sockaddr*)&s_addr, sizeof(s_addr)) < 0) {
        throw runtime_error("Bind socket error for the HTTP server");
    }
    if (listen(this->fd, LISTEN_BACKLOG) < 0) {
        throw runtime_error("Socket listen error for the HTTP server");
    } 
    // set itself as the read handler for the socket
    this->loop->set_handler(this->fd, this, true);
}

AsyncHttpServer::~AsyncHttpServer() {
    vector<pair<string, HttpRequestHandler*> >::iterator it;
    for (it = this->handlers.begin(); it != this->handlers.end(); it++) {
        delete (*it).second;
    }
    this->read_buffers.clear();
    this->write_buffers.clear(); 
    this->handlers.clear();
}

void AsyncHttpServer::add_handler(const string& pattern, 
                                  HttpRequestHandler* const handler) {
    this->handlers.push_back(make_pair(pattern, handler));
}

HttpRequestHandler* AsyncHttpServer::remove_handler(const string& pattern) {
    HttpRequestHandler* removed = NULL;
    vector<pair<string, HttpRequestHandler*> >::iterator it;
    for (it = this->handlers.begin(); it != this->handlers.end(); it++) {
        if ((*it).first.compare(pattern) == 0) {
            this->handlers.erase(it);
            removed = (*it).second;
            break;
        }
    }
    return removed;
}

// IOLoop

IOLoop* IOLoop::loop = new IOLoop();

IOLoop::IOLoop() {
    this->fd = epoll_create(EPOLL_SIZE);
}

IOHandler* IOLoop::set_handler(const int& fd, IOHandler* const handler, 
                               bool read) {
    // set the socket non-blocking
    int flags; 
    if ((flags = fcntl(fd, F_GETFL, 0)) < 0) {
        throw runtime_error("Read file descriptor flag error");
    }
    flags = flags | O_NONBLOCK;
    if (fcntl(fd, F_SETFL, flags) < 0) {
        throw runtime_error("Set file descriptor flag error");
    }
    // add the socket to epoll
    struct epoll_event event;
    event.data.fd = fd;
    if (read) {
        event.events = EPOLLIN | EPOLLET;
    } else {
        event.events = EPOLLOUT | EPOLLET;
    }
    // unset the previous handler if any and set the new one 
    IOHandler* previous = this->unset_handler(fd);
    if (epoll_ctl(this->fd, EPOLL_CTL_ADD, fd, &event) < 0) {
        throw runtime_error("Set epoll file decriptor error (read)");
    }
    this->handlers[fd] = handler;
    return previous;
}

IOHandler* IOLoop::unset_handler(const int& fd) {
    if (epoll_ctl(this->fd, EPOLL_CTL_DEL, fd, NULL) < 0) {
        if (errno != ENOENT) { 
            throw runtime_error(strerror(errno));
        }
    }
    if (this->handlers.count(fd) == 0) {
        return NULL;
    } else {
        IOHandler* found = this->handlers[fd];
        this->handlers.erase(fd);
        return found;
    }
} 

void IOLoop::start() {
    // at the moment run forever unless an error occurs
    struct epoll_event* events = (struct epoll_event*)malloc(
        sizeof(struct epoll_event) * MAX_EVENTS);
    while (true) {
        int n;
        if ((n = epoll_wait(this->fd, events, MAX_EVENTS, -1)) < 0) {
            throw runtime_error("Wait error in epoll");
        }
        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;
            if ((events[i].events & EPOLLERR) || (events[i].events & EPOLLHUP)) {
                this->handlers[fd]->on_error(fd);
                this->unset_handler(fd);
                close(fd);
                cout << "ERROR" << endl;
            } 
            else if (events[i].events & EPOLLOUT) {
                this->handlers[fd]->on_write(fd);
            }
            else if (events[i].events & EPOLLIN) {
                this->handlers[fd]->on_read(fd);
            } 
        }
    }
}

IOLoop* IOLoop::instance() {
    return IOLoop::loop;
}
