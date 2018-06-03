#include <iostream>
#include <cstdio>
#include <unistd.h>
#include <cerrno>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <cstring>
#include <cstdlib>
#include <sys/stat.h>
#include <poll.h>
#include <arpa/inet.h>
#include <netdb.h>
#include "../headers/FIFO_Queue.h"
#include "../headers/crawl.h"
#include "../headers/str_history.h"
#include "../headers/crawling_monitoring.h"
#include "../headers/executables_paths.h"


using namespace std;


#define NUM_OF_WORKERS 5                     // number of workers for jobExecutor
#define COMMAND_QUEUE_SIZE 20                // size of the queue for incoming TCP command connections (only one will be handled at a time)
#define MAX_COMMAND_WORD_SIZE 256            // maximum size for a word in a command (for bigger words we will only keep the first 256 characters)
#define BUFFER_SIZE 2048                     // size of the buffer used to read the jobExecutor's answers to commands
#define MAX_ARGUMENT_WORD_SIZE 256           // the maximum size of a word argument for a jobExecutor command (used to estimate the buffer size for reading from the socket)


/* useful macros */
#define CHECK_PERROR(call, callname, handle_code) { if ( ( call ) < 0 ) { perror(callname); handle_code } }
#define CHECK(call, callname, handle_code) { if ( ( call ) < 0 ) { cerr << (callname) << " failed" << endl; handle_code } }


/*_____Global variables______*/
/* Statistics: */
time_t time_crawler_started;
pthread_mutex_t stat_lock;                   // mutex protecting stats' variables
unsigned int total_pages_downloaded = 0;
unsigned int total_bytes_downloaded = 0;
/* web crawling: */
char *save_dir = NULL;
pthread_cond_t QueueIsEmpty;                 // condition variable for whether or not the Queue of URLs is empty
bool threads_must_terminate = false;         // used by the crawling_monitoring.cpp thread (along with a broadcast) to notify the other threads that they have to exit prematurely
int num_of_threads_blocked = 0;              // number of threads blocked on cond_t QueueIsEmpty
FIFO_Queue *urlQueue = NULL;                 // common URL Queue for all threads: stores both full http URLS and root-relative URLS
str_history *urlHistory = NULL;              // common URL History for all threads: stores only the root-relative version of URLS concerning our server. This data structure is a search tree allowing for an O(logn) search and insertion
/* thread monitoring: */
bool crawling_has_finished = false;          // will be set to true by the last crawl.cpp thread when it's about to block with an empty urlQueue along with a signal to crawlingHasFinished cond_t
pthread_cond_t crawlingFinished;             // The crawling_monitoring.cpp thread will block waiting on this cond_t. When notified that crawling has finished then it will in turn notify all threads to exit and initialize the jobExecutor
pthread_mutex_t crawlingFinishedLock;        // mutex that protects boolean "crawling_has_finished"  and cond_t "crawlingFinished"
bool monitor_forced_exit = false;            // set to true if and when we receive a SHUTDOWN command before crawling has finished along with a signal to cond_t crawlingFinished so that the crawling_monitoring.cpp thread (and the other ones) will exit.
/* jobExecutor connection: */
bool jobExecutorReadyForCommands = false;    // will be set to true by the crawling_monitoring.cpp thread when (webcrawling has finished and) jobExecutor has been initialized.
pid_t jobExecutor_pid = -1;                  // jobExecutor's pid, will be updated after fork + exec by the crawling_monitoring.cpp thread
str_history *alldirs = NULL;                 // we keep track of all directories that the crawler has downloaded here. This way only directories downloaded in this execution will be sent to jobExecutor
extern int toJobExecutor_pipe, fromJobExecutor_pipe;   // file descriptors for write and read end respectivelly of the two pipes used for communication with the jobExecutor


/* Local Functions */
int parse_arguments(int argc, char *const *argv, char *&host_or_IP, uint16_t &server_port, uint16_t &command_port, int &num_of_threads, char *&save_dir, char *&starting_url);


