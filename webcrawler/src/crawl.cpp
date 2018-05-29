#include <iostream>
#include <pthread.h>
#include <netinet/in.h>
#include <cstdio>
#include <unistd.h>
#include <cstring>
#include <sstream>
#include <sys/stat.h>
#include <cerrno>
#include "../headers/FIFO_Queue.h"
#include "../headers/crawl.h"
#include "../headers/str_history.h"


using namespace std;


#define BUFFER_SIZE 1024
#define MAX_LINK_SIZE 512


/* useful macros */
#define CHECK_PERROR(call, callname, handle_code) { if ( ( call ) < 0 ) { perror(callname); handle_code } }
#define CHECK(call, callname, handle_code) { if ( ( call ) < 0 ) { cerr << (callname) << " failed" << endl; handle_code } }


/* Global Variables */
extern pthread_mutex_t stat_lock;
extern unsigned int total_pages_downloaded;
extern unsigned int total_bytes_downloaded;
extern char *save_dir;
extern pthread_cond_t QueueIsEmpty;
extern FIFO_Queue *urlQueue;
extern bool threads_must_terminate;
extern int num_of_threads_blocked;           // number of threads blocked on cond wait
extern str_history *urlHistory;
extern bool crawling_has_finished;
extern pthread_cond_t crawlingFinished;
extern pthread_mutex_t crawlingFinishedLock;
extern str_history *alldirs;


/* Local Functions */
int parse_http_response(const char *response, int &content_length);
void create_subdir_if_necessary(const char *url);
void crawl_for_links(char *filepath);


