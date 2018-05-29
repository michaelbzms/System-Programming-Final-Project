#include <iostream>
#include <pthread.h>
#include "../headers/ServeRequestBuffer.h"


using namespace std;


ServeRequestBuffer::intNode::intNode(int file_desc) : fd(file_desc), next(NULL) {}

ServeRequestBuffer::ServeRequestBuffer() : head(NULL) {
    if ( pthread_mutex_init(&lock, NULL) < 0 ){
        cerr << "Warning: pthread_mutex_init failed!" << endl;
    }
}

ServeRequestBuffer::~ServeRequestBuffer() {
    intNode *temp = head;
    while ( temp != NULL ){
        intNode *tmp = temp->next;
        delete temp;
        temp = tmp;
    }
    if ( pthread_mutex_destroy(&lock) < 0 ){
        cerr << "Warning: pthread_mutex_destroy failed!" << endl;
    }
}

bool ServeRequestBuffer::isEmpty() const { return (head == NULL); }

void ServeRequestBuffer::push(int newfd) {       // O(1)
    intNode *temp = head;
    head = new intNode(newfd);
    head->next = temp;
}

int ServeRequestBuffer::pop() {                  // O(n)
    if ( head == NULL ){                         // no elements
        return -1;
    } else if ( head->next == NULL ) {           // one element
        int ret_val = head->fd;
        delete head;
        head = NULL;
        return ret_val;
    } else {                                     // more than one element
        intNode *temp = head->next, *pre = head;
        while (temp->next != NULL){              // traverse the list until temp is the last node
            pre = temp;
            temp = temp->next;
        }
        int ret_val = temp->fd;
        delete temp;
        pre->next = NULL;
        return ret_val;
    }
}

bool ServeRequestBuffer::acquire() {
    if ( pthread_mutex_lock(&lock) < 0 ){
        cerr << "Warning: failed to lock the mutex!" << endl;
    }
}

bool ServeRequestBuffer::release() {
    if ( pthread_mutex_unlock(&lock) < 0 ){
        cerr << "Warning: failed to unlock the mutex!" << endl;
    }
}
