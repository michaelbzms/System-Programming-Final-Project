#include <iostream>
#include <cstring>
#include <fstream>
#include <dirent.h>               // for directory traversing
#include "../headers/inverted_index.h"
#include "../headers/util.h"
#include "../headers/textfiles_parsing.h"
#include "../headers/map.h"


using namespace std;


/* Global variables */
extern char **subdirectories;             // a table containing ONLY this worker's directory paths in C strings
extern int subdirfilesize;                // its count
extern Trie *inverted_index;              // the inverted_index (Trie) for this worker
extern Map *map;


/* Local Functions */
int parse_text(int subdir_num, char *name);
int count_lines(char *filepath, intnode *&line_sizes);


int parse_subdirectories(){                         // parse all text (ascii) files in all your directories
    if ( subdirfilesize < 0 ){
        cerr << "Error parsing subdirectories: subdirectories not initialized yet" << endl;
        return -3;
    }
    // First count how many text files at our jurisdiction
    int filecount = 0;
    for (int i = 0 ; i < subdirfilesize && subdirectories[i] != NULL ; i++) {    // for each directory assigned to this worker
        DIR *pdir = NULL;
        pdir = opendir(subdirectories[i]);
        if (pdir == NULL) {
            cerr << "\nERROR! pdir could not be initialised correctly";
            return -1;
        }
        struct dirent *pent = NULL;
        while (pent = readdir(pdir)) {                     // while there is still something in the directory to list
           if (strcmp(pent->d_name, ".") != 0 && strcmp(pent->d_name, "..") != 0) filecount++;
        }
        closedir(pdir);
    }
    // Create inverted_index and map structures
    inverted_index = new Trie();
    map = new Map(filecount);
    // Parse each text file, adding its contents to inverted_index and map
    for (int i = 0 ; i < subdirfilesize && subdirectories[i] != NULL ; i++) {
        DIR *pdir = NULL;
        pdir = opendir(subdirectories[i]);
        if (pdir == NULL) {
            cerr << "\nERROR! pdir could not be initialised correctly";
            return -1;
        }
        struct dirent *pent = NULL;
        while (pent = readdir(pdir)) {              // while there is still something in the directory to list
            if (strcmp(pent->d_name, ".") != 0 && strcmp(pent->d_name, "..") != 0) {
                int feedback = parse_text(i, pent->d_name);     // parse that text file
                if (feedback < 0) {
                    cerr << "Something went wrong parsing a text file. Feedback: " << feedback << "\n"
                         << "fileID: " << i << " name: " << pent->d_name << endl;
                    return -2;
                }
            }
        }
        closedir(pdir);
    }
    return 0;
}


