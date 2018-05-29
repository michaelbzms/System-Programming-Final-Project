#ifndef SERVEREQUESTBUFFER_H
#define SERVEREQUESTBUFFER_H


class ServeRequestBuffer {
    struct intNode{
        int fd;
        intNode *next;
        intNode(int file_desc);
    } *head;
public:
    pthread_mutex_t lock;        // only public so that it's used in pthread_cond_wait(); - nowhere else
    ServeRequestBuffer();
    ~ServeRequestBuffer();
    bool isEmpty() const;
    // Important: push and pop do NOT lock / unlock the mutex-lock, this has to happen separately
    void push(int newfd);
    int pop();
    // Locking happens with:
    bool acquire();
    bool release();
};


#endif //SERVEREQUESTBUFFER_H
