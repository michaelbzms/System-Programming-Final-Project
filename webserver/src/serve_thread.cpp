#include <iostream>
#include <pthread.h>
#include <cstdio>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include "../headers/serve_thread.h"
#include "../headers/ServeRequestBuffer.h"


using namespace std;


#define MAX_GET_REQUEST_BUFFER_LEN 1024    // code assumes that no request bigger than this will be received. If we do get a bigger one, we will ignore any "overflown" data
#define BUFFER_SIZE 4096                   // size of the buffer used tp read from files chunk-by-chunk
#define HTTP_GET_READ_BUF_SIZE 256         // size of the buffer used to read the http get header

/* useful macros */
#define CHECK_PERROR(call, callname, handle_code) { if ( ( call ) < 0 ) { perror(callname); handle_code } }
#define CHECK(call, callname, handle_code) { if ( ( call ) < 0 ) { cerr << (callname) << " failed" << endl; handle_code } }


/* Global variables */
extern char *root_dir;
extern ServeRequestBuffer *serve_request_buffer;
extern pthread_cond_t bufferIsReady;
extern bool server_must_terminate;
extern pthread_mutex_t stat_lock;
extern unsigned int total_pages_returned;
extern unsigned long long int total_bytes_returned;


/* Local functions */
bool check_if_valid(char *http_request_str, char *&filename);           // checks if http get request header is valid (<=> 1. 1st line is "HTTP/1.1 GET <link>", 2. There is a "Host:" field, 3. Every field header ends in ':')
char *get_current_time(struct tm *timestamp, char *date_and_time);      // returns a pointer to date_and_time argument which is filled with current time information according to the RFC protocol for TCP
size_t bufferlen(const char *buf);                      // returns BUFFER_SIZE except if it comes across a '\0' in which case it returns the size of the string before it without it


