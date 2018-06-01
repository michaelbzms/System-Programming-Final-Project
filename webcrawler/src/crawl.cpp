#include <iostream>
#include <pthread.h>
#include <netinet/in.h>
#include <cstdio>
#include <unistd.h>
#include <cstring>
#include <sstream>
#include <sys/stat.h>
#include <cerrno>
#include <cstdlib>
#include <arpa/inet.h>
#include <netdb.h>
#include "../headers/FIFO_Queue.h"
#include "../headers/crawl.h"
#include "../headers/str_history.h"


using namespace std;


#define BUFFER_SIZE 1024
#define MAX_LINK_SIZE 512
#define HEADER_READ_BUF_SIZE 128
#define MAX_HEADER_SIZE 1024


/* useful macros */
#define CHECK_PERROR(call, callname, handle_code) { if ( ( call ) < 0 ) { perror(callname); handle_code } }
#define CHECK(call, callname, handle_code) { if ( ( call ) < 0 ) { cerr << (callname) << " failed" << endl; handle_code } }
#define MIN(A , B) ( ((A) < (B)) ? (A) : (B) )


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
int parse_url(const char *possibly_full_url,char *&root_relative_url, char *&host_or_IP, char *&port_number_str);
void crawl_for_links(char *filepath, const sockaddr_in &server_sa);
sockaddr_in look_up_addr_r(const char *host_or_IP, int port);


