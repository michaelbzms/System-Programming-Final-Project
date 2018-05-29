#include <iostream>
#include <unistd.h>
#include <cstdlib>
#include <fcntl.h>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <signal.h>
#include <cerrno>
#include "../headers/map.h"
#include "../headers/inverted_index.h"
#include "../headers/textfiles_parsing.h"
#include "../headers/util.h"


using namespace std;


#define LOGS_PATH_INTRO "./jobExecutor/log/Worker_"


/* Global variables */
char **subdirectories = NULL;             // a table containing ONLY this worker's directory paths in C strings
int subdirfilesize = -1;                  // its count
Trie *inverted_index = NULL;
Map *map = NULL;
int TotalWordsFound = 0;

/* Global variables used by signal handlers */
volatile bool search_canceled = false;
volatile bool worker_must_terminate = false;


/* Local functions */
void search(char **query, int query_size, const int answer_fd, ofstream &log);


/* Signal Handlers */
static void stop_searching(int sig);
static void handle_sig_termination(int sig);


int main(int argc, char *argv[]){
    subdirfilesize = atoi(argv[1]);
    if (subdirfilesize <= 0){
        cerr << "Error: Created a worker with no directories\n";
        return -1;
    }
    // open (and create if 404) your logs file
    char logsfilename[128];
    {   // construct logsfilename:
        strcpy(logsfilename, LOGS_PATH_INTRO);
        char pid_str[128];
        sprintf(pid_str, "%d", getpid());
        strcat(logsfilename, pid_str);
    }
    ofstream log;
    log.open(logsfilename ,ofstream::out | ofstream::app);     // if there are previous contents, do not write over them, rather append the new ones there
    if ( log.fail() ){
        perror("openning log file for writting failed\n");
        return -2;
    }
    char pipeR[128], pipeA[128];      // Request pipe and Answer pipe
    strcpy(pipeR, argv[2]);
    strcpy(pipeA, argv[3]);
    subdirectories = new char*[subdirfilesize];

    // open your (read) side of the request pipe. Block until jobExecutors also opens its (write) side.
    int request_fd = open(pipeR, O_RDONLY);
    if ( request_fd < 0 ){
        perror("worker could not open request pipe");
        delete[] subdirectories;
        log.close();
        return -3;
    }

    // read subdirectories (= your subset of all the directories containing text files) from pipeR
    for (int i = 0 ; i < subdirfilesize ; i++) {
        Header header;
        if ( read(request_fd, &header, sizeof(Header)) < 0 ) perror("reading subdirectories");
        subdirectories[i] = new char[header.message_size];
        if ( read(request_fd, subdirectories[i], header.message_size) < 0 ) perror("reading subdirectories");
        if (header.type == END_OF_MESSAGES ) {    // if this was the last directory assigned to this worker
            i++;                                  // (!)
            for (; i < subdirfilesize; i++) {
                subdirectories[i] = NULL;
            }
            break;
        }
    }

    // parse subdirectories (= your subset of all the directories containing text files)
    if (parse_subdirectories() < 0){
        cerr << "Worker" << getpid() << " could not parse its directories\n";
        if (inverted_index != NULL) delete inverted_index;
        if (map != NULL) delete map;
        for (int i = 0 ; i < subdirfilesize && subdirectories[i] != NULL ; i++){
            delete[] subdirectories[i];
        }
        delete[] subdirectories;
        close(request_fd);
        log.close();
        return -4;
    }

    // Signal Handling
    struct sigaction act;
    memset(&act, 0, sizeof(act));
    act.sa_handler = handle_sig_termination;
    sigaction(SIGINT, &act, NULL);
    sigaction(SIGTERM, &act, NULL);
    sigaction(SIGQUIT, &act, NULL);

    memset(&act, 0, sizeof(act));
    act.sa_handler = SIG_IGN;
    sigaction(SIGUSR1, &act, NULL);                 // ignore USR1 signal (used in /search)

    memset(&act, 0, sizeof(act));
    act.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &act, NULL);                 // ignore SIGPIPE signal

    // open your (write) side of the answer pipe. The other end should already be opened from the jobExecutor (who should be blocked)
    int answer_fd = open(pipeA, O_WRONLY);
    if ( answer_fd < 0 ){
        perror("worker could not open answer pipe");
        if (inverted_index != NULL) delete inverted_index;
        if (map != NULL) delete map;
        for (int i = 0 ; i < subdirfilesize && subdirectories[i] != NULL ; i++){
            delete[] subdirectories[i];
        }
        delete[] subdirectories;
        close(request_fd);
        log.close();
        return -5;
    }


    // Start accepting requests
    while ( !worker_must_terminate ) {
        Header header;
        ssize_t nbytes = read(request_fd, &header, sizeof(Header));
        if ( nbytes < 0 && errno == EINTR && worker_must_terminate ) {       // if we get a terminating signal whilst blocked on read
            break;
        }
        else if ( nbytes < 0 ){
            perror("read header on worker");        // should not happen
            continue;
        }
        if ( header.type == EXIT ){                 // jobExecutor ordered this worker to stop
            // send jobExecutor the number of (different) words that were successfully /searched
            write(answer_fd, &TotalWordsFound, sizeof(int));
            break;
        }
        else if ( header.type == SEARCH ){          // maximum words for query are 10 (the rest are ignored)
            search_canceled = false;                // initialize the flag for search cancelation
            // setup a signal handler for SIGUSR1 temporarily. If we get this signal the the deadline has expired in jobExecutor and we should try to stop searching in vain asap
            memset(&act, 0, sizeof(act));
            act.sa_handler = stop_searching;
            sigaction(SIGUSR1, &act, NULL);
            int query_size = header.message_size;   // message_size here is used as a way to pass the query's size
            // read all query's words from the request pipe
            ssize_t bytes_read;
            char **words = new char*[query_size];
            for (int i = 0 ; i < query_size ; i++){
                Header wordhdr;
                while ( ( bytes_read = read(request_fd, &wordhdr, sizeof(Header)) ) < 0 && errno == EINTR) ;         // if interupted by SIGUSR1 we still have to read all the input request
                if (bytes_read < 0) { perror("read on worker"); }   // should not happen
                words[i] = new char[wordhdr.message_size];
                while ( ( bytes_read = read(request_fd, words[i], wordhdr.message_size) ) < 0 && errno == EINTR) ;   // if interupted by SIGUSR1 we still have to read all the input request
                if (bytes_read < 0) { perror("read on worker"); }   // should not happen
            }
            if (worker_must_terminate){    // reading the whole request message before terminating is important! (for his possible replacement to work)
                for (int i = 0 ; i < query_size ; i++){
                    delete[] words[i];
                }
                delete[] words;
                break;
            }
            // execute /search and write results in the answer pipe as well as update log
            search(words, query_size, answer_fd, log);
            // reset the ignore action for SIGUSR1
            memset(&act, 0, sizeof(act));
            act.sa_handler = SIG_IGN;
            sigaction(SIGUSR1, &act, NULL);
            for (int i = 0 ; i < query_size ; i++){
                delete[] words[i];
            }
            delete[] words;
        }
        else if ( header.type == MAXCOUNT ){
            char *keyword = new char[header.message_size];
            if ( read(request_fd, keyword, header.message_size) < 0){
                perror("read on worker");
            }
            // response header:
            Header response;
            // search for the keyword on the inverted_index
            posting_list *pl = inverted_index->search(keyword);
            if ( pl != NULL && !worker_must_terminate ){
                int max = -1;
                int max_fileID = pl->maxcount_file(map, max);
                if ( max_fileID < 0 || max == -1 ) {   // should not happen
                    cerr << "Unexpected error: maxcount failed" << endl;
                    response.type = ERROR;
                    response.message_size = 0;
                    write(answer_fd, &response, sizeof(Header));
                }
                else{
                    char *fpath = map->getFilePath(max_fileID);
                    if (fpath != NULL ){
                        response.type = MAXCOUNT;
                        response.message_size = (unsigned int) (strlen(fpath) + 1);
                        write(answer_fd, &response, sizeof(Header));
                        write(answer_fd, fpath, strlen(fpath) + 1);
                        write(answer_fd, &max, sizeof(int));
                        // update log
                        log << get_current_time() << " : maxcount : " << keyword << " : " << max << " : " << fpath << endl;
                    }
                    else {   // should not happen
                        cerr << "Error 404: file found does not exist?" << endl;
                        response.type = ERROR;
                        response.message_size = 0;
                        write(answer_fd, &response, sizeof(Header));
                    }
                }
            } else if (!worker_must_terminate){       // keyword does not exist in any of this worker's textfiles (could happen)
                response.type = END_OF_MESSAGES;
                response.message_size = 0;
                write(answer_fd, &response, sizeof(Header));
                log << get_current_time() << " : maxcount : " << keyword << " : - : -" << endl;  // "-" means the word was not found
            }
            delete[] keyword;
        }
        else if ( header.type == MINCOUNT ){
            char *keyword = new char[header.message_size];
            if ( read(request_fd, keyword, header.message_size) < 0 ){
                perror("read on worker");             // should not happen
            }
            // response header:
            Header response;
            // search for the keyword on the inverted_index
            posting_list *pl = inverted_index->search(keyword);
            if ( pl != NULL && !worker_must_terminate ){
                int min = -1;
                int min_fileID = pl->mincount_file(map, min);
                if ( min_fileID < 0 || min == -1 ) {   // should not happen
                    cerr << "Unexpected error: maxcount failed" << endl;
                    response.type = ERROR;
                    response.message_size = 0;
                    write(answer_fd, &response, sizeof(Header));
                }
                else{
                    char *fpath = map->getFilePath(min_fileID);
                    if (fpath != NULL ){
                        response.type = MINCOUNT;
                        response.message_size = (unsigned int) (strlen(fpath) + 1);
                        write(answer_fd, &response, sizeof(Header));
                        write(answer_fd, fpath, strlen(fpath) + 1);
                        write(answer_fd, &min, sizeof(int));
                        // update log
                        log << get_current_time() << " : mincount : " << keyword << " : " << min << " : " << fpath << endl;
                    }
                    else {  // should not happen
                        cerr << "Error 404: file found does not exist?" << endl;
                        response.type = ERROR;
                        response.message_size = 0;
                        write(answer_fd, &response, sizeof(Header));
                    }
                }
            } else if (!worker_must_terminate){       // keyword does not exist in any of this worker's textfiles (could happen)
                response.type = END_OF_MESSAGES;
                response.message_size = 0;
                write(answer_fd, &response, sizeof(Header));
                log << get_current_time() << " : mincount : " << keyword << " : - : -" << endl;  // "-" means the word was not found
            }
            delete[] keyword;
        }
        else if ( header.type == WORDCOUNT ){
            int bytecount, wordcount, linecount;
            bytecount = map->byteCount;
            wordcount = map->wordCount;
            linecount = map->lineCount;
            // there is no need for a response header here as jobExecutor knows what to expect as a response to its request
            write(answer_fd, &bytecount, sizeof(int));
            write(answer_fd, &wordcount, sizeof(int));
            write(answer_fd, &linecount, sizeof(int));
            // update log
            log << get_current_time() << " : wc : " << bytecount << " : " << wordcount << " : " << linecount << endl;
        }
        else if ( !worker_must_terminate ){
            cerr << "Unrecognized command (" << header.type <<  ")given to a worker\n";
        }
    }
    // close logs file
    log.close();
    // close pipes
    close(request_fd);
    close(answer_fd);
    // cleanup:
    delete inverted_index;
    delete map;
    for (int i = 0 ; i < subdirfilesize && subdirectories[i] != NULL ; i++){
        delete[] subdirectories[i];
    }
    delete[] subdirectories;
    return 0;
}