void *handle_http_requests(void *arguements){
    int request_fd;                                     // the file descriptor that is popped from the queue on every loop
    struct tm timestamp;                                // each thread has its own, to be used by gmtime_r on get_current_time()
    char date_and_time[256];                            // ^^ (same)
    while (!server_must_terminate){
        // try to pop a file descriptor from the serve_request_buffer, if not possible block
        serve_request_buffer->acquire();                // lock the mutex
        while ( serve_request_buffer->isEmpty() ){
            CHECK( pthread_cond_wait(&bufferIsReady, &serve_request_buffer->lock) , "pthread_cond_wait" , )
            if ( server_must_terminate ){               // if notified because server must terminate then exit by breaking this and then the outer while loop
                serve_request_buffer->release();        // unlock the mutex
                break;
            }
        }
        if ( server_must_terminate ) break;             // break again to exit the outer while loop
        request_fd = serve_request_buffer->pop();       // pop next file descriptor from buffer
        if ( request_fd < 0 ) cerr << "Warning: could not pop an element from the request buffer even though it should not be empty" << endl;
        serve_request_buffer->release();                // unlock the mutex

        // read http get request from popped file descriptor
        char http_request_str[MAX_GET_REQUEST_BUFFER_LEN];
        int i = 0;
        ssize_t nbytes = 0;
        bool stop = false, previous_chunk_ends_in_endl = false;       // the 2nd one is in case our "buffering" "cuts" the "\n\n" or "\n\r\n" in different chunks
        while ( !stop && i < MAX_GET_REQUEST_BUFFER_LEN - 1 && ( nbytes = read(request_fd, http_request_str + i, HTTP_GET_READ_BUF_SIZE) ) > 0 ){
            if (previous_chunk_ends_in_endl && ( http_request_str[i] == '\n' || (http_request_str[i] == '\r' && nbytes > 1 && http_request_str[i+1] == '\n') ) ){
                i += nbytes;
                break;
            } else if (nbytes > 1) previous_chunk_ends_in_endl = false;   // reset this
            // search bytes read for "\n\n" or (\r)"\n\r\n", which signals the end of the HTTP GET request
            for (int j = 0 ; j < nbytes - 1 ; j++){
                if ( http_request_str[i+j] == '\n' && (http_request_str[i+j+1] == '\n' || (http_request_str[i+j+1] == '\r' && j < nbytes - 2 && http_request_str[i+j+2] == '\n') ) ){
                    stop = true;
                    break;
                }
            }
            if ( http_request_str[i+nbytes-1] == '\n' || (nbytes > 1 && http_request_str[i+nbytes-2] == '\n' && http_request_str[i+nbytes-1] == '\r') ){
                previous_chunk_ends_in_endl = true;
            }
            i += nbytes;
        }
        http_request_str[i] = '\0';
        if ( i == MAX_GET_REQUEST_BUFFER_LEN ){ cerr << "Warning: Might not have read full HTTP GET request due to buffer size overflow" << endl; }
        if ( nbytes < 0 ){ perror("read on serve socket"); close(request_fd); continue; }
        CHECK_PERROR( shutdown(request_fd, SHUT_RD), "shutdown read from accepted serving socket", )   // wont read any more data

        // handle http get request gotten
        char *filename = NULL;
        bool valid = check_if_valid(http_request_str, filename);    // this also returns the filename to be used if valid
        if ( !valid ){           // invalid HTTP GET request
            // answer with a 400 bad request response
            char message[1024];
            sprintf(message, "HTTP/1.1 400 Bad Request\nDate: %s\nServer: myhttpd/1.0.0 (Ubuntu64)\nContent-Length: %zu\nContent-Type: text/html\nConnection: Closed\n\n<html>Sorry bro, I can only handle HTTP GET requests.</html>\n", get_current_time(&timestamp, date_and_time), sizeof("<html>Sorry bro, I can only handle HTTP GET requests.</html>\n"));
            CHECK_PERROR( write(request_fd, message, strlen(message) + 1) , "write to serving socket" , )
        }
        else {
            char *filepath = new char[strlen(root_dir) + strlen(filename) + 1];
            strcpy(filepath, root_dir);                    // (!) root_dir should NOT have a "/" at the end (dealt with at command line parameter parsing)
            strcat(filepath, filename);                    // because filename should have a "/" at the start
            delete[] filename;
            cout << "serving port received a request for " << filepath << endl;
            FILE *page = fopen(filepath, "r");             // fopen is thread safe
            if (page == NULL) {
                if (errno == EACCES) {                     // did not have permission for the requested file
                    // answer with a 403 http response
                    char message[512];
                    sprintf(message, "HTTP/1.1 403 Forbidden\nDate: %s\nServer: myhttpd/1.0.0 (Ubuntu64)\nContent-Length: %zu\nContent-Type: text/html\nConnection: Closed\n\n<html>Trying to access this file but I do not think can make it.</html>\n", get_current_time(&timestamp, date_and_time), sizeof("<html>Trying to access this file but I do not think can make it.</html>\n"));
                    CHECK_PERROR(write(request_fd, message, strlen(message) + 1), "write to serving socket",)
                } else if (errno == ENOENT) {              // requested file does not exist
                    // answer with a 404 http response
                    char message[512];
                    sprintf(message, "HTTP/1.1 404 Not Found\nDate: %s\nServer: myhttpd/1.0.0 (Ubuntu64)\nContent-Length: %zu\nContent-Type: text/html\nConnection: Closed\n\n<html>Sorry dude, could not find this file.</html>\n", get_current_time(&timestamp, date_and_time), sizeof("<html>Sorry dude, could not find this file.</html>\n"));
                    CHECK_PERROR(write(request_fd, message, strlen(message) + 1), "write to serving socket",)
                } else {
                    perror("Error at fopening a requested page");
                }
            } else {
                // get html's file size
                long content_length = 0;
                long pos = ftell(page);              // current position
                fseek(page, 0, SEEK_END);            // go to end of file
                content_length = ftell(page);        // read the position which is the size of the file
                fseek(page, pos, SEEK_SET);          // restore original position
                // write the 200 OK response header with the appropriate content_length
                char header[1024];
                sprintf(header, "HTTP/1.1 200 OK\nDate: %s\nServer: myhttpd/1.0.0 (Ubuntu64)\nContent-Length: %zu\nContent-Type: text/html\nConnection: Closed\n\n", get_current_time(&timestamp, date_and_time), content_length);
                CHECK_PERROR( write(request_fd, header, strlen(header)), "write to serving socket", break; );
                // write the page itself chunk-by-chunk using a buffer
                char buffer[BUFFER_SIZE];
                size_t bytes_read;
                for (;;) {                                // read file using a buffer and create a list of chunks that make up the requested page
                    CHECK_PERROR( (bytes_read = fread(buffer, 1, BUFFER_SIZE, page)), "read from page's html file", break; )
                    if ( bytes_read < BUFFER_SIZE )  // if read less than BUFFER_SIZE data then
                        buffer[bytes_read] = '\0';   // put a '\0' at the end so that bufferlen will "save us" from writting garbage from a previous read (useful for last write)
                    if ( bytes_read > 0 ) {
                        // bufferlen guarantees that (nor the firsts nor) the last following writes will contain a '\0' at the end. We do not want a '\0' sent over the socket.
                        CHECK_PERROR( write(request_fd, buffer, bufferlen(buffer)), "write to serving socket", break; )
                    }
                    if ( bytes_read < BUFFER_SIZE ) {
                        break;
                    }
                }

                // update statistics (consistently using their lock)
                CHECK( pthread_mutex_lock(&stat_lock), "pthread_mutex_lock",  )
                total_pages_returned++;
                total_bytes_returned += content_length;
                CHECK( pthread_mutex_unlock(&stat_lock), "pthread_mutex_unlock",  )

                fclose(page);
            }
            delete[] filepath;
        }

        // close the accepted TCP serving connection
        CHECK_PERROR(close(request_fd) , "closing serving socket from a thread" , )
    }
    return NULL;
}


