#include <iostream>
#include <pthread.h>
#include <cstring>
#include "../headers/FIFO_Queue.h"


using namespace std;


FIFO_Queue::urlNode::urlNode(const char *link) : url(NULL), next(NULL) {
    if ( link != NULL ) {
        url = new char[strlen(link) + 1];
        strcpy(url, link);
    } else cerr << "Warning: NULL link pushed in FIFO Queue?" << endl;
}

FIFO_Queue::urlNode::~urlNode() {
    delete[] url;
}


FIFO_Queue::FIFO_Queue() : head(NULL) {
    if ( pthread_mutex_init(&lock, NULL) < 0 ){
        cerr << "Warning: pthread_mutex_init failed!" << endl;
    }
}

FIFO_Queue::~FIFO_Queue() {
    urlNode *temp = head;
    while ( temp != NULL ){
        urlNode *tmp = temp->next;
        delete temp;
        temp = tmp;
    }
    if ( pthread_mutex_destroy(&lock) < 0 ){
        cerr << "Warning: pthread_mutex_destroy failed!" << endl;
    }
}

bool FIFO_Queue::isEmpty() const { return (head == NULL); }

void FIFO_Queue::push(const char *link) {        // O(1)
    urlNode *temp = head;
    head = new urlNode(link);
    head->next = temp;
}

char *FIFO_Queue::pop() {                        // O(n) - user must delete the returning C string from the heap himself (!)
    if ( head == NULL ){                         // no elements
        return NULL;
    } else if ( head->next == NULL ) {           // one element
        char *ret_url = new char[strlen(head->url) + 1];
        strcpy(ret_url, head->url);
        delete head;
        head = NULL;
        return ret_url;
    } else {                                     // more than one element
        urlNode *temp = head->next, *pre = head;
        while (temp->next != NULL){              // traverse the list until temp is the last node
            pre = temp;
            temp = temp->next;
        }
        char *ret_url = new char[strlen(temp->url) + 1];
        strcpy(ret_url, temp->url);
        delete temp;
        pre->next = NULL;
        return ret_url;
    }
}

void FIFO_Queue::acquire() {
    if ( pthread_mutex_lock(&lock) < 0 ){
        cerr << "Warning: failed to lock the mutex!" << endl;
    }
}

void FIFO_Queue::release() {
    if ( pthread_mutex_unlock(&lock) < 0 ){
        cerr << "Warning: failed to unlock the mutex!" << endl;
    }
}