int main(int argc, char *argv[]) {
    time_crawler_started = time(NULL);
    // before doing anything else make sure that our paths to executable files are correct:
    if( access(JOBEXECUTOR_PATH, F_OK ) < 0 ) {      // if jobExecutor's executable does not exist then we cannot run this program
        cerr << "Error: jobExecutor's executable does not exist on #defined location" << endl;
        return -1;
    }
    // parse main's parameters
    char *host_or_IP = NULL, *starting_url = NULL;
    uint16_t server_port = 0, command_port = 0;
    int num_of_threads = -1;
    if (parse_arguments(argc, argv, host_or_IP, server_port, command_port, num_of_threads, save_dir, starting_url) < 0 ){
        cerr << "Invalid web crawler parameters" << endl;
        return -1;
    }
    cout << "Web crawler initialized with:\n - host_or_IP: " << host_or_IP << "\n - server port: " << server_port << "\n - command port: " << command_port
         << "\n - num_of_threads: " << num_of_threads << "\n - save directory: " << save_dir << "\n - starting_url: " << starting_url << endl << endl;

    // threads should ignore SIGPIPE in case connection is closed by the server and they are blocked on read
    struct sigaction act;
    memset(&act, 0, sizeof(act));
    act.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &act, NULL);

    // Find out server's socket address
    struct sockaddr_in server_sa;
    server_sa.sin_family = AF_INET;
    server_sa.sin_port = htons(server_port);
    struct in_addr in;
    memset(&in, 0, sizeof(in));
    if ( inet_aton(host_or_IP, &in) == 1) {       // try parsing host_or_IP as an IP
        server_sa.sin_addr = in;
    }
    else {                                        // else if unsuccessful then host_or_IP must be a host and not an IP after all
        struct hostent *lookup = gethostbyname(host_or_IP);
        if ( lookup  == NULL ){
            cout << "Could not find host (DNS failed)" << endl;
            delete[] save_dir; delete[] host_or_IP; delete[] starting_url; return -2;
        } else {
            server_sa.sin_addr = *((struct in_addr *) lookup->h_addr);    // arbitrarily pick the first address
        }
    }

    // Create command socket
    struct sockaddr_in command_sa;
    command_sa.sin_family = AF_INET;
    command_sa.sin_port = htons(command_port);
    command_sa.sin_addr.s_addr = htonl(INADDR_ANY);
    int command_socket_fd;
    CHECK_PERROR( (command_socket_fd = socket(AF_INET, SOCK_STREAM, 0)) , "socket" , delete[] save_dir; delete[] host_or_IP; delete[] starting_url; return -4; )
    CHECK_PERROR( bind(command_socket_fd, (struct sockaddr *) &command_sa, sizeof(command_sa)) , "bind" , close(command_socket_fd); delete[] save_dir; delete[] host_or_IP; delete[] starting_url; return -4; )
    CHECK_PERROR( listen(command_socket_fd, COMMAND_QUEUE_SIZE) , "listen" , close(command_socket_fd); delete[] save_dir; delete[] host_or_IP; delete[] starting_url; return -4; )

    // init threadpool here
    pthread_t *threadpool = new pthread_t[num_of_threads];
    for (int i = 0 ; i < num_of_threads ; i++){
        threadpool[i] = 0;
    }

    // create the FIFO Queue that will be used by all threads and push starting url into it. This Queue can contain both: root relative urls and full http urls
    urlQueue = new FIFO_Queue();
    urlQueue->push(starting_url);                   // locking is not necessary yet - only one thread

    // create the URL History search tree (empty at start): it will contain ONLY the root relative urls for ALL the pages that were added to the urlQueue and are asked from our host_or_IP server, in order to we make sure they're added only once
    urlHistory = new str_history();
    char *root_relative_starting_url;
    findRootRelativeUrl(starting_url, root_relative_starting_url);    // Note: starting_url should be for host_or_IP server, but even if it is not, it's ok to add it to urlHistory here since no crawling will be done whatsoever
    if ( root_relative_starting_url == NULL ){      // should not happen
        cerr << "Unexpected failure for finding the root relative link of the starting_url: " << link << endl;
        root_relative_starting_url = starting_url;
    }
    urlHistory->add(root_relative_starting_url);    // (atomically although not necessary yet) add the first url to our urlHistory struct

    // create the directories search tree struct, where we store all site directories downloaded for the jobExecutor to use (if empty the jobExecutor should not be initialized nor used)
    alldirs = new str_history();

    // create one thread to monitor EXACTLY when the web crawling has finished
    cout << "Creating monitor thread..." << endl;
    struct monitor_args margs;
    margs.num_of_threads = num_of_threads;
    margs.threadpool = threadpool;
    margs.num_of_workers = NUM_OF_WORKERS;
    CHECK( pthread_cond_init(&crawlingFinished, NULL) , "pthread_cond_init", delete urlQueue; delete urlHistory; delete alldirs; delete[] threadpool; close(command_socket_fd); delete[] save_dir; delete[] host_or_IP; delete[] starting_url; return -5; )
    CHECK( pthread_mutex_init(&crawlingFinishedLock, NULL) , "pthread_mutex_init" , delete urlQueue; delete urlHistory; delete alldirs; delete[] threadpool; close(command_socket_fd); delete[] save_dir; delete[] host_or_IP; delete[] starting_url; return -5; )
    pthread_t monitor_tid = 0;
    CHECK( pthread_create(&monitor_tid, NULL, monitor_crawling, (void *) &margs), "pthread_create monitor thread" , delete urlQueue; delete urlHistory; delete alldirs; delete[] threadpool; close(command_socket_fd); delete[] save_dir; delete[] host_or_IP; delete[] starting_url; return -5; )

    // create num_of_thread threads
    cout << "Creating num_of_threads threads..." << endl;
    CHECK( pthread_cond_init(&QueueIsEmpty, NULL) , "pthread_cond_init", delete urlQueue; delete urlHistory; delete alldirs; delete[] threadpool; close(command_socket_fd); delete[] save_dir; delete[] host_or_IP; delete[] starting_url; return -6; )
    CHECK( pthread_mutex_init(&stat_lock, NULL) , "pthread_mutex_init" , delete urlQueue; delete urlHistory; delete alldirs; delete[] threadpool; close(command_socket_fd); delete[] save_dir; delete[] host_or_IP; delete[] starting_url; return -6; )
    struct args arguements(&server_sa, num_of_threads);
    for (int i = 0 ; i < num_of_threads ; i++){
        CHECK( pthread_create(&threadpool[i], NULL, crawl, (void *) &arguements) , "pthread_create" , threadpool[i] = 0; )   // (!) threadpool[i] = 0 signifies that this thread was not created
    }

    cout << "Ready to receive commands from the command socket..." << endl;
    // repeatedly block and wake up to handle a command from the command socket
    struct pollfd *pfds = new struct pollfd[1];
    pfds[0].fd = command_socket_fd;
    pfds[0].events =  POLLIN;
    pfds[0].revents = 0;
    int retval;
    for (;;) {
        retval = poll(pfds, 1, -1);                 // wait indefinitely until a TCP connection request comescheck_if_webcrawling_finished(num_of_threads, threadpool);
        if ( retval < 0 ){
            if ( errno == EINTR ) {    // terminating signal?
                cerr << "poll interrupted by a signal! webcrawling exiting..." << endl;
                CHECK( pthread_mutex_lock(&crawlingFinishedLock) , "pthread_mutex_lock", )           // lock crawling_has_finished's mutex
                if ( !crawling_has_finished ){      // if webcrawling has not finished yet then force it to
                    monitor_forced_exit = true;     // by setting this boolean to true
                    CHECK ( pthread_cond_signal(&crawlingFinished), "pthread_cond_signal", )         // and signaling the monitor thread to exit (this thread will join all the other ones)
                    CHECK( pthread_mutex_unlock(&crawlingFinishedLock) , "pthread_mutex_unlock", )   // unlock crawling_has_finished's mutex
                    cout << "Exiting before web crawling finished..." << endl;
                }
                CHECK( pthread_mutex_unlock(&crawlingFinishedLock) , "pthread_mutex_unlock", )       // unlock crawling_has_finished's mutex
                break;
            } else {
                perror("poll() failed");
            }
        } else if ( retval == 0 ){
            cerr << "Unexpected return 0 from poll" << endl;
        } else {
            if ( pfds[0].revents & POLLIN ){
                pfds[0].revents = 0;                // reset revents field
                int new_connection;
                struct sockaddr_in incoming_sa;
                socklen_t len = sizeof(incoming_sa);
                CHECK_PERROR((new_connection = accept(command_socket_fd, (struct sockaddr *) &incoming_sa, &len)), "accept on command socket failed unexpectedly", break; )
                cout << "Crawler accepted a (command) connection from " << inet_ntoa(incoming_sa.sin_addr) << " : " << incoming_sa.sin_port << endl;
                // read char-by-char (should be fine, worst case is 128 reads) until " " or "\n" and consider this a command
                bool noMoreInput = false;
                char command[128];
                char prev = ' ';
                int i = 0;
                while ( i < 128 ) {
                    CHECK_PERROR( read(new_connection, command + i, 1), "read from accepted command socket", continue; )
                    if ( (prev != ' ' && command[i] == ' ') || command[i] == '\n' ){
                        if ( command[i] == '\n' ) noMoreInput = true;
                        if ( i > 0 && command[i-1] == '\r' ) command[i-1] = '\0';
                        else command[i] = '\0';
                        break;
                    } else if ( prev == ' ' && command[i] == ' ' ){   // ignore continuous white space at start
                        continue;
                    }
                    prev = command[i];
                    i++;
                }
                if ( i == 128 ) command[127] = '\0';      // if command was too big it's not going to be legal anyway
                if ( strcmp(command, "SHUTDOWN") == 0 ){
                    cout << "received SHUTDOWN command" << endl;
                    CHECK( pthread_mutex_lock(&crawlingFinishedLock) , "pthread_mutex_lock", )           // lock crawling_has_finished's mutex
                    if ( !crawling_has_finished ){        // if web crawling has not finished yet then force it to
                        monitor_forced_exit = true;       // by setting this boolean to true
                        CHECK ( pthread_cond_signal(&crawlingFinished), "pthread_cond_signal", )         // and signaling the monitor thread to exit (this thread will join all the other ones)
                        CHECK( pthread_mutex_unlock(&crawlingFinishedLock) , "pthread_mutex_unlock", )   // unlock crawling_has_finished's mutex
                        cout << "Exiting before web crawling finished..." << endl;
                    }
                    CHECK( pthread_mutex_unlock(&crawlingFinishedLock) , "pthread_mutex_unlock", )       // unlock crawling_has_finished's mutex
                    break;
                }
                else if ( strcmp(command, "STATS") == 0 ){
                    cout << "received STATS command" << endl;
                    time_t Dt = time(NULL) - time_crawler_started;
                    char response[256];
                    CHECK( pthread_mutex_lock(&stat_lock), "pthread_mutex_lock",  )
                    sprintf(response, "Crawler has been up for %.2zu:%.2zu:%.2zu, downloaded %u pages, %u bytes\n", Dt / 3600, (Dt % 3600) / 60 , (Dt % 60), total_pages_downloaded, total_bytes_downloaded);
                    CHECK( pthread_mutex_unlock(&stat_lock), "pthread_mutex_unlock",  )
                    CHECK_PERROR( write(new_connection , response, strlen(response)), "write to accepted command socket", )
                }
                else if ( strcmp(command, "SEARCH") == 0 || strcmp(command, "MAXCOUNT") == 0 || strcmp(command, "MINCOUNT") == 0 || strcmp(command, "WORDCOUNT") == 0 ) {
                    if (alldirs->get_size() == 0){   // get_size is not atomic, but this is harmless since we only read the size variable (it will not affect other threads)
                        char msg[] = "web crawler found and downloaded 0 pages, hence jobExecutor commands cannot be used\n";   // jobExecutor will not be initialized if this is the case
                        CHECK_PERROR( write(new_connection, msg, strlen(msg)), "write to accepted command socket", );
                    } else if (!jobExecutorReadyForCommands){
                        char msg[] = "web crawling is still in progress (or jobExecutor is not initialized yet)\n";
                        CHECK_PERROR( write(new_connection, msg, strlen(msg)), "write to accepted command socket", );
                    } else if (noMoreInput && (strcmp(command, "SEARCH") == 0 || strcmp(command, "MAXCOUNT") == 0 || strcmp(command, "MINCOUNT") == 0)){
                        CHECK_PERROR( write(new_connection , "No arguments given\n", strlen("No arguments given\n")), "write to accepted command socket", )
                    } else {
                        if ( strcmp(command, "SEARCH") == 0 ){
                            cout << "received SEARCH command" << endl;
                            // send /search command along with any word arguments to the jobExecutor
                            CHECK_PERROR(write(toJobExecutor_pipe, "/search ", strlen("/search ")), "write to jobExecutor", )         // send "/search" command
                            char arguments[MAX_ARGUMENT_WORD_SIZE * 10 + 64];   // + 64 just in case there is a lot of whitespace between the words
                            ssize_t bytes_read = 0, total_bytes_read = 0;
                            bool stop = false;
                            while( total_bytes_read < MAX_ARGUMENT_WORD_SIZE * 10 + 64 && !stop ){                // while not read '\n'
                                CHECK_PERROR((bytes_read = read(new_connection, arguments + total_bytes_read, MAX_ARGUMENT_WORD_SIZE * 10 + 64 - total_bytes_read)), "read from accepted command socket", break; )
                                for (int j = (int) total_bytes_read ; j < total_bytes_read + bytes_read ; j++){   // if found '\n' on data just read
                                    if ( arguments[j] == '\n' ){
                                        if ( j > 0 && arguments[j-1] == '\r') total_bytes_read = j - 1;
                                        else total_bytes_read = j;
                                        stop = true;                                                              // then stop reading
                                        break;
                                    }
                                }
                                if (!stop) total_bytes_read += bytes_read;
                            }
                            if ( total_bytes_read == MAX_ARGUMENT_WORD_SIZE * 10 + 64 ){
                                cerr << "Warning: might not have read the whole arguments for this command (arguments too big)" << endl;
                                if (arguments[MAX_ARGUMENT_WORD_SIZE * 10 + 64 - 1] == '\r') arguments[MAX_ARGUMENT_WORD_SIZE * 10 + 64 - 1] = ' ';
                            }
                            CHECK_PERROR( write(toJobExecutor_pipe, arguments, total_bytes_read), "write to jobExecutor", )
                            CHECK_PERROR( write(toJobExecutor_pipe, "\n", 1), "write to jobExecutor", )
                        }
                        else if ( strcmp(command, "MAXCOUNT") == 0 || strcmp(command, "MINCOUNT") == 0){
                            cout << "received " << ((strcmp(command, "MAXCOUNT") == 0) ? "MAXCOUNT" : "MINCOUNT") << " command" << endl;
                            // send /maxcount or /mincount command along with any word arguments to jobExecutor (if there are more than one words then jobExecutor will ignore any but the first)
                            CHECK_PERROR(write(toJobExecutor_pipe, ((strcmp(command, "MAXCOUNT") == 0) ? "/maxcount " : "/mincount "), strlen("/maxcount ")), "write to jobExecutor", )         // send "/maxcount" or "/mincount" command
                            char arguments[MAX_ARGUMENT_WORD_SIZE + 8];   // + 8 just in case there is a lot of whitespace before the first word
                            ssize_t bytes_read = 0, total_bytes_read = 0;
                            bool stop = false;
                            while( total_bytes_read < MAX_ARGUMENT_WORD_SIZE + 8 && !stop ){                      // while not read '\n'
                                CHECK_PERROR((bytes_read = read(new_connection, arguments + total_bytes_read, MAX_ARGUMENT_WORD_SIZE + 8 - total_bytes_read)), "read from accepted command socket", break; )
                                for (int j = (int) total_bytes_read ; j < total_bytes_read + bytes_read ; j++){   // if found '\n' on data just read
                                    if ( arguments[j] == '\n' ){
                                        if ( j > 0 && arguments[j-1] == '\r') total_bytes_read = j - 1;
                                        else total_bytes_read = j;
                                        stop = true;                                                              // then stop reading
                                        break;
                                    }
                                }
                                if (!stop) total_bytes_read += bytes_read;
                            }
                            if ( total_bytes_read == MAX_ARGUMENT_WORD_SIZE + 8 ){
                                cerr << "Warning: might not have read the whole arguments for this command (arguments too big)" << endl;
                                if (arguments[MAX_ARGUMENT_WORD_SIZE + 8 - 1] == '\r') arguments[MAX_ARGUMENT_WORD_SIZE + 8 - 1] = ' ';
                            }
                            CHECK_PERROR( write(toJobExecutor_pipe, arguments, total_bytes_read), "write to jobExecutor", )
                            CHECK_PERROR( write(toJobExecutor_pipe, "\n", 1), "write to jobExecutor", )
                        }
                        else if ( strcmp(command, "WORDCOUNT") == 0 ){
                            cout << "received WORDCOUNT command" << endl;
                            // send /worcount command to jobExecutor
                            CHECK_PERROR(write(toJobExecutor_pipe, "/wc\n", strlen("/wc\n")), "write to jobExecutor", )         // send "/wordcount" command
                        }
                        // read and send to socket jobExecutor's answer, whatever it was
                        // IMPORTANT: jobExecutor must print (\n)"<\n" after each command's output so that we know here when to stop reading! Since html tags are ignored, this message wont be anywhere else in its answer!
                        char buffer[BUFFER_SIZE];
                        bool previous_chunk_ends_in_tag = false;
                        for (;;) {
                            ssize_t bytes_read;
                            CHECK_PERROR((bytes_read = read(fromJobExecutor_pipe, buffer, BUFFER_SIZE)), "read from jobExecutor", break;)
                            if (previous_chunk_ends_in_tag && bytes_read > 0 && buffer[0] == '\n'){
                                if (bytes_read > 1) cerr << "Warning: jobExecutor sent more stuff after agreed upon \"<endl\"" << endl;
                                break;
                            }
                            else previous_chunk_ends_in_tag = false;    // not really necessary since the only way to find "<" is followed by an "\n"
                            if ( bytes_read >= 2 && buffer[bytes_read-2] == '<' && buffer[bytes_read-1] == '\n' ){   // if this was the last read from the jobExecutor's answer
                                bytes_read -= 2;      // do not write "<\n" on output to socket
                                if (bytes_read > 0){
                                    CHECK_PERROR( write(new_connection, buffer, bytes_read), "write to accepted command socket", );
                                }
                                break;
                            }
                            else if ( bytes_read >= 1 && buffer[bytes_read-1] == '<' ){
                                bytes_read--;        // do not write "<" on output socket
                                previous_chunk_ends_in_tag = true;
                            }
                            CHECK_PERROR( write(new_connection, buffer, bytes_read), "write to accepted command socket", );
                        }
                    }
                }
                else{
                    cout << "Received illegal command: " << command << endl;     // (!) keep in mind that white spaces sent at start are also considered illegal
                    CHECK_PERROR( write(new_connection, "illegal command\n", strlen("illegal command\n")) , "write to accepted command socket", )
                }
                CHECK_PERROR( close(new_connection), "closing accepted command connection", )
            }
            else cerr << "Unexpected return >0 from poll: command socket had no data ready" << endl;
        }
    }

    delete[] pfds;

    CHECK_PERROR( close(command_socket_fd), "closing command socket", )

    void *status;
    CHECK(pthread_join(monitor_tid, &status), "pthread_join monitor thread",)
    if (status != NULL) { cerr << "monitor thread terminated with an unexpected status" << endl; }
    CHECK( pthread_mutex_destroy(&crawlingFinishedLock) , "pthread_mutex_destroy" , )
    CHECK( pthread_cond_destroy(&crawlingFinished), "pthread_cond_destroy", )

    CHECK( pthread_mutex_destroy(&stat_lock) , "pthread_mutex_destroy" , )
    CHECK( pthread_cond_destroy(&QueueIsEmpty), "pthread_cond_destroy", )

    delete urlQueue;
    delete urlHistory;
    delete[] threadpool;
    delete[] host_or_IP;
    delete[] save_dir;
    delete[] starting_url;

    if (!monitor_forced_exit && alldirs->get_size() > 0) {          // if jobExecutor was initialized in the first place
        // tell jobExecutor to stop
        CHECK_PERROR(write(toJobExecutor_pipe, "/exit\n", strlen("/exit\n")), "write \"/exit\" to jobExecutor failed", )
        int stat;
        waitpid(jobExecutor_pid, &stat, 0);
        if (stat != 0) {
            cerr << "(!) jobExecutor terminated with status: " << stat << endl;
        }
    }

    delete alldirs;

    return 0;
}



