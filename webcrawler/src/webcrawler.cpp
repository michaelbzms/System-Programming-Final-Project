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


using namespace std;


#define COMMAND_QUEUE_SIZE 20
#define NUM_OF_WORKERS 5                     // num of workers for jobExecutor
#define MAX_SEARCH_WORD_SIZE 256
#define MAX_LINE_SIZE 2048

/* useful macros */
#define CHECK_PERROR(call, callname, handle_code) { if ( ( call ) < 0 ) { perror(callname); handle_code } }
#define CHECK(call, callname, handle_code) { if ( ( call ) < 0 ) { cerr << (callname) << " failed" << endl; handle_code } }


/* Global variables */
time_t time_crawler_started;
pthread_mutex_t stat_lock;
unsigned int total_pages_downloaded = 0;
unsigned int total_bytes_downloaded = 0;
char *save_dir = NULL;
pthread_cond_t QueueIsEmpty;
bool threads_must_terminate = false;
int num_of_threads_blocked = 0;
FIFO_Queue *urlQueue = NULL;                 // common str Queue for all threads
str_history *urlHistory = NULL;              // common str History for all threads (a search Tree of all the urls already downloaded during web crawling)
bool crawling_has_finished = false;
pthread_cond_t crawlingFinished;             // concerns boolean "crawling_has_finished"
pthread_mutex_t crawlingFinishedLock;        // protects boolean "crawling_has_finished"
bool monitor_forced_exit = false;
bool jobExecutorReadyForCommands = false;
pid_t jobExecutor_pid = -1;
str_history *alldirs = NULL;                 // keep track of all directories that you have downloaded
extern int toJobExecutor_pipe, fromJobExecutor_pipe;


/* Local Functions */
int parse_arguements(int argc, char *const *argv, char *&host_or_IP, uint16_t &server_port, uint16_t &command_port, int &num_of_threads, char *&save_dir, char *&starting_url);