void *crawl(void *arguement){
    const struct sockaddr_in &server_sa = *(((struct args *) arguement)->server_sa);
    while (!threads_must_terminate) {
        char *url;
        urlQueue->acquire();
        while (urlQueue->isEmpty() && !threads_must_terminate) {
            num_of_threads_blocked++;

            // calculate if crawling has finished in order to signal the monitor thread (will only be true for the last thread to block on cond_wait)
            CHECK( pthread_mutex_lock(&crawlingFinishedLock) , "pthread_mutex_lock", )         // lock crawling_has_finished's mutex
            crawling_has_finished = urlQueue->isEmpty() && num_of_threads_blocked == ((struct args *) arguement)->num_of_threads;
            if (crawling_has_finished) {                     // only signal if it web crawling has actually finished
                CHECK(pthread_cond_signal(&crawlingFinished), "pthread_cond_signal to crawlingFinished cond_t",)
            }
            CHECK( pthread_mutex_unlock(&crawlingFinishedLock) , "pthread_mutex_unlock", )   // unlock crawling_has_finished's mutex

            /// what if broadcast (from the monitor thread) happens after condition "!threads_must_terminate" and before pthread_cond_wait? Would we be stuck on cond_wait? -> NO because broadcast happens with the url mutex locked!
            CHECK(pthread_cond_wait(&QueueIsEmpty, &urlQueue->lock), "pthread_cond_wait",)

            num_of_threads_blocked--;
            if (threads_must_terminate) {
                urlQueue->release();                    // unlock the mutex
                break;
            }
        }
        if (threads_must_terminate) break;              // break again to exit the outer while loop
        url = urlQueue->pop();
        urlQueue->release();
        if ( url == NULL ){                             // should not happen
            cerr << "Warning: A thread tried to pop from empty FIFO Queue" << endl;
        } else{
            // send http get request for "str"
            int http_socket;
            CHECK_PERROR( (http_socket = socket(AF_INET, SOCK_STREAM, 0)) , "socket",  continue; )
            CHECK_PERROR( connect(http_socket, (struct sockaddr *) &server_sa, sizeof(server_sa)) , "connect", close(http_socket); continue; )
            char request[512];
            sprintf(request, "GET %s HTTP/1.1\nHost: webcrawler\nAccept-Language: en-us\nConnection: Close\n\n", url);
            CHECK_PERROR( write(http_socket, request, strlen(request)), "write", close(http_socket); continue; )

            CHECK_PERROR( shutdown(http_socket, SHUT_WR), "shutdown write end of http socket", )    // wont write any more data into socket

            // read http response header (should not be more than 1024 Bytes
            char response_header[1024];
            int i = 0, backslashN_counter = 0;
            while ( i < 1023 && backslashN_counter < 2 && read(http_socket, response_header + i, 1) > 0 ){      // read char-by-char the response header until blank line ("\n\n" or "\n\r\n")
                if ( response_header[i] == '\n' ) { backslashN_counter++; }
                else if ( response_header[i] != '\r' ) { backslashN_counter = 0; }
                i++;
            }
            response_header[i] = '\0';                      // (!) important
            int content_length = -1;

            // parse response
            int fb = parse_http_response(response_header, content_length);
            switch (fb){
                case -1: cerr << "Error reading C String from string stream" << endl; break;
                case -2: cerr << "Unexpected http response format from server" << endl; break;
                case -3: cout << "A thread requested a str from the server that does not exist" << endl; break;
                case -4: cerr << "Warning: server's http response did not contain a \"Content-Length\" field" << endl; break;
            }

            // save response's content in save_dir
            create_subdir_if_necessary(url);               // create the "sitei" folder for the given str's page if
            char *filepath = new char[strlen(save_dir) + strlen(url) + 1];
            strcpy(filepath, save_dir);                    // save dir is guaranted NOT to have a '/' at the end
            strcat(filepath, url);                         // whilst "str" SHOULD have a '/' at the start
            delete[] url;                                  // dont need "str" any more
            FILE *page = fopen(filepath, "w");             // fopen is thread safe - file is created if it doesnt exist, else overwritten
            int total_bytes_read = 0;
            if (page != NULL) {
                char buffer[BUFFER_SIZE];
                ssize_t bytes_read;
                // read content_length Bytes from server
                while ( total_bytes_read < content_length  && (bytes_read = read(http_socket, buffer, BUFFER_SIZE)) > 0) {     // (!) could fail with errno ECONNRESET when the server closes the connection
                    total_bytes_read += bytes_read;
                    if ( fwrite(buffer, 1, bytes_read, page) < bytes_read ) { cerr << "Warning fwrite did not write all bytes" << endl; }
                }
                if (bytes_read < 0 && errno == ECONNRESET ) { perror("Warning, client did not download the whole page in time"); }      // should not happen because server's socket should be configured to linger at close
                else if (bytes_read < 0){ perror("Warning, reading content from http socket"); }
                CHECK_PERROR(fclose(page), "fclose",)
            } else {
                perror("Warning: A thread could not create a page file");
            }

            // close TCP connection
            CHECK_PERROR( close(http_socket) , "closing http client socket from a thread" , )

            // update stats
            CHECK( pthread_mutex_lock(&stat_lock), "pthread_mutex_lock",  )
            total_pages_downloaded++;
            total_bytes_downloaded += total_bytes_read;
            CHECK( pthread_mutex_unlock(&stat_lock), "pthread_mutex_unlock",  )

            if (threads_must_terminate) break;

            // crawl for more links in the page we just downloaded and add them to urlQueue while signalling one thread for each link added
            crawl_for_links(filepath);

            delete[] filepath;
        }
    }
    return NULL;
}