/* Local Function Implementation */
int parse_arguments(int argc, char *const *argv, char *&host_or_IP, uint16_t &server_port, uint16_t &command_port, int &num_of_threads, char *&save_dir, char *&starting_url) {
    bool vital_params_given[4] = {false, false, false, false} , num_of_threads_given = false;
    for (int i = 1 ; i < argc - 1 ; i += 2){
        if ( strcmp(argv[i], "-h") == 0 && i + 1 < argc && argv[i+1][0] != '-' ){
            host_or_IP = new char[strlen(argv[i+1]) + 1];
            strcpy(host_or_IP, argv[i+1]);
            vital_params_given[0] = true;
        }
        else if ( strcmp(argv[i], "-p") == 0 && i + 1 < argc - 1 && argv[i+1][0] != '-' ){
            server_port = (uint16_t) atoi(argv[i+1]);
            vital_params_given[1] = true;
        }
        else if ( strcmp(argv[i], "-c") == 0 && i + 1 < argc - 1 && argv[i+1][0] != '-' ){
            command_port = (uint16_t) atoi(argv[i+1]);
            vital_params_given[2] = true;
        }
        else if ( strcmp(argv[i], "-t") == 0 && i + 1 < argc - 1 && argv[i+1][0] != '-' ){
            num_of_threads = atoi(argv[i+1]);
            num_of_threads_given = true;
        }
        else if ( strcmp(argv[i], "-d") == 0 && i + 1 < argc - 1 && argv[i+1][0] != '-' ){
            int add_extra_byte = 1;
            if ( argv[i+1][strlen(argv[i+1]) - 1] == '/' ){       // if save_dir argument has a '/' at the end
                argv[i+1][strlen(argv[i+1]) - 1] = '\0';          // remove it
                add_extra_byte = 0;
            }
            save_dir = new char[strlen(argv[i+1]) + add_extra_byte];
            strcpy(save_dir, argv[i+1]);
            vital_params_given[3] = true;
        }
        else{
            if (vital_params_given[0]){ delete[] host_or_IP; }
            if (vital_params_given[3]){ delete[] save_dir; }
            return -1;
        }
    }
    if ( !num_of_threads_given ){
        num_of_threads = 4;                // default value
    }
    if ( !vital_params_given[0] || !vital_params_given[1] || !vital_params_given[2] || !vital_params_given[3] || (num_of_threads_given && num_of_threads <= 0) ){
        if (vital_params_given[0]){ delete[] host_or_IP; }
        if (vital_params_given[3]){ delete[] save_dir; }
        return -2;
    }
    // check if save_dir exists
    struct stat info;
    if( stat(save_dir, &info ) != 0 ) {
        cerr << "cannot access save directory" << endl;
        delete[] save_dir;
        delete[] host_or_IP;
        return -3;
    }else if( ! ( info.st_mode & S_IFDIR ) ){
        cerr << "given save directory is not a directory" << endl;
        delete[] save_dir;
        delete[] host_or_IP;
        return -4;
    }
    // Now read last command line argument as starting str
    int slash_counter = 0;
    for (int i = 0 ; i < strlen(argv[argc-1]) ; i++ ){
        if ( argv[argc-1][i] == '/' ) slash_counter++;
    }
    if (argv[argc-1][0] == '/' && slash_counter != 2){  // root relative links must have 2 '/'s in total: /sitei/pagei_j.html
        cerr << "starting url is root relative but is incorect. Should be of type: /site/page.html" << endl;
        delete[] save_dir;
        delete[] host_or_IP;
        return -5;
    } else if ( slash_counter != 4 ) {                      // full http urls must have 4 '/'s in total: http://linux01.di.uoa.gr/sitei/pagei_j_html
        cerr << "starting url is a full http URL but is incorect. Should be of type: http://hostname[:port]/site/page.html" << endl;
        delete[] save_dir;
        delete[] host_or_IP;
        return -5;
    }
    starting_url = new char[strlen(argv[argc - 1]) + 1];
    strcpy(starting_url, argv[argc - 1]);
    return 0;
}
