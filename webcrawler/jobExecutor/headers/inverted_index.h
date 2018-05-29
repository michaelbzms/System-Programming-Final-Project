#ifndef INVERTED_INDEX_H
#define INVERTED_INDEX_H

#include "map.h"

/** Note:                                                                                                     *
 * - The post_nodes in posting_list are ordered by their pair of (fileID, lineID) values (in that order)                                       *
 * - The Trie_nodes in the Trie are also ordered horizontally-wise by their letter-value (aka alphabetically) *
 * This allows for faster operations on them in the price of a slight pre-processing cost at creation time.   */


/* Posting List */
struct post_node{
    int fileID, lineID, times;             // each post node contains: [fileID, lineID, times]
    post_node *next;
    post_node(int fileNum, int lineNum);
};

class posting_list{
    post_node *head;
    unsigned int size;                     // the count of a list which is also the word's document frequency
    bool found_by_worker;                  // true if the worker had to search for this word (and found it obviously)
public:
    posting_list();
    ~posting_list();
    void set_found_word();
    bool get_found_word() const;
    void add_appearance(int file_num, int line_num);         // adds an appearance for the specific word in the given file and line
    unsigned int get_size() const;
    int maxcount_file(Map *map, int &max) const;
    int mincount_file(Map *map, int &min) const;
    void involves_lines(bool *table, int file_id) const;     // turns on the i-th byte in the given bool table iff a post_node with lineID = i and fileID = file_id exists on the list
    void involves_files(bool *table, int fileCount);         // turns on the i-th byte in the given bool table iff there is a post_node with fileID = i on the posting_list
};


/* Trie (inverted_index) */
struct Trie_node{
    char letter;
    Trie_node *down;                      // the current letter in the same word, NULL if it doesn't exist
    Trie_node *next;                      // another letter instead of this as a current char for its father
    class posting_list *posting_list;     // if != NULL then this->letter is the last char of a word
    Trie_node(char ch);
};

class Trie{
    Trie_node *root;
public:
    Trie();
    ~Trie();
    void insert(char *word, int file_num, int line_num);   // inserts a word from a given line in a given text file in the Trie
    posting_list *search(char *word);                      // searches for a word in the Trie and if found returns its posting list, else NULL
};


#endif //INVERTED_INDEX_H
