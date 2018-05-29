#ifndef URL_HISTORY_H
#define URL_HISTORY_H


class str_history {             // struct that stores all C strings only once - binary search tree: Search O(logn), Insertion: O(logn)
    struct histNode{
        char *str;
        histNode *left, *right;
        histNode(const char *str);
        ~histNode();
    } *head;
    pthread_mutex_t lock;
    unsigned int size;
public:
    str_history();
    ~str_history();
    // add and search ARE ATOMIC because they lock the struct's mutex
    void add(const char *str);
    bool search(const char *str);
    char **get_all_strings_as_table();       // returns a this->size big table of all C Strings on the tree (in the heap)
    unsigned int get_size() const;
private:
    // Locking happens with:
    void acquire();
    void release();
    // helper funtion
    void rec_get_all_strings(histNode *p, char **table);
};


#endif //URL_HISTORY_H
