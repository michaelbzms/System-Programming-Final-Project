#include <iostream>
#include <cstring>
#include <climits>
#include "../headers/map.h"
#include "../headers/inverted_index.h"


using namespace std;


/* posting_list implementation */
post_node::post_node(int fileNum, int lineNum) : fileID(fileNum), lineID(lineNum), times(1), next(NULL) {}

posting_list::posting_list() : head(NULL), size(0), found_by_worker(false) {}

posting_list::~posting_list() {
    post_node *temp;
    while (head != NULL){
        temp = head->next;
        delete head;
        head = temp;
        size--;
    }
}

unsigned int posting_list::get_size() const { return size; }

void posting_list::add_appearance(int file_num, int line_num) {
    if (head == NULL){
        head = new post_node(file_num, line_num);
        size = 1;
    } else{
        // search for lineID in the posting-list. If it doesn't exist add it in the correct (ordered) spot.
        post_node *prev = NULL;
        post_node *curr = head;
        do{
            if ( curr->fileID == file_num && curr->lineID == line_num ){         // found the correct post-node
                curr->times++;                           // increment the number of appearances for the word
                return;
            }
            else if ( ( curr->fileID == file_num  && curr->lineID > line_num ) || curr->fileID > file_num  ){
                // the correct post-node does not exist because there is no entry for this path with that lineID (for this word)
                // OR ("||") the correct post-node does not exist because there is no entry for this fileID at all
                if ( prev == NULL ){    // then curr should be head
                    head = new post_node(file_num, line_num);
                    head->next = curr;
                } else {
                    prev->next = new post_node(file_num, line_num);   // so create it
                    prev->next->next = curr;
                }
                size++;
                return;
            }   // else continue searching...
            prev = curr;
            curr = curr->next;
        } while ( curr != NULL );
        // if we reach the end of the postlist without a spot for the new lineID, then we must create it here
        prev->next = new post_node(file_num, line_num);
        size++;
    }
}

void posting_list::involves_lines(bool *table, int file_id) const {
    for (post_node *p = head; p != NULL ; p = p->next){
        if ( p->fileID == file_id ) table[p->lineID] = true;
        else if ( p->fileID > file_id ) break;          // if fileID post_nodes are exhausted then no need to keep searching
        // else continue searching
    }
}

void posting_list::involves_files(bool *table, int fileCount) {
    post_node *temp = head;
    while ( temp != NULL ){
        table[temp->fileID] = true;                     // there may be multiple post_nodes with the same fileID but that's ok: we have to check all of them anyway
        temp = temp->next;
    }
}

int posting_list::maxcount_file(Map *map, int &max) const {      // assumes that files are continuous on the posting list (which they should be)
    if ( head == NULL || map == NULL ) return -1;
    int max_fid = -1, cur_fid = -1, totaltimes = 0, maxtimes = -1;
    post_node *p = head;
    while (p != NULL){
        if ( cur_fid == p->fileID){                    // new post node is another line in the same file
            totaltimes += p->times;
        } else {                                       // the start of a new file
            if (p != head && totaltimes > maxtimes){   // p != head  -> so that we do not make this check on the first loop
                maxtimes = totaltimes;
                max_fid = cur_fid;
            } else if ( p != head && totaltimes == maxtimes ){
                // maxtimes stays as is
                const char *prevpath = map->getFilePath(max_fid);
                const char *newpath = map->getFilePath(cur_fid);
                if ( strcmp(newpath, prevpath) < 0 ){   // if new path is "smaller" alphabetically then keep that one instead
                    max_fid = cur_fid;
                }   // else max_fid stays as is
            }
            cur_fid = p->fileID;
            totaltimes = p->times;
        }
        p = p->next;
    }
    if (totaltimes > maxtimes){                          // in case max_fid should be the last cur_fid
        maxtimes = totaltimes;
        max_fid = cur_fid;
    } else if (totaltimes == maxtimes ){
        const char *prevpath = map->getFilePath(max_fid);
        const char *newpath = map->getFilePath(cur_fid);
        if ( strcmp(newpath, prevpath) < 0 ){            // if new path is "smaller" alphabetically then keep that one instead
            max_fid = cur_fid;
        }   // else max_fid stays as is
    }
    max = maxtimes;
    return max_fid;
}

int posting_list::mincount_file(Map *map, int &min) const {
    if ( head == NULL || map == NULL ) return -1;
    int min_fd = -1, cur_fid = -1, totaltimes = 0, mintimes = INT_MAX;
    post_node *p = head;
    while (p != NULL){
        if ( cur_fid == p->fileID){                    // new post node is another line in the same file
            totaltimes += p->times;
        } else {                                       // the start of a new file
            if (p != head && totaltimes < mintimes){   // p != head  -> so that we do not make this check on the first loop
                mintimes = totaltimes;
                min_fd = cur_fid;
            } else if ( p != head && totaltimes == mintimes ){
                // mintimes stays as is
                const char *prevpath = map->getFilePath(min_fd);
                const char *newpath = map->getFilePath(cur_fid);
                if ( strcmp(newpath, prevpath) < 0 ){   // if new path is "smaller" alphabetically then keep that one instead
                    min_fd = cur_fid;
                }   // else min_fd stays as is
            }
            cur_fid = p->fileID;
            totaltimes = p->times;
        }
        p = p->next;
    }
    if (totaltimes < mintimes){                         // in case min_fd should be the last cur_fid
        mintimes = totaltimes;
        min_fd = cur_fid;
    } else if (totaltimes == mintimes ){
        const char *prevpath = map->getFilePath(min_fd);
        const char *newpath = map->getFilePath(cur_fid);
        if ( strcmp(newpath, prevpath) < 0 ){           // if new path is "smaller" alphabetically then keep that one instead
            min_fd = cur_fid;
        }   // else min_fd stays as is
    }
    min = mintimes;
    return min_fd;
}

