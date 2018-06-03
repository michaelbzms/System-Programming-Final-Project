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


#define MAX_LINK_SIZE 512                    // the maximum size a link can have. Overflowing links will not be saved in their entirety so set this wisely
#define BUFFER_SIZE 1024                     // buffer size for both: 1. reading from the socket to download a page, 2. reading from a page we just downloaded as a file
#define HEADER_READ_BUF_SIZE 128             // buffer size for reading the http header. We do not want this too high so as to not get much of the content along with the header
#define MAX_HEADER_SIZE 1024                 // the maximum size an http response header can have for us to support it. Overflown headers will not be saved in their entirety


/* useful macros */
#define CHECK_PERROR(call, callname, handle_code) { if ( ( call ) < 0 ) { perror(callname); handle_code } }
#define CHECK(call, callname, handle_code) { if ( ( call ) < 0 ) { cerr << (callname) << " failed" << endl; handle_code } }
#define MIN(A , B) ( ((A) < (B)) ? (A) : (B) )


/* Global Variables (explained at webcrawler.cpp) */
extern pthread_mutex_t stat_lock;
extern unsigned int total_pages_downloaded;
extern unsigned int total_bytes_downloaded;
extern char *save_dir;
extern pthread_cond_t QueueIsEmpty;
extern FIFO_Queue *urlQueue;
extern bool threads_must_terminate;
extern int num_of_threads_blocked;
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
    const struct sockaddr_in &server_sa = *(((struct args *) arguement)->server_sa);    // this is a reference to server_sa from this thread's arguements
    while (!threads_must_terminate) {
        char *root_relative_url, *possibly_full_url = NULL;      // urls in the Queue could be both full http://... links and root relaive links depending on what we find

        urlQueue->acquire();                                     // lock urlQueue's mutex
        while (urlQueue->isEmpty() && !threads_must_terminate) {
            num_of_threads_blocked++;                            // increase num_of_threads_blocked before we block

            // calculate if crawling has finished in order to signal the monitor thread (will only be true for the last thread to block on cond_wait)
            CHECK( pthread_mutex_lock(&crawlingFinishedLock) , "pthread_mutex_lock", )         // lock crawling_has_finished's mutex
            crawling_has_finished = urlQueue->isEmpty() && num_of_threads_blocked == ((struct args *) arguement)->num_of_threads;
            if (crawling_has_finished) {                         // only signal if it web crawling has actually finished
                CHECK(pthread_cond_signal(&crawlingFinished), "pthread_cond_signal to crawlingFinished cond_t",)
            }
            CHECK( pthread_mutex_unlock(&crawlingFinishedLock) , "pthread_mutex_unlock", )     // unlock crawling_has_finished's mutex

            // What if broadcast (from the monitor thread) happens after condition "!threads_must_terminate" and before pthread_cond_wait?
            // Would we be stuck on cond_wait? -> NO because broadcast happens with the urlQueue's mutex locked!
            CHECK(pthread_cond_wait(&QueueIsEmpty, &urlQueue->lock), "pthread_cond_wait",)     // block on cond_wait until signaled

            num_of_threads_blocked--;                   // when unblocked and whilst holding the mutex decrease this value
            if (threads_must_terminate) {
                urlQueue->release();                    // unlock urlQueue's mutex
                break;
            }
        }
        if (threads_must_terminate) break;              // break again to exit the outer while loop
        possibly_full_url = urlQueue->pop();            // pop a possible full http url (or a root-relative one) from the urlQueue which is guaranteed not to be empty
        urlQueue->release();                            // unlock urlQueue's mutex
        if ( possibly_full_url == NULL ){               // should not happen
            cerr << "Warning: A thread tried to pop from empty FIFO Queue" << endl;
        } else {
            // parse possibly_full_url to make root_relative_url point to the root_relative part of the first one
            char *host_or_IP = NULL, *port_str = NULL;
            int result;
            CHECK((result = parse_url(possibly_full_url, root_relative_url, host_or_IP, port_str)), "could not parse a url in the urlQueue", delete[] possibly_full_url; continue; )
            if (result == 1) {
                cerr << "Warning: popped an url from the urlQueue which is neither root relative nor an http:// link. Ignoring it..." << endl;
                delete[] possibly_full_url;
                continue;
            }

            // if possibly_full_url contained a host (name or IP) and a port (no port means 8080) then we have to check if it refers to server_sa or a different server
            // 1. If it refers to server_sa, then we go ahead and we download and crawl this page normally
            // 2. If it refers to another server, then for the purposes of this exercise we do not download nor crawl that page, rather we just print a "crawler found this link" message and ignore it
            struct sockaddr_in *server_to_query = NULL;      // the server to which we will have to query (this will end up pointing to server_sa if we are to download the page)
            struct sockaddr_in host_sa;                      // the sockaddr_in for the server in possibly_full_url
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
                    // because multiple threads are running we have to use the thread_safe version of gethostbyname here
                    struct hostent *lookup;
                    struct hostent hostent;
                    int h_errnop = 0, ret_val;
                    size_t bufferlen = 1024;
                    char *work_buffer = new char[bufferlen];
                    do {
                        ret_val = gethostbyname_r(host_or_IP, &hostent, work_buffer, bufferlen, &lookup, &h_errnop);
                        if (h_errnop == ERANGE) {           // double buffer's size and try again
                            bufferlen *= 2;
                            delete[] work_buffer;
                            work_buffer = new char[bufferlen];
                        }
                    } while ((h_errnop == TRY_AGAIN || h_errnop == ERANGE));
                    if (lookup == NULL || ret_val < 0) {    // if DNS is unsuccessful then the link we just popped was probably invalid, hence ignore it
                        cout << "Could not find host (DNS failed) for host: " << host_or_IP << endl;
                        delete[] work_buffer;
                        delete[] possibly_full_url;
                        delete[] host_or_IP;
                        continue;
                    } else{
                        host_sa.sin_addr = *((struct in_addr *) lookup->h_addr);     // arbitrarily pick the first address
                    }
                    delete[] work_buffer;
                }
                delete[] host_or_IP;
                server_to_query = &host_sa;
                // if this link was for a different server than the one we got from the command line
                if ( server_to_query->sin_addr.s_addr != server_sa.sin_addr.s_addr || server_to_query->sin_port != server_sa.sin_port ){
                    cout << "> Web crawler found an http URL for a different server (or a different port) than the one in its arguments: " << possibly_full_url << endl
                         << "  This link will NOT be downloaded nor crawled as this crawler only supports links for the given server and port" << endl;
                    delete[] possibly_full_url;
                    continue;
                }      // else server_to_query must be == to &server_sa
            } else {   // else if link is root-relative then assume this link is for our server (the one got from command line)
                if (port_str != NULL) delete[] port_str;
                server_to_query = (struct sockaddr_in *) &server_sa;
            }

            // After this points any HTTP GET request will be done to our server (this check just makes sure of that)
            if ( server_to_query->sin_addr.s_addr != server_sa.sin_addr.s_addr || server_to_query->sin_port != server_sa.sin_port ){    // should not happen
                cerr << "Unexpected warning: server_to_query was not server_sa!!!" << endl;
                server_to_query->sin_addr.s_addr = server_sa.sin_addr.s_addr;
                server_to_query->sin_port = server_sa.sin_port;
            }

            // make a TCP connection and send http get request for root_relative_url (which we got from parsing possibly_full_url) to the server_to_query
            int http_socket;
            CHECK_PERROR( (http_socket = socket(AF_INET, SOCK_STREAM, 0)) , "socket", delete[] possibly_full_url; continue; )
            CHECK_PERROR( connect(http_socket, (struct sockaddr *) server_to_query, sizeof(struct sockaddr_in)) , "Connecting to server failed", delete[] possibly_full_url; close(http_socket); continue; )
            char request[512];
            sprintf(request, "GET %s HTTP/1.1\nHost: mycrawler\nAccept-Language: en-us\nConnection: Close\n\n", root_relative_url);
            CHECK_PERROR( write(http_socket, request, strlen(request)), "write", delete[] possibly_full_url; close(http_socket); continue; )
            CHECK_PERROR( shutdown(http_socket, SHUT_WR), "shutdown write end of http socket", )    // wont write any more data into socket

            // read http response header (should not be more than MAX_HEADER_SIZE Bytes)
            char response_header[MAX_HEADER_SIZE];
            size_t i = 0, finish_pos = 0;        // if we read (part of or the whole of) content along with the header, then that starts from response_header[finish_pos]
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

            // check if we got part (or all) of the content along with the header and handle it if true
            char first_content[HEADER_READ_BUF_SIZE];    // can't be more than HEADER_READ_BUF_SIZE
            first_content[0] = '\0';
            // if finish_pos == i  then we have read the whole header and nothing from the content, else if:
            if ( finish_pos < i ){                       // then we have read some (or all) of the content whilst reading the header
                strcpy(first_content, response_header + finish_pos);          // copy that content to first_content char[]
                response_header[finish_pos] = '\0';      // and "remove" it from response_header
            }

            // parse response
            int content_length = -1;
            int fb = parse_http_response(response_header, content_length);    // this parsing will give us content_length, assuming the response was valid
            switch (fb){
                case -1: cerr << "Error: reading C String from string stream failed" << endl; break;
                case -2: cerr << "Error: Unexpected http response format from server" << endl; break;
                case -3: cout << "A thread requested a root_relative_url from the server that does not exist or the server cannot access it: " << root_relative_url << endl; break;
                case -4: cerr << "Error: server's http response did not contain a \"Content-Length\" field" << endl; break;
            }
            // if server could not give us the requested page then close the connection and go to the next loop
            if (fb == -3) { delete[] possibly_full_url; close(http_socket); continue; }     // can happen
            else if (fb < 0){    // should not happen from our server
                cerr << "Unexpected error parsing http get response. Aborting download of url: " << possibly_full_url << endl;
                delete[] possibly_full_url; close(http_socket); continue;
            }

            // Note: if thread continues here then the page we requested exists, is accessible and will be downloaded from the server

            // create the directory where the page will be saved if it doesn't already exists (if it exists we will NOT purge it, we will just overwrite that page file if it also exists)
            create_subdir_if_necessary(root_relative_url);       // Note: this function also adds directory found to the alldirs struct, which is used to pass to jobExecutor's the folders he will have to distribute to its workers

            // figure out the filepath (including its file name) for the page we will download and open it for writing
            char *filepath = new char[strlen(save_dir) + strlen(root_relative_url) + 1];
            strcpy(filepath, save_dir);                    // save dir is guaranted NOT to have a '/' at the end
            strcat(filepath, root_relative_url);           // whilst "str" SHOULD have a '/' at the start, since it is a root-relative link
            delete[] possibly_full_url;                    // don't need popped url any more
            FILE *page = fopen(filepath, "w");             // fopen is thread safe - file is created if it doesnt exist, else overwritten
            int total_bytes_read = 0;                      // we will have downloaded the whole page when total_bytes_read == content_length
            // read all content from the socket and write it to the filepath file
            if (page != NULL) {
                // if a starting part of the content was read whilst reading the header then write that part to the file first
                if ( first_content[0] != '\0' ){
                    size_t first_content_len = strlen(first_content);
                    total_bytes_read += first_content_len;
                    if ( fwrite(first_content, 1, first_content_len, page) < first_content_len ) { cerr << "Warning fwrite did not write all bytes" << endl; }
                }
                // then continue to read from the socket and write to the file BUFFER_SIZE sized chunks until all the file has been downloaded aka total_bytes_read == content_length
                char buffer[BUFFER_SIZE];
                ssize_t bytes_read;
                while ( total_bytes_read < content_length  && (bytes_read = read(http_socket, buffer, BUFFER_SIZE)) > 0) {
                    total_bytes_read += bytes_read;
                    if ( fwrite(buffer, 1, bytes_read, page) < bytes_read ) { cerr << "Warning fwrite did not write all bytes" << endl; }
                }
                if (bytes_read < 0 && errno == ECONNRESET ) { perror("Warning, client did not download the whole page in time. Peer reset the connection"); }   // our server-peer shouldn't do that
                else if (bytes_read < 0){ perror("Warning, reading content from http socket"); }
                CHECK_PERROR(fclose(page), "fclose",)
            } else {
                perror("Warning: A thread could not create a page file");
            }

            // close TCP connection
            CHECK_PERROR( close(http_socket) , "closing http client socket from a thread" , )

            // update stats (consistently using their lock)
            CHECK( pthread_mutex_lock(&stat_lock), "pthread_mutex_lock",  )
            total_pages_downloaded++;
            total_bytes_downloaded += total_bytes_read;   // should be equal with content_length if everything goes well
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
    if (urlLen == 0){                       // should not happen
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
            port_number_str = NULL;     // port number will get the default 8080 value later
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
    // get file's size
    long pos = ftell(page);              // current position
    fseek(page, 0, SEEK_END);            // go to end of file
    long int file_length = ftell(page);  // read the position which is the size of the file
    char *webpage = new char[file_length + 1];
    fseek(page, pos, SEEK_SET);          // restore original position
    {   // load the entire webpage onto memory using a buffer (this allows for simpler - and, therefore, safer - parsing of the page)
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
    // parse page file in order to find: "<a" + "href" + "<link>" + ">" and add each <link> to the urlQueue,
    // but only if it does not exists on urlHistory (aka it's not been put into the urlQueue before)
    char link[MAX_LINK_SIZE];
    for (int i = 0 ; i < file_length && webpage[i] != '\0' ; i++) {  // iterate over the file's characters
        if ( webpage[i] == '<' && i + 1 < file_length && webpage[i+1] == 'a' ){   // if came across an "<a" from an <a ...> tag
            i += 3;                        // skip "<a "
            while ( i < file_length && webpage[i] != '>' && !(i + 4 < file_length && webpage[i] == 'h' && webpage[i + 1] == 'r' && webpage[i + 2] == 'e' && webpage[i + 3] == 'f') )
                i++;                       // advance until href or '>'
            if (webpage[i] != '>') {       // "href" detected
                i += 4;                    // skip "href"
                while (i < file_length && webpage[i] != '>' && webpage[i] != '"') i++;               // advance until '>' or '"'
                if (webpage[i] != '>' && i+1 < file_length) {       // link starts at webpage[i+1] !
                    i++;                   // skip '"'
                    int j = 0;
                    if (webpage[i] == '.' && i+1 < file_length && webpage[i+1] == '.') i += 2;       // if link starts as "../sitei/pagei_j.html" then skip those ".."
                    while (i + j < file_length && webpage[i + j] != '"' && webpage[i + j] != '>' && j < MAX_LINK_SIZE) {   // read link (could be root_relative or a full http url)
                        link[j] = webpage[i+j];
                        j++;
                    }
                    link[j] = '\0';
                    if ( webpage[i + j] != '>' ) {                      // if <a > tag was not closed before the link ended, then:
                        if (j > 0) {                                    // if actually found a link (and not ex: "")
                            // first examine link found to see to which server and which port it refers to
                            bool add_link_to_history = true;            // because if it does not refer to our server then we must not add it to urlHistory
                            char *root_relative_link;
                            findRootRelativeUrl(link, root_relative_link);      // find the root-relative part of this possibly full http link
                            if (root_relative_link == NULL) {          // should not happen
                                cerr << "Unexpected failure for finding the root relative link of the url: " << link << endl;
                                root_relative_link = link;              //Adding itself to history instead..
                            } else if (root_relative_link != link) {    // if link was not root_relative then examine it
                                int port, res;
                                char *host_or_IP, *port_num_str;
                                CHECK((res = parse_url(link, root_relative_link, host_or_IP, port_num_str)), "parse url while crawling for links",)   // parse full http url
                                if (res == 1) {                         // found an invalid link
                                    // it's ok to add this link to urlQueue, it will be recognized as a false link when we pop it
                                    add_link_to_history = false;        // but do not add it to urlHistory
                                } else if (host_or_IP != NULL) {
                                    if (port_num_str == NULL) port = 8080;     // default port
                                    else port = atoi(port_num_str);
                                    sockaddr_in lookedUpAddr = look_up_addr_r(host_or_IP, port);    // look up sockadrr_in address for host_or_IP, port found
                                    if (lookedUpAddr.sin_port != server_sa.sin_port || lookedUpAddr.sin_addr.s_addr != server_sa.sin_addr.s_addr) {
                                        // The root-relative links for URLS refering to OTHER servers should NOT be added to the urlHistory
                                        // in case they conflict with the root_relative links for our server
                                        add_link_to_history = false;
                                    }
                                    delete[] host_or_IP;
                                    delete[] port_num_str;
                                }
                            }

                            // then (atomically) add the link to urlQueue  and, if the host of the link is the given server, to urlHistrory as well
                            urlQueue->acquire();
                            // IMPORTANT: acquiring Queue's lock BEFORE the next if-check avoids the race condition of two threads finding the same link simultaneously!
                            if (!urlHistory->search(root_relative_link)) {                          // add link to urlQueue ONLY if it doesn't exist on our urlHistory structure (aka this link has not been added to the urlQueue before)
                                if (add_link_to_history) urlHistory->add(root_relative_link);       // ATOMICALLY add new found link to our urlHistory struct if above-stated conditions are met
                                urlQueue->push(link);                                               // push new link to the urlQueue
                                CHECK(pthread_cond_signal(&QueueIsEmpty), "pthread_cond_signal",)   // signal ONE thread to read the new link from the urlQueue
                                cout << "added a link: " << link << endl;                           // print a corresponding message (this is not printed for starting_url onbiously)
                            }
                            urlQueue->release();
                        }

                        i += j;       // skip link and the 2nd '"' (or the '>')
                        while (i < file_length && webpage[i] != '>') i++;       // ignore the rest of the tag until it closes
                    }
                }
            }
        }
    }
    delete[] webpage;
    CHECK_PERROR(fclose(page), "fclose",)
}

sockaddr_in look_up_addr_r(const char *host_or_IP,int port){   // look up a sockaddr_in for given host_or_IP, port but thread safely
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