/* Local Functions Implementation */
void search(char **query, int query_size, const int answer_fd, ofstream &log) {
    bool found_at_least_one_word = false;
    posting_list **pls = new posting_list *[query_size];
    for (int i = 0 ; i < query_size && !search_canceled && !worker_must_terminate ; i++) {    // for every word in the query
        pls[i] = inverted_index->search(query[i]);           // search if that word exists on the inverted_index
        if ( pls[i] != NULL ){
            found_at_least_one_word = true;
        }
        if ( pls[i] != NULL && !pls[i]->get_found_word() ){  // if search was successfull and found a new word that has not been searched before
            TotalWordsFound++;                               // then update TotalWordsFound to reflect that
            pls[i]->set_found_word();                        // update posting list so that we know this word was found by the worker on a search request
        }
    }
    if ( worker_must_terminate ){
        delete[] pls;                                        // do not send END_OF_MESSAGES so that jobExecutor can detect our death as a death and not a search cancellation
        return;
    }
    if (search_canceled){                                    // check if search was cancelled due to deadline expired
        Header header(END_OF_MESSAGES, 0);
        write(answer_fd, &header, sizeof(Header));
        delete[] pls;
        return;
    }
    if (!found_at_least_one_word){
        for (int i = 0 ; i < query_size ; i++){
            log << get_current_time() << " : search : " << query[i] << " : -" << endl;
            // "-" denotes that the word was not found in the worker's text files
        }
        Header header(END_OF_MESSAGES, 0);
        write(answer_fd, &header, sizeof(Header));
        delete[] pls;
        return;
    }
    for (int i = 0 ; i < map->getFileCount() ; i++){         // for each text file
        int linecount = map->getFileLinesCount(i);
        bool *query_involves_lines = new bool[linecount];    // a boolean table "parallel" to documents table
        for (int j = 0 ; j < linecount ; j++){               // initialize to all false
            query_involves_lines[j] = false;
        }
        for (int j = 0 ; j < query_size ; j++){              // for every word in the query
            if ( pls[j] != NULL) {                           // if the search for the j-th word was successful
                pls[j]->involves_lines(query_involves_lines, i);                     // query_involves_lines[j] will be true if the the j-th line contains a word from the query
            }
        }
        for (int j = 0 ; j < linecount && !worker_must_terminate ; j++){             // for each line relevant to the query
            if (query_involves_lines[j]){
                // construct final string for this line
                const char *path = map->getFilePath(i);
                const char *line = map->getPTRforline(i, j);
                char line_number[256];
                sprintf(line_number, "%d", j+1);
                int msg_size = strlen(path) + 2 + strlen(line_number) + 1 + strlen(line) + 1;
                char *response_str = new char[msg_size];
                strcpy(response_str, path);
                strcat(response_str, "  ");
                strcat(response_str, line_number);
                strcat(response_str, "\n");
                strcat(response_str, line);
                // check if search was cancelled due to deadline expired
                if (search_canceled){                       // Only realistically possible at first loop, else we should probably have got the answer from poll at jobExecutor in time
                    Header header(END_OF_MESSAGES, 0);
                    write(answer_fd, &header, sizeof(Header));
                    delete[] pls;
                    delete[] query_involves_lines;
                    delete[] response_str;
                    return;
                }   // else search was finished in time
                // write a response header in the answer pipe
                Header response(SEARCH, msg_size);
                write(answer_fd, &response, sizeof(Header));
                // and then write the whole output string for this line
                write(answer_fd, response_str, msg_size);
                delete[] response_str;
            }
        }
        delete[] query_involves_lines;
    }
    if (worker_must_terminate){
        delete[] pls;
        return;
    }
    // update log:
    for (int i = 0 ; i < query_size ; i++) {         // for each query word find which files contain it
        if ( pls[i] != NULL ){                       // if successful search
            log << get_current_time() << " : search : " << query[i] << " :";
            int fileCount = map->getFileCount();
            bool *involved_files = new bool[fileCount];
            for (int j = 0 ; j < fileCount ; j++){
                involved_files[j] = false;
            }
            pls[i]->involves_files(involved_files, fileCount);
            for (int j = 0 ; j < fileCount ; j++){
                if ( involved_files[j] ){
                    log << ' ' << map->getFilePath(j);
                }
            }
            log << endl;
            delete[] involved_files;
        }
        else{     // unsuccessful search -> write a log with no pathname
            log << get_current_time() << " : search : " << query[i] << " : -" << endl;
            // "-" denotes that the word was not found in the worker's text files
        }
    }
    delete[] pls;
    // tell jobExecutor to not expect any more lines
    Header header(END_OF_MESSAGES, 0);
    write(answer_fd, &header, sizeof(Header));
}


/* Signal Handlers Implemenation */
static void stop_searching(int sig){
    search_canceled = true;
}


static void handle_sig_termination(int sig){       // signal handler for when terminated via signal
    worker_must_terminate = true;                  // the worker will try to terminate asap
}