void *crawl(void *arguement){
    const struct sockaddr_in &server_sa = *(((struct args *) arguement)->server_sa);
    while (!threads_must_terminate) {
        char *root_relative_url, *possibly_full_url = NULL;    // urls in the Queue could be both full http://... links and root relaive links depending on what we find
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

            /// what if broadcast (from the monitor thread) happens after condition "!threads_must_terminate" and before pthread_cond_wait?
            /// Would we be stuck on cond_wait? -> NO because broadcast happens with the urlQueue's mutex locked!
            CHECK(pthread_cond_wait(&QueueIsEmpty, &urlQueue->lock), "pthread_cond_wait",)

            num_of_threads_blocked--;
            if (threads_must_terminate) {
                urlQueue->release();                    // unlock the mutex
                break;
            }
        }
        if (threads_must_terminate) break;              // break again to exit the outer while loop
        possibly_full_url = urlQueue->pop();
        urlQueue->release();
        if ( possibly_full_url == NULL ){               // should not happen
            cerr << "Warning: A thread tried to pop from empty FIFO Queue" << endl;
        } else {
            // parse possibly_full_url to make root_relative_url point to the root_relative part of the first one
            char *host_or_IP = NULL, *port_str = NULL;
            int result;
            CHECK((result = parse_url(possibly_full_url, root_relative_url, host_or_IP, port_str)), "could not parse a url in the urlQueue",delete[] possibly_full_url; continue; )
            if (result == 1) {
                cerr << "Warning: popped a url from the urlQueue which is neither root relative nor an http:// link. Ignoring it..." << endl;
                delete[] possibly_full_url;
                continue;
            }

            // if possibly_full_url contained a host (name or IP) and a port (no port means 8080) then instead of a socket to
            // server_sa we should make one to the sockaddr for the host on the possibly_full_url
            struct sockaddr_in *server_to_query = NULL;
            struct sockaddr_in host_sa;
            host_sa.sin_family = AF_INET;
            if (host_or_IP != NULL) {                        // if link is a full http link, then redo address resolution
                int port_num;
                if (port_str == NULL) {
                    port_num = 8080;
                }
                else {
                    port_num = atoi(port_str);
                    delete[] port_str;
                }
                host_sa.sin_port = htons(port_num);
                struct in_addr in;
                memset(&in, 0, sizeof(in));
                if (inet_aton(host_or_IP, &in) == 1) {       // try parsing host_or_IP as an IP
                    host_sa.sin_addr = in;
                } else {                                     // else if unsuccessful then host_or_IP must be a host and not an IP after all
                    struct hostent *lookup;
                    struct hostent hostent;
                    int h_errnop = 0, ret_val;
                    size_t bufferlen = 1024;
                    char *work_buffer = new char[bufferlen];
                    do {
                        ret_val = gethostbyname_r(host_or_IP, &hostent, work_buffer, bufferlen, &lookup, &h_errnop);
                        if (h_errnop == ERANGE) {
                            bufferlen *= 2;
                            delete[] work_buffer;
                            work_buffer = new char[bufferlen];
                        }
                    } while ((h_errnop == TRY_AGAIN || h_errnop == ERANGE));
                    if (lookup == NULL || ret_val < 0) {
                        cout << "Could not find host (DNS failed) for host: " << host_or_IP << endl;
                        delete[] work_buffer;
                        delete[] possibly_full_url;
                        delete[] host_or_IP;
                        continue;
                    } else{
                        host_sa.sin_addr = *((struct in_addr *) lookup->h_addr);        // arbitrarily pick the first address
                    }
                    delete[] work_buffer;
                }
                delete[] host_or_IP;
                server_to_query = &host_sa;
                if ( server_to_query->sin_addr.s_addr != server_sa.sin_addr.s_addr ){   // if this link was for a different server than the one we got from the command line
                    cout << "> Web crawler found an http URL for a different server than the one in its arguments!" << endl
                         << "  URL: " << possibly_full_url << endl
                         << "  This link will NOT be downloaded nor crawled as this crawler only supports links for the given server" << endl;
                    delete[] possibly_full_url;
                    continue;
                }
            } else {                                         // else if link is root-relative then assume this link is for our server (the one got from command line)
                if (port_str != NULL) delete[] port_str;     // just in case
                server_to_query = (struct sockaddr_in *) &server_sa;
            }
            // After this points any HTTP GET request will be done our server (server_to_query must be == to server_sa
            if ( server_to_query->sin_addr.s_addr != server_sa.sin_addr.s_addr || server_to_query->sin_port != server_sa.sin_port ){
                cerr << "Unexpected warning: server_to_query was not server_sa!!!" << endl;
                server_to_query->sin_addr.s_addr = server_sa.sin_addr.s_addr;
                server_to_query->sin_port != server_sa.sin_port;
            }

            // send http get request for "root_relative_url"
            int http_socket;
            CHECK_PERROR( (http_socket = socket(AF_INET, SOCK_STREAM, 0)) , "socket", delete[] possibly_full_url; continue; )
            CHECK_PERROR( connect(http_socket, (struct sockaddr *) server_to_query, sizeof(struct sockaddr_in)) , "connect", delete[] possibly_full_url; close(http_socket); continue; )
            char request[512];
            sprintf(request, "GET %s HTTP/1.1\nHost: webcrawler\nAccept-Language: en-us\nConnection: Close\n\n", root_relative_url);
            CHECK_PERROR( write(http_socket, request, strlen(request)), "write", delete[] possibly_full_url; close(http_socket); continue; )

            CHECK_PERROR( shutdown(http_socket, SHUT_WR), "shutdown write end of http socket", )    // wont write any more data into socket

            // read http response header (should not be more than MAX_HEADER_SIZE Bytes)
            char response_header[MAX_HEADER_SIZE];
            size_t i = 0, finish_pos = 0;        // if read content then that starts from response_header[finish_pos]
            ssize_t nbytes = 0;
            bool header_finished = false, previous_chunk_ends_in_endl = false;
            while ( !header_finished && i < MAX_HEADER_SIZE - 1 && (nbytes = read(http_socket, response_header + i, MIN(HEADER_READ_BUF_SIZE, MAX_HEADER_SIZE - i))) > 0 ){
                if (previous_chunk_ends_in_endl && response_header[i] == '\n' ) {
                    finish_pos = i+1;
                    i += nbytes;
                    break;
                } else if ( previous_chunk_ends_in_endl && response_header[i] == '\r' && nbytes > 1 && response_header[i+1] == '\n' ) {
                    finish_pos = i+2;
                    i += nbytes;
                    break;
                } else if (nbytes > 1) previous_chunk_ends_in_endl = false;   // reset this
                // check nbytes read for "/n/n" or "/n/r/n"
                for (size_t j = i ; j < i + nbytes - 1 ; j++){
                    if ( response_header[j] == '\n' && response_header[j+1] == '\n' ){
                        header_finished = true;
                        finish_pos = j+2;
                        break;
                    } else if ( response_header[j] == '\n' && response_header[j+1] == '\r' && j < i + nbytes - 2 && response_header[j+2] == '\n' ){
                        header_finished = true;
                        finish_pos = j+3;
                        break;
                    }
                }
                if ( response_header[i+nbytes-1] == '\n' || (nbytes > 1 && response_header[i+nbytes-2] == '\n' && response_header[i+nbytes-1] == '\r') ){
                    previous_chunk_ends_in_endl = true;
                }
                i += nbytes;
            }
            response_header[i] = '\0';
            if ( i == MAX_HEADER_SIZE ) { cerr << "Warning: crawler thread might not have read the entire http response header" << endl; }
            if ( nbytes < 0 ){ perror("read on from server's socket"); delete[] possibly_full_url; close(http_socket); continue; }

            char first_content[HEADER_READ_BUF_SIZE];    // won't be more than this
            first_content[0] = '\0';
            // if finish_pos == i  then read the whole header and nothing from the content, else if:
            if ( finish_pos < i ){                       // then read some of the content whilst reading the header
                strcpy(first_content, response_header + finish_pos);       // copy that content to first_content char[]
                response_header[finish_pos] = '\0';      // and "remove" it from response_header
            }

            // parse response
            int content_length = -1;
            int fb = parse_http_response(response_header, content_length);
            switch (fb){
                case -1: cerr << "Error reading C String from string stream" << endl; break;
                case -2: cerr << "Unexpected http response format from server" << endl; break;
                case -3: cout << "A thread requested a root_relative_url from the server that does not exist: " << root_relative_url << endl; break;
                case -4: cerr << "Warning: server's http response did not contain a \"Content-Length\" field" << endl; break;
            }
            if (fb == -3) { delete[] possibly_full_url; close(http_socket); continue; }     // if link the root_relative_url was invalid then go to the next loop (!)
            // if threads continues here then the page we requested exists and will be downloaded

            // save response's content in save_dir - root_relative_url must be root-relative from now on
            create_subdir_if_necessary(root_relative_url);               // create the "sitei" folder for the given str's page if it doesn't exist. Also adds directory to the alldirs struct.
            char *filepath = new char[strlen(save_dir) + strlen(root_relative_url) + 1];
            strcpy(filepath, save_dir);                    // save dir is guaranted NOT to have a '/' at the end
            strcat(filepath, root_relative_url);                         // whilst "str" SHOULD have a '/' at the start
            delete[] possibly_full_url;                                  // dont need root_relative_url any more
            FILE *page = fopen(filepath, "w");             // fopen is thread safe - file is created if it doesnt exist, else overwritten
            int total_bytes_read = 0;

            if ( first_content[0] != '\0' ){               // if a starting part of the content was read whilst reading the header then
                size_t first_content_len = strlen(first_content);
                total_bytes_read += first_content_len;
                if ( fwrite(first_content, 1, first_content_len, page) < first_content_len ) { cerr << "Warning fwrite did not write all bytes" << endl; }
            }

            char buffer[BUFFER_SIZE];
            if (page != NULL) {
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
            crawl_for_links(filepath, server_sa);

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


void create_subdir_if_necessary(const char *url) {      // find out and create dir if it doesn't exist - urls MUST be root relative and of the form "/site/page" for this function to work!
    // figure out the site directory for given root-relative url
    size_t k, url_len = strlen(url), save_dir_len = strlen(save_dir);
    char *subdir = new char[url_len + save_dir_len + 1];
    strcpy(subdir, save_dir);
    subdir[save_dir_len] = '/';
    for (k = 1 ; k < url_len && url[k] != '/' ; k++){   // k = 1 -> skip first '/'
        subdir[k + save_dir_len] = url[k];
    }
    subdir[k + save_dir_len] = '\0';

    // ATOMICALLY add subdir to alldirs but only if it's not already in it (add ensures that - no need to search separately)
    alldirs->add(subdir);

    struct stat st = {0};
    if (stat(subdir, &st) == -1) {                     // if dir does not exist, then create it
        CHECK_PERROR( mkdir(subdir, 0755), "mkdir", )
    }
    delete[] subdir;
}


int parse_url(const char *possibly_full_url,char *&root_relative_url, char *&host_or_IP, char *&port_number_str){
    if (possibly_full_url == NULL){
        cerr << "Warning: NULL url on urlQueue" << endl;
        root_relative_url = NULL;
        host_or_IP = NULL;
        port_number_str = NULL;
        return -1;
    }
    size_t urlLen = strlen(possibly_full_url);
    if (urlLen == 0){        // should not happen
        cerr << "Warning: empty ("") url on urlQueue" << endl;
        root_relative_url = NULL;
        host_or_IP = NULL;
        port_number_str = NULL;
        return -2;
    }
    if (possibly_full_url[0] == '/'){      // root relative link
        host_or_IP = NULL;
        port_number_str = NULL;
        root_relative_url = (char *) possibly_full_url;
        return 0;
    } else {                               // full url should be of type: "http://host_or_IP:port/<root_relative_url>", where <root_relative_url> = sitei/pagei_j.html for our own sites
        int i, slashesCounter = 0;
        int host_or_ip_start = -1, host_or_ip_end = -1;
        int port_start = -1, port_end = -1;
        for (i = 0 ; slashesCounter < 3 && i < urlLen ; i++){
            if ( possibly_full_url[i] == '/' ){
                slashesCounter++;
                if (slashesCounter == 2){         // start of host_or_IP field
                    host_or_ip_start = i+1;
                }
                else if (slashesCounter == 3){    // end of port field
                    if (port_start != -1) port_end = i-1;
                    else host_or_ip_end = i-1;
                }
            }
            else if ( slashesCounter >= 2 && possibly_full_url[i] == ':' ){
                host_or_ip_end = i-1;
                port_start = i+1;
            }
        }
        if (slashesCounter < 3 ) {
            root_relative_url = NULL;
            host_or_IP = NULL;
            port_number_str = NULL;
            return 1;
        }
        if (host_or_ip_start == -1 || host_or_ip_end == -1){    // should not happen
            cerr << "Unexpected parsing error of http url" << endl;
            root_relative_url = NULL;
            host_or_IP = NULL;
            port_number_str = NULL;
            return -3;
        } else{
            host_or_IP = new char[host_or_ip_end + 2 - host_or_ip_start];
            int k = 0;
            for (int j = host_or_ip_start ; j <= host_or_ip_end ; j++){
                host_or_IP[k++] = possibly_full_url[j];
            }
            host_or_IP[k] = '\0';
        }
        if ( port_start == -1 || port_end == -1 ){
            port_number_str = NULL;                            // port number will get the default 8080 value later
        } else{
            port_number_str = new char[port_end + 2 - port_start];
            int k = 0;
            for (int j = port_start ; j <= port_end ; j++){
                port_number_str[k++] = possibly_full_url[j];
            }
            port_number_str[k] = '\0';
        }
        root_relative_url = ((char *) possibly_full_url) + i - 1;
        return 0;
    }
}


int findRootRelativeUrl(const char *possibly_full_url, char *&root_relative_url){
    if (possibly_full_url == NULL){
        cerr << "Warning: NULL url on urlQueue" << endl;
        root_relative_url = NULL;
        return -1;
    }
    size_t urlLen = strlen(possibly_full_url);
    if (urlLen == 0){        // should not happen
        cerr << "Warning: empty ("") url on urlQueue" << endl;
        root_relative_url = NULL;
        return -2;
    }
    if (possibly_full_url[0] == '/') {      // already root relative link
        root_relative_url = (char *) possibly_full_url;
    } else{
        int i, slashCount = 0;
        for (i = 0 ; slashCount < 3 && i < urlLen ; i++){
            if ( possibly_full_url[i] == '/' ){
                slashCount++;
            }
        }
        if (slashCount < 3){
            root_relative_url = NULL;
            cerr << "warning: possibly full url has not enough \'/\'" << endl;
            return -3;
        }
        --i;
        root_relative_url = (char *) &possibly_full_url[i];
    }
    return 0;
}

void crawl_for_links(char *filepath, const sockaddr_in &server_sa) {
    FILE *page = fopen(filepath, "r");
    if (page == NULL){
        cerr << "Error: could not open a page that was just downloaded and saved" << endl;
        return;
    }

    long pos = ftell(page);              // current position
    fseek(page, 0, SEEK_END);            // go to end of file
    long int file_length = ftell(page);  // read the position which is the size of the file
    char *webpage = new char[file_length + 1];
    fseek(page, pos, SEEK_SET);          // restore original position

    {   // load the entire webpage onto memory
        char buffer[BUFFER_SIZE];
        size_t bytes_read, k = 0;
        for (;;) {                                // read file using a buffer and create a list of chunks that make up the requested page
            CHECK_PERROR((bytes_read = fread(buffer, 1, BUFFER_SIZE, page)), "read from page's html file", break;)
            if (bytes_read > 0) {
                for (int i = 0; i < bytes_read; i++) {
                    webpage[k + i] = buffer[i];
                }
                k += bytes_read;
            }
            if (bytes_read < BUFFER_SIZE) {
                break;
            }
        }
        if (k > file_length) {
            cerr << "Warning: file size calculation was off" << endl;
            delete[] webpage;
            return;
        }
        else webpage[k] = '\0';
    }

    char link[MAX_LINK_SIZE];
    for (int i = 0 ; i < file_length && webpage[i] != '\0' ; i++) {               // read file char-by-char using a buffer
        if ( webpage[i] == '<' && i + 1 < file_length && webpage[i+1] == 'a' ){   // if came across an <a ...> tag
            i += 3;                        // skip "<a "
            while ( i < file_length && webpage[i] != '>' && !(i + 4 < file_length && webpage[i] == 'h' && webpage[i + 1] == 'r' && webpage[i + 2] == 'e' && webpage[i + 3] == 'f') )
                i++;                       // advance until href or '>'
            if (webpage[i] != '>') {       // "href" detected
                i += 4;                    // skip "href"
                while (i < file_length && webpage[i] != '>' && webpage[i] != '"') i++;               // advance until '>' or '"'
                if (webpage[i] != '>' && i+1 < file_length) {       // link starts at webpage[i+1] !
                    i++;
                    int j = 0;
                    if (webpage[i] == '.' && i+1 < file_length && webpage[i+1] == '.') i += 2;       // if link starts as "../sitei/pagei_j.html" then skip those ".."
                    while (i + j < file_length && webpage[i + j] != '"' && webpage[i + j] != '>' && j < MAX_LINK_SIZE) {
                        link[j] = webpage[i+j];
                        j++;
                    }
                    link[j] = '\0';

                    if (j > 0) {                                    // if found a link
                        // first examine link found
                        bool add_link_to_history = true;
                        char *root_relative_link;
                        findRootRelativeUrl(link, root_relative_link);
                        if ( root_relative_link == NULL ){
                            cerr << "Unexpected failure for finding the root relative link of the url: " << link << endl;
                            cerr << "Adding itself to history instead..." << endl;
                            root_relative_link = link;
                        }
                        else if ( root_relative_link != link ) {    // if link was not root_relative then examine it
                            // The root-relative links for those URLS should NOT be added to the urlHistory
                            // in case they conflict with the root_relative links for our server
                            int port, res;
                            char *host_or_IP, *port_num_str;
                            CHECK((res = parse_url(link, root_relative_link, host_or_IP, port_num_str)), "parse url while crawling for links", )
                            if (res == 1) {
                                // cerr << "Warning: found a link on a page which is neither root relative nor an http:// link. Ignoring it..." << endl;
                                // it's ok to add this link to urlQueue, it will be recognized as a false link when we pop it
                                add_link_to_history = false;        // but do not add it to urlHistory
                            } else if (host_or_IP != NULL ) {
                                if (port_num_str == NULL) port = 8080;   // default port
                                else port = atoi(port_num_str);
                                sockaddr_in lookedUpAddr = look_up_addr_r(host_or_IP, port);
                                if (lookedUpAddr.sin_port != server_sa.sin_port || lookedUpAddr.sin_addr.s_addr != server_sa.sin_addr.s_addr) {
                                    // if link is not for the server specified in the command line arguments then do not add it to history
                                    add_link_to_history = false;
                                }
                                delete[] host_or_IP;
                                delete[] port_num_str;
                            }
                        }

                        // then add it to urlQueue and, if the host of the link is the given server, to urlHistrory as well
                        urlQueue->acquire();                        // only one thread is allowed on the following critical section
                        // IMPORTANT: acquiring Queue's lock BEFORE the next if check avoids the race condition of two threads finding the same link simultaneously!
                        // If I didn't use Queue's lock for this I would have to use History's lock for both, search and add, TOGETHER!
                        if (!urlHistory->search(root_relative_link)) {   // add link to urlQueue ONLY if it doesn't exist on our urlHistory structure (aka we have not downloaded this page yet)
                            if (add_link_to_history) urlHistory->add(root_relative_link);           // ATOMICALLY add new found link to our urlHistory struct
                            urlQueue->push(link);
                            CHECK(pthread_cond_signal(&QueueIsEmpty), "pthread_cond_signal",)       // signal ONE thread to read the new link from the urlQueue
                            cout << "added a link: " << link << endl;
                        }
                        urlQueue->release();
                    }

                    i += j;       // skip link and the 2nd '"' (or the '>')
                    while ( i < file_length && webpage[i] != '>' ) i++;   // ignore the rest of the tag until it closes
                }
            }
        }
    }
    delete[] webpage;
    CHECK_PERROR(fclose(page), "fclose",)
}

sockaddr_in look_up_addr_r(const char *host_or_IP,int port){
    sockaddr_in resolved_address;
    resolved_address.sin_family = AF_INET;
    resolved_address.sin_port = htons(port);
    struct in_addr in;
    memset(&in, 0, sizeof(in));
    if (inet_aton(host_or_IP, &in) == 1) {       // try parsing host_or_IP as an IP
        resolved_address.sin_addr = in;
    } else {                                     // else if unsuccessful then host_or_IP must be a host and not an IP after all
        struct hostent *lookup;
        struct hostent hostent;
        int h_errnop = 0, ret_val;
        size_t bufferlen = 1024;
        char *work_buffer = new char[bufferlen];
        do {
            ret_val = gethostbyname_r(host_or_IP, &hostent, work_buffer, bufferlen, &lookup, &h_errnop);
            if (h_errnop == ERANGE) {
                bufferlen *= 2;
                delete[] work_buffer;
                work_buffer = new char[bufferlen];
            }
        } while ((h_errnop == TRY_AGAIN || h_errnop == ERANGE));
        if (lookup == NULL || ret_val < 0) {
            cout << "Could not find host (DNS failed) for host: " << host_or_IP << endl;
            resolved_address.sin_addr.s_addr = 0;
        } else{
            resolved_address.sin_addr = *((struct in_addr *) lookup->h_addr);        // arbitrarily pick the first address
        }
        delete[] work_buffer;
    }
    return resolved_address;
}