int main(int argc, char *argv[]) {
    time_crawler_started = time(NULL);
    // parse main's parameters
    char *host_or_IP = NULL, *starting_url = NULL;
    uint16_t server_port = 0, command_port = 0;
    int num_of_threads = -1;
    if ( parse_arguements(argc, argv, host_or_IP, server_port, command_port, num_of_threads, save_dir, starting_url) < 0 ){
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

    // create the FIFO Queue that will be used by all threads and push starting str into it
    urlQueue = new FIFO_Queue();
    urlQueue->push(starting_url);      // locking is not necessary yet - only one thread

    // create the URL History search tree and add starting link to it
    urlHistory = new str_history();
    urlHistory->add(starting_url);

    // create the directories created History search tree and add staring link's directory to it
    alldirs = new str_history();
    // (!) starting_url MUST be root-relative ex: /sitei/pagei_j.html
    {   int i, save_dir_len = strlen(save_dir);
        char *startingdir = new char[save_dir_len + strlen(starting_url) + 1];
        strcpy(startingdir, save_dir);
        startingdir[save_dir_len] = '/';        // combine save_dir and "sitei" with a '/' in between
        for (i = 1 ; i < strlen(starting_url) && starting_url[i] != '/' ; i++ ){   // i = 1 -> skip 1st '/' and stop at the second one
            startingdir[i+save_dir_len] = starting_url[i];
        }
        startingdir[i+save_dir_len] = '\0';
        alldirs->add(startingdir);
        delete[] startingdir;
    }

    cout << "Creating monitor thread..." << endl;
    // create one thread to monitor exactly when the web crawling has finished
    struct monitor_args margs;
    margs.num_of_threads = num_of_threads;
    margs.threadpool = threadpool;
    margs.num_of_workers = NUM_OF_WORKERS;
    margs.save_dir = save_dir;
    CHECK( pthread_cond_init(&crawlingFinished, NULL) , "pthread_cond_init", delete urlQueue; delete urlHistory; delete alldirs; delete[] threadpool; close(command_socket_fd); delete[] save_dir; delete[] host_or_IP; delete[] starting_url; return -5; )
    CHECK( pthread_mutex_init(&crawlingFinishedLock, NULL) , "pthread_mutex_init" , delete urlQueue; delete urlHistory; delete alldirs; delete[] threadpool; close(command_socket_fd); delete[] save_dir; delete[] host_or_IP; delete[] starting_url; return -5; )
    pthread_t monitor_tid = 0;
    CHECK( pthread_create(&monitor_tid, NULL, monitor_crawling, (void *) &margs), "pthread_create monitor thread" , delete urlQueue; delete urlHistory; delete alldirs; delete[] threadpool; close(command_socket_fd); delete[] save_dir; delete[] host_or_IP; delete[] starting_url; return -5; )

    cout << "Creating num_of_threads threads..." << endl;
    // create num_of_thread threads
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

            /// --> join with monitor thread here? (or always at exit?)

            if ( pfds[0].revents & POLLIN ){
                pfds[0].revents = 0;                // reset revents field
                int new_connection;
                struct sockaddr_in incoming_sa;
                socklen_t len = sizeof(incoming_sa);
                CHECK_PERROR((new_connection = accept(command_socket_fd, (struct sockaddr *) &incoming_sa, &len)), "accept on command socket failed unexpectedly", break; )
                cout << "Crawler accepted a (command) connection from " << inet_ntoa(incoming_sa.sin_addr) << " : " << incoming_sa.sin_port << endl;
                // read char-by-char until " " and consider this a command
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
                else if ( strcmp(command, "SEARCH") == 0 ){
                    cout << "received SEARCH command" << endl;
                    if (!jobExecutorReadyForCommands){
                        char msg[] = "web crawling is still in progress (or jobExecutor is not initialized yet)\n";
                        CHECK_PERROR( write(new_connection, msg, strlen(msg)), "write to accepted command socket", );
                    } else if (noMoreInput){
                        CHECK_PERROR( write(new_connection , "no SEARCH arguments given\n", strlen("no SEARCH arguements given\n")), "write to accepted command socket", )
                    } else {
                        // send /search command to jobExecutor
                        CHECK_PERROR(write(toJobExecutor_pipe, "/search", strlen("/search")), "write to jobExecutor", )         // send "/search" command
                        noMoreInput = false;
                        while (!noMoreInput) {
                            char word[MAX_SEARCH_WORD_SIZE];
                            char prev = ' ';
                            i = 0;
                            while (i < MAX_SEARCH_WORD_SIZE) {
                                CHECK_PERROR(read(new_connection, word + i, 1), "read from accepted command socket", continue;)
                                if ( prev != ' ' && word[i] == ' ' ){
                                    word[i] = '\0';
                                    CHECK_PERROR(write(toJobExecutor_pipe, " ", 1), "write to jobExecutor", )                   // send a white space
                                    CHECK_PERROR(write(toJobExecutor_pipe, word, strlen(word)), "write to jobExecutor", )       // send word to jobExecutor
                                    break;
                                }
                                else if ( word[i] == '\n' ){         // last word or empty
                                    if (i > 0) {                     // only send word if there was a word to send and not just whitespace
                                        if (i > 0 && word[i - 1] == '\r') word[i - 1] = '\0';
                                        else word[i] = '\0';
                                        CHECK_PERROR(write(toJobExecutor_pipe, " ", 1), "write to jobExecutor", )                   // send a white space
                                        CHECK_PERROR(write(toJobExecutor_pipe, word, strlen(word)), "write to jobExecutor", )       // send word to jobExecutor
                                    }
                                    noMoreInput = true;
                                    break;
                                }
                                else if (prev == ' ' && word[i] == ' ' ){               // ignore continuous white space
                                    continue;
                                }
                                prev = word[i];
                                i++;
                            }
                        }
                        CHECK_PERROR(write(toJobExecutor_pipe, "\n", 1), "write to jobExecutor", )
                        // read and send to socket jobExecutor's answer
                        // IMPORTANT: jobExecutor must print (\n)"<END>\n" after each /search command's output so that we know here when to stop reading!
                        char line[MAX_LINE_SIZE];
                        bool line_too_big = false;
                        for (;;){
                            // read line-by-line
                            i = 0;
                            while( i < MAX_LINE_SIZE - 1 ){
                                CHECK_PERROR(read(fromJobExecutor_pipe, line + i, 1), "read from jobExecutor", continue;)
                                if ( line[i] == '\n' ){
                                    if ( i > 0 && line[i-1] == '\r') line[i-1] = '\0';
                                    else line[i] = '\0';
                                    break;
                                }
                                i++;
                            }
                            if (i == MAX_LINE_SIZE - 1) {
                                cerr << "Warning: an output line was too big for our buffer" << endl;
                                line[i] = '\0';                 // the rest of it will be printed on the next loop?
                                line_too_big = true;
                            }
                            if (strcmp(line, "<END>") == 0){
                                break;
                            } else{
                                CHECK_PERROR( write(new_connection, line, strlen(line)), "write to accepted command socket", );
                                if (!line_too_big) write(new_connection, "\n", 1);
                                else line_too_big = false;     // reset this
                            }
                        }
                    }
                }
                else{
                    cout << "Received illegal command: " << command << endl;     // (!) keep in mind that white spaces sent at start are also considered illegal
                    write(new_connection, "illegal command\n", strlen("illegal command\n"));
                }
                CHECK_PERROR( close(new_connection), "closing accepted command connection", )
            }
            else cerr << "Unexpected return >0 from poll: command socket had no data ready" << endl;
        }
    }

    delete[] pfds;

    if (!monitor_forced_exit) {          // if jobExecutor was initialized in the first place
        CHECK_PERROR(write(toJobExecutor_pipe, "/exit\n", strlen("/exit\n")), "write \"/exit\" to jobExecutor failed",)         // tell jobExecutor to stop
        int stat;
        waitpid(jobExecutor_pid, &stat, 0);
        if (stat != 0) {
            cerr << "(!) jobExecutor terminated with status: " << stat << endl;
        }
    }

    CHECK_PERROR( close(command_socket_fd), "closing command socket", )

    CHECK( pthread_mutex_destroy(&stat_lock) , "pthread_mutex_destroy" , )
    CHECK( pthread_cond_destroy(&QueueIsEmpty), "pthread_cond_destroy", )

    void *status;
    CHECK(pthread_join(monitor_tid, &status), "pthread_join monitor thread",)
    if (status != NULL) { cerr << "monitor thread terminated with an unexpected status" << endl; }
    CHECK( pthread_mutex_destroy(&crawlingFinishedLock) , "pthread_mutex_destroy" , )
    CHECK( pthread_cond_destroy(&crawlingFinished), "pthread_cond_destroy", )

    delete urlQueue;
    delete urlHistory;
    delete alldirs;
    delete[] threadpool;
    delete[] host_or_IP;
    delete[] save_dir;
    delete[] starting_url;
    return 0;
}



/* Local Function Implementation */
int parse_arguements(int argc, char *const *argv, char *&host_or_IP, uint16_t &server_port, uint16_t &command_port, int &num_of_threads, char *&save_dir, char *&starting_url) {
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
    starting_url = new char[strlen(argv[argc - 1]) + 1];
    strcpy(starting_url, argv[argc - 1]);
    return 0;
}