void posting_list::set_found_word() {
    found_by_worker = true;
}

bool posting_list::get_found_word() const {
    return found_by_worker;
}


/* Trie Implementation */
Trie_node::Trie_node(char ch) : letter(ch), down(NULL), next(NULL), posting_list(NULL) {}

Trie::Trie() : root(NULL) {}

void recDelete(Trie_node *curNode){                                    // recursively deletes the Trie in a dfs manner
    if (curNode->down != NULL) recDelete(curNode->down);               // delete "your" eldest child
    if (curNode->next != NULL) recDelete(curNode->next);               // delete "your" current-in-line brother
    if (curNode->posting_list != NULL) delete curNode->posting_list;   // delete "your" posting list
    delete curNode;                                                    // then delete "yourself"
}

Trie::~Trie() {
    if (root != NULL) recDelete(root);
}

void Trie::insert(char *word, int file_num, int line_num) {             // word must be a NULL-terminated C string
    if (word == NULL || word[0] == '\0'){ cerr << "Warning: empty word insertion to the inverted_index!"; return;}          // should not insert the empty word ("")
    int i = 0;
    Trie_node *curnode = root;
    Trie_node *previous = NULL;
    if (root == NULL){                                    // if called for root == NULL (will only happen at first insert)
        root = new Trie_node(word[0]);
        i = 1;
        curnode = NULL;     // = root->down
        previous = root;    // (!) Important for later
    }
    for ( ; word[i] != '\0' ; i++, curnode = curnode->down ){     // each iteration traverses (using curnode) a horizontal list on the Trie
        if ( curnode == NULL){                            // if this horizontal list is NULL
            curnode = new Trie_node(word[i]);             // create its first node
            previous->down = curnode;
        }
        else {                                            // else (curnode != NULL)
            // search for the alphabetically correct spot/node for the character word[i]
            Trie_node *temp;
            bool first_loop = true;
            for (temp  = curnode ; temp != NULL ; temp = temp->next ){   // traverse the horizontal list
                if ( word[i] == temp->letter ){           // if word[i] letter already exists on the Trie
                    curnode = temp;                       // curnode now points to that node
                    break;
                }
                else if ( word[i] < temp->letter ){       // else if it doesn't exist
                    curnode = new Trie_node(word[i]);     // create a new node (in its correct spot)
                    if (first_loop && previous != NULL) previous->down = curnode;   // (!) if its correct spot is the head of the horizontal list the previous->down must be updated
                    else if (previous != NULL) previous->next = curnode;            // else it is previous->current which must be updated to curnode
                    else root = curnode;                  // the only circumstance under which previous is NULL is when the new curnode should be placed on the head of the FIRST horizontal list
                    curnode->next = temp;
                    break;
                }
                previous = temp;
                first_loop = false;
            }
            if ( temp == NULL ){                          // if word[i] should be on the end of this horizontal list (<=> previous for loop expired)
                curnode = new Trie_node(word[i]);         // then create a new node here
                previous->next = curnode;                 // previous can never be NULL here because: curnode != NULL => (temp = curnode) != NULL => (previous = temp) != NULL
            }
        }
        previous = curnode;
    }
    // By the end of all of the above the new word ends at the TrieNode: *previous. (or *root if previous is NULL)
    if (previous == NULL){
        if (root->posting_list == NULL) { root->posting_list = new posting_list(); }
        root->posting_list->add_appearance(file_num, line_num);
    }
    else {
        if ( previous->posting_list == NULL) { previous->posting_list = new posting_list(); }
        previous->posting_list->add_appearance(file_num, line_num);
    }
}

posting_list *Trie::search(char *word) {                  // word must be a NULL-terminated C string
    if (root == NULL || word[0] == '\0') return NULL;     // should not search for the empty word ("")
    Trie_node *curNode = root, * previous = root;
    for (int i = 0 ; word[i] != '\0' ; i++, curNode = curNode->down){     // iterate word's characters (at least one loop should be executed)
        // find the correct curNode for this horizontal list based on word[i]
        if (curNode == NULL) return NULL;                 // if we reached a leaf whilst not having depleted the given word then it doesn't exist on the Trie
        while ( curNode->letter < word[i] ) {             // keep traversing the horizontal list until curNode->letter >= word[i]
            if (curNode->next == NULL) return NULL;       // or all letters on the horizontal list are < word[i], in which case the word doesn't exist so return NULL
            curNode = curNode->next;
        }
        if (curNode->letter > word[i]) return NULL;       // if a node with node->letter == word[i] doesn't exist then the word we are searching for doesn't exist so return NULL
        // if we haven't "returned" by now then curNode->letter == word[i]
        previous = curNode;                               // save previous value (only used after the last loop)
    }
    return previous->posting_list;
}