/* Local Functions Implementation */
int parse_http_response(const char *response, int &content_length){
    char *copy = new char[strlen(response) + 1];
    strcpy(copy, response);
    char *rest = copy, *line;
    bool found_content_len = false;
    {   // for the 1st line, check that the answer was "200 OK", else return error
        line = strtok_r(rest, "\r\n", &rest);
        stringstream linestream(line);
        char word[256];
        linestream >> word;
        if ( linestream.fail() ) { delete[] copy; return -1; }
        if ( strcmp(word, "HTTP/1.1") != 0 ) { delete[] copy; return -2; }
        linestream >> word;
        if ( linestream.fail() ) { delete[] copy; return -1; }
        if ( strcmp(word, "200") != 0 ) { delete[] copy; return -3; }
        linestream >> word;
        if ( linestream.fail() ) { delete[] copy; return -1; }
        if ( strcmp(word, "OK") != 0 ) { delete[] copy; return -3; }
    }
    while ((line = strtok_r(rest, "\r\n", &rest))){    // for each line after the 1st one in response header
        stringstream linestream(line);
        char word[256];
        linestream >> word;
        if (linestream.fail()){
            cerr << "Warning: reading from C string to stringstream failed" << endl;
            linestream.clear();
            continue;
        }
        if (strcmp(word, "Content-Length:") == 0){
            linestream >> content_length;
            if ( linestream.fail() ){
                cerr << "Warning: \"Content-Length:\" field had a no numeric value" << endl;
                linestream.clear();
                continue;
            }
            found_content_len = true;
        }
    }
    delete[] copy;
    return (found_content_len ? 0 : -4);
}

void create_subdir_if_necessary(const char *url) {      // find out and create dir if it doesn't exist - urls MUST be root relative!
    size_t k, url_len = strlen(url), save_dir_len = strlen(save_dir);
    char *subdir = new char[url_len + save_dir_len + 1];
    strcpy(subdir, save_dir);
    subdir[save_dir_len] = '/';
    for (k = 1 ; k < url_len && url[k] != '/' ; k++){   // k = 1 -> skip first '/'
        subdir[k + save_dir_len] = url[k];
    }
    subdir[k + save_dir_len] = '\0';

    // ATOMICALLY add subdir to alldirs but only if it's not already in it (add ensures that - no need to search seperately)
    alldirs->add(subdir);

    struct stat st = {0};
    if (stat(subdir, &st) == -1) {             // if dir does not exist, then create it
        CHECK_PERROR( mkdir(subdir, 0755), "mkdir", )
    }
    delete[] subdir;
}

void crawl_for_links(char *filepath) {
    FILE *page = fopen(filepath, "r");
    char c;
    char link[MAX_LINK_SIZE];
    size_t bytes_read;
    for (;;) {               // read file char-by-char using a buffer
        CHECK_PERROR((bytes_read = fread(&c, 1, 1, page)), "read a char from str page", break; )
        // ignore every char c you read except if it's '<' followed by an 'a' in which case a link should follow before a closing '>'
        if (bytes_read > 0) {
            if ( c == '<' ){
                bool url_is_next = false;
                int i = 0, urlcounter = 0;
                while ( c != '>' && i < MAX_LINK_SIZE ){
                    CHECK_PERROR((bytes_read = fread(&c, 1, 1, page)), "read a char from str page", break; )
                    if (bytes_read > 0) {
                        if ( c == '"' ) url_is_next = !url_is_next;           // becomes true at first '"' and false at second '"'.
                        if (url_is_next && c != '"' && ( urlcounter > 2 || c != '.')) link[i++] = c;    // ignore possible ".." at the start of the link
                        if (url_is_next && c != '"') urlcounter++;
                    }
                    else { CHECK_PERROR(fclose(page), "fclose", ) return; }   // should not happen
                }
                link[i] = '\0';
                if ( i > 0 ) {      // found a link
                    urlQueue->acquire();                          // SOS acquiring Queue's lock BEFORE the next if check avoids the race condition of two threads finging the same link simultaneously
                    if ( !urlHistory->search(link) ) {            // add link to urlQueue ONLY if it doesn't exist on our urlHistory structure (aka we have not downloaded this page yet)
                        urlHistory->add(link);
                        urlQueue->push(link);
                        CHECK(pthread_cond_signal(&QueueIsEmpty), "pthread_cond_signal",)      // signal ONE thread to read the new link from the urlQueue
                        cout << "added a link: " << link << endl;
                    }
                    urlQueue->release();
                }
            }
        } else break;     // should not happen
    }
    CHECK_PERROR(fclose(page), "fclose",)
}