int parse_text(int subdir_num, char *name){    // parses a single textfile
    static int fileID = 0;                     // the fileID for each file we parse with this function
    char *filepath = new char[strlen(subdirectories[subdir_num]) + strlen(name) + 2];  // +1 for '/' and +1 for '\0'
    strcpy(filepath, subdirectories[subdir_num]);
    strcat(filepath, "/");
    strcat(filepath, name);
    intnode *line_sizes = NULL;                // a list of the sizes of each line (needed for the dynamic allocation of them in our "map" structure)
    int linecount = count_lines(filepath, line_sizes);
    if ( linecount <= 0 ) {                    // if count_lines failed then return error
        while (line_sizes != NULL){            // after cleaning up the already created int list
            intnode *tmp = line_sizes->next;
            delete line_sizes;
            line_sizes = tmp;
        }
        delete[] filepath;
        return -1;
    }
    ifstream textfile(filepath);
    if ( !line_sizes ) {                       // should not fail since it must have worked on count_lines()
        cerr << "Unexpected error reopening a textfile!\n";
        while (line_sizes != NULL){            // cleanup int list
            intnode *tmp = line_sizes->next;
            delete line_sizes;
            line_sizes = tmp;
        }
        delete[] filepath;
        return -2;
    }
    map->add_new_textfile(filepath, linecount);  // add a new entry for this textfile on our map structure
    intnode *l = line_sizes;                 // l is used to traverse the int list
    char ch;
    char *curline = NULL;
    int curlinelen = -1;
    for ( int i = 0 ; i < linecount && textfile.peek() != EOF ; i++ ){
        ignore_whitespace(textfile);         // ignore whitespace at the start of each line
        if ( textfile.peek() == '\n' ){      // Ignore empty lines
            textfile.ignore(1);              // ignore it
            i--;                             // without adding to i
            continue;
        }
        if ( l == NULL ){                    // should not happen
            cerr << "Unexpected error: int list of line_sizes not complete?\n";
            break;
        }
        curlinelen = l->val + 1;             // l->val is the chars on this line as counted in the 1st pass, + 1 for '\0'
        curline = new char[curlinelen];
        int j = 0;
        bool html_tag = false;
        textfile.get(ch);
        while ( ch != '\n' ){                // read char-by-char on this line and write each char to curline[j], while ignore html tages "<...>"
            if ( ch == '<' ) html_tag = true;
            if ( j > l->val || (j == l->val && !html_tag) ){               // should not happen
                cerr << "Unexpected error: Possible wrong calculation; More chars in a document than anticipated!\n";
                cerr << "j = " << j << ", l->val = " << l->val << endl;
                break;
            }
            if (!html_tag) {
                curline[j++] = ch;
                map->byteCount++;            // increment total number of bytes found
            }
            if ( ch == '>' ) html_tag = false;
            textfile.get(ch);
        }
        curline[j] = '\0';                   // add the final '\0'
        l = l->next;                         // move to the current int node
        // Add this line to our map structure
        map->lineCount++;                    // increment total number of lines found
        int fb = map->insert_line_to_textfile(fileID, i, curline, curlinelen);
        if ( !fb ){
            cerr << "Unexpected warning: could not insert a line to a textfile in map\n";
        }
        // Add each word (using white space as delimiter in strtok) to the inverted index, but only if line is not actually empty without the html tags
        if ( strcmp(curline, "") != 0 ) {
            char *wordptr;
            wordptr = strtok(curline, " \t");
            while (wordptr != NULL) {
                inverted_index->insert(wordptr, fileID, i);
                map->wordCount++;                // increment total number of words found
                wordptr = strtok(NULL, " \t");
            }
        }
        delete[] curline;
    }
    // increment fileID static counter for next call
    fileID++;
    // cleanup int list
    while (line_sizes != NULL){
        intnode *tmp = line_sizes->next;
        delete line_sizes;
        line_sizes = tmp;
    }
    delete[] filepath;
    return 0;
}


int count_lines(char *filepath, intnode *&line_sizes) {   // This is the 1st pass of each textfile
    ifstream textfile(filepath);
    if (!textfile){                              // if we can not open the textfile return error
        cerr << "Cannot open textfile" << endl;
        return -1;
    }
    int line_count = 0;                          // the number of lines aka documents
    int char_count;                              // the count of the (C String) document in each line, which is saved on an int list
    char ch;
    intnode *l = line_sizes;                     // l is used to create & traverse a list of integers
    while ( textfile.peek() != EOF ) {
        ignore_whitespace(textfile);             // ignore possible white space at the start of each line
        if (textfile.peek() == '\n') { textfile.ignore(1); continue; }   // empty line? Ignore it
        line_count++;                            // increment line_count
        if (line_sizes == NULL){
            line_sizes = l = new intnode;
        } else{
            l->next = new intnode;
            l = l->next;
        }
        char_count = 0;
        textfile.get(ch);
        bool html_tag = false;
        while ( ch != '\n' ) {                  // read char-by-char each line, while ignore html tags "<...>"
            if ( ch == '<' ) html_tag = true;
            if (!html_tag) char_count++;        // count how many chars in a line
            if ( ch == '>' ) html_tag = false;
            textfile.get(ch);
        }
        l->val = char_count;                    // save how many chars there are in each line on line_sizes int list
        l->next = NULL;
    }
    return line_count;
}