/* Local Functions Implementation */
bool check_if_valid(char *http_request_str, char *&filename) {   // This function is a bit messy but it works for all scenarios I checked it on
    char *rest = http_request_str;
    bool host_field_exists = false;
    char *line;
    char *word;
    int k = 0;
    while ((line = strtok_r(rest, "\r\n", &rest))) {             // strtok_r is thread safe
        if (k == 0) {                                            // check if first line is an HTTP GET line
            word = &line[0];
            int j = 0;
            size_t length =  strlen(line);
            for (int i = 0; i < length; i++) {
                if (line[i] == ' ' || line[i] == '\t') {                   // then word now points at the beginning of the word that just ended
                    line[i++] = '\0';
                    while (line[i] == ' ' || line[i] == '\t') i++;         // ignore continuous whitespace
                    if (j == 0 && strcmp(word, "GET") != 0) {
                        return false;
                    } else if (j == 1) {
                        if ( strlen(word) >= 2 && word[0] == '.' && word[1] == '.' ){         // if GET message is of the form "../sitei/pagei_j.html" then
                            filename = new char[strlen(word) - 2 + 1];     // copy filename without the two starting ".."
                            int i;
                            for ( i = 2 ; word[i] != '\0' ; i++ ){
                                filename[i - 2] = word[i];
                            }
                            filename[i - 2] = '\0';
                        } else {                                           // else if GET message is of the form "/sitei/pagei_j.html" then
                            filename = new char[strlen(word) + 1];         // copy filename as is
                            strcpy(filename, word);
                        }
                        // other filename formats will probably cause an expected error later (ex 404 Not Found)
                    } else if (j >= 2) {
                        if ( line[i] == '\0' ){                // then this is the last word
                            if (strcmp(word, "HTTP/1.1") != 0){
                                delete[] filename;
                                return false;
                            }
                            j++;
                            break;
                        } else {                               // else this line has more than 3 words
                            delete[] filename;
                            return false;
                        }
                    }
                    if (line[i] != '\0') j++;
                    word = &line[i];
                } else if (i == length - 1 && strcmp(word, "HTTP/1.1") != 0){          // last word ('\0' already exists from strtok)
                    delete[] filename;
                    return false;
                }
            }
            if ( j < 2 ){                                              // line has less than 3 words
                delete[] filename;
                return false;
            }
        } else {
            word = &line[0];
            for (int i = 0; i < strlen(line); i++) {                   // only check the first word
                if (line[i] == ' ' || line[i] == '\t') {               // then word now points at the beginning of the word that just ended
                    line[i] = '\0';
                    if (strcmp(word, "Host:") == 0) {
                        host_field_exists = true;
                    } else if ( word[strlen(word) - 1] != ':' ){       // if a field name does not end on a ":"
                        delete[] filename;
                        return false;
                    }
                    break;
                }
            }
        }
        k++;
    }
    if ( k > 0 && host_field_exists ) return true;
    else{
        delete[] filename;
        return false;
    }
}


size_t bufferlen(const char *buf){
    for (size_t i = 0 ; i < BUFFER_SIZE ; i++){
        if ( buf[i] == '\0' ){
            return i;        // i (over i+1) because we do NOT want to carry the final '\0' over the socket
        }
    }
    return BUFFER_SIZE;
}


const char *getDayName(int num){
    switch (num){
        case 0: return "Sun";
        case 1: return "Mon";
        case 2: return "Tue";
        case 3: return "Wed";
        case 4: return "Thu";
        case 5: return "Fri";
        case 6: return "Sat";
        default: return "Error";
    }
}


const char *getMonthName(int num){
    switch (num){
        case 0: return "Jan";
        case 1: return "Feb";
        case 2: return "Mar";
        case 3: return "Apr";
        case 4: return "May";
        case 5: return "Jun";
        case 6: return "Jul";
        case 7: return "Aug";
        case 8: return "Sep";
        case 9: return "Oct";
        case 10: return "Nov";
        case 11: return "Dec";
        default: return "Error";
    }
}


char *get_current_time(struct tm *timestamp, char *date_and_time) {    // follows the RFC prototype
    time_t now = time(NULL);
    struct tm *timeptr = gmtime_r(&now, timestamp);                    // thread safe version of gmtime_r
    sprintf(date_and_time, "%s, %.2u %s %.4u %.2u:%.2u:%.2u GMT", getDayName(timeptr->tm_wday), timeptr->tm_mday, getMonthName(timeptr->tm_mon),
            1900 + timeptr->tm_year, timeptr->tm_hour, timeptr->tm_min, timeptr->tm_sec);
    return date_and_time;
}
