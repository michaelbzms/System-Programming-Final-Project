#ifndef FIFO_QUEUE_H
#define FIFO_QUEUE_H


class FIFO_Queue {
    struct urlNode{
        char *url;
        urlNode *next;
        urlNode(const char *link);
        ~urlNode();
    } *head;
public:
    pthread_mutex_t lock;        // only public so that it's used in pthread_cond_wait(); - nowhere else
    FIFO_Queue();
    ~FIFO_Queue();
    bool isEmpty() const;
    // Important: push and pop do NOT lock / unlock the mutex-lock, this has to happen separately
    void push(const char *link);
    char *pop();                 // returns a C string pointer to heap - must be deleted after it's used
    // Locking happens with:
    void acquire();
    void release();
};

#endif //FIFO_QUEUE_H
