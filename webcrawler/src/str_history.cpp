#include <iostream>
#include <cstring>
#include "../headers/str_history.h"


using namespace std;


str_history::histNode::histNode(const char *string) : left(NULL), right(NULL) {
    str = new char[strlen(string) + 1];
    strcpy(this->str, string);
}

str_history::histNode::~histNode() {
    delete[] str;
    if (left != NULL) delete left;
    if (right != NULL) delete right;
}

str_history::str_history() : head(NULL), size(0) {
    if ( pthread_mutex_init(&lock, NULL) < 0 ){
        cerr << "Warning: pthread_mutex_init failed!" << endl;
    }
}

str_history::~str_history() {
    if ( head != NULL ) delete head;            // this delete will recursively delete the whole Tree
    if ( pthread_mutex_destroy(&lock) < 0 ){
        cerr << "Warning: pthread_mutex_destroy failed!" << endl;
    }
}

void str_history::add(const char *str) {       // O(logn)
    this->acquire();            // lock the mutex
    if (head == NULL){
        size++;
        head = new histNode(str);
    } else {
        histNode *pos = head;
        for (;;) {              // find the right position for a new node
            if ( strcmp(str, pos->str) == 0 ){
                // If the str already exist in our history tree then do NOT add it again
                break;
            }
            else if (strcmp(str, pos->str) < 0) {
                if (pos->left == NULL){
                    pos->left = new histNode(str);
                    size++;
                    break;
                } else pos = pos->left;
            } else {
                if (pos->right == NULL){
                    pos->right = new histNode(str);
                    size++;
                    break;
                } else pos = pos->right;
            }
        }
    }
    this->release();           // unlock the mutex
}

bool str_history::search(const char *str) {      // O(logn)
    this->acquire();          // lock the mutex
    if (head == NULL) { this->release(); return false; }
    histNode *pos = head;
    for (;;) {
        if ( strcmp(str, pos->str) == 0 ){
            this->release();  // unlock the mutex
            return true;
        }
        else if ( strcmp(str, pos->str) < 0 ){
            if ( pos->left == NULL ) { this->release(); return false; }
            else pos = pos->left;
        }
        else {
            if ( pos->right == NULL ) { this->release(); return false; }
            else pos = pos->right;
        }
    }
}

void str_history::rec_get_all_strings(histNode *p, char **table) {
    static int pos = 0;
    table[pos] = new char[strlen(p->str) + 1];
    strcpy(table[pos], p->str);
    pos++;
    if ( pos > this->size ) { cerr << "Warning: getting all strings found more than struct's size, pos = "<< pos << "and size = " << size << endl; return; }
    if ( p->left != NULL ) rec_get_all_strings(p->left, table);
    if ( p->right != NULL ) rec_get_all_strings(p->right, table);
}

char **str_history::get_all_strings_as_table() {
    this->acquire();          // lock the mutex
    if ( head == NULL ) return NULL;
    char **table = new char*[this->size];
    rec_get_all_strings(head, table);
    this->release();          // unlock the mutex
    return table;
}

void str_history::acquire() {
    if ( pthread_mutex_lock(&lock) < 0 ){
        cerr << "Warning: failed to lock the mutex!" << endl;
    }
}

void str_history::release() {
    if ( pthread_mutex_unlock(&lock) < 0 ){
        cerr << "Warning: failed to unlock the mutex!" << endl;
    }
}

unsigned int str_history::get_size() const {
    return size;
}
