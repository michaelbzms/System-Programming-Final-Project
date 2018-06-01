#include <iostream>
#include <cstdio>
#include <unistd.h>
#include <cerrno>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>
#include <cstring>
#include <cstdlib>
#include <sys/stat.h>
#include <poll.h>
#include <arpa/inet.h>
#include "../headers/ServeRequestBuffer.h"
#include "../headers/serve_thread.h"


using namespace std;


#define MAX_COMMAND_SIZE 128              // any command more than 128 bytes would be illegal anyway
#define HTTP_REQUEST_QUEUE_SIZE 128       // queue size for incoming serving TCP Connections
#define COMMAND_QUEUE_SIZE 20             // queue size for incoming command TCP Connections (only one command can't be served at one time)
#define FLUSH_SIZE 1024                   // size of the buffer used to flush any command given than was more than 128 Bytes (may or may not be necessary - not sure but I do it just in case)
#define TIME_OUT 30                       // 30 seconds timeout for data to be ACKed in each TCP serving connection (I use the SO_LINGER option)


/* useful macros */
#define CHECK_PERROR(call, callname, handle_code) { if ( ( call ) < 0 ) { perror(callname); handle_code } }
#define CHECK(call, callname, handle_code) { if ( ( call ) < 0 ) { cerr << (callname) << " failed" << endl; handle_code } }


/* Global variables */
time_t time_server_started;
pthread_mutex_t stat_lock;                         // mutex that protects access of global statistics variables
unsigned int total_pages_returned = 0;
unsigned int total_bytes_returned = 0;
char *root_dir = NULL;
ServeRequestBuffer *serve_request_buffer = NULL;   // a FIFO Queue of "unlimited" size is used as buffer for the accepted serving sockets' file descriptors
pthread_cond_t bufferIsReady;                      // condition variable for whether the buffer is empty or not
bool server_must_terminate  = false;               // used (along with a broadcast to bufferIsReady cond_t) to inform the threads to exit because the server must exit


/* Local Functions */
int parse_arguements(int argc, char *const *argv, uint16_t &serving_port, uint16_t &command_port, int &num_of_threads, char **root_dir);


int main(int argc, char *argv[]) {
    time_server_started = time(NULL);
    uint16_t serving_port, command_port;
    int num_of_threads;
    if ( parse_arguements(argc, argv, serving_port, command_port, num_of_threads, &root_dir) < 0 ){
        cerr << "Invalid web server parameters" << endl;
        return -1;
    }
    cout << "Web server initialized with serving port " << serving_port << ", command port " << command_port
         << ", number of threads " << num_of_threads << " and root directory " << root_dir << endl;

    // server and thread should ignore SIGPIPE in case they try to write an answer and the client has closed their connection (or else server would terminate)
    struct sigaction act;
    memset(&act, 0, sizeof(act));
    act.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &act, NULL);

    // Create command socket
    struct sockaddr_in command_sa;
    command_sa.sin_family = AF_INET;
    command_sa.sin_port = htons(command_port);
    command_sa.sin_addr.s_addr = htonl(INADDR_ANY);
    int command_socket_fd;
    CHECK_PERROR( ( command_socket_fd = socket(AF_INET, SOCK_STREAM, 0)) , "command socket" , delete[] root_dir; return -2; )
    CHECK_PERROR( bind(command_socket_fd, (struct sockaddr *) &command_sa, sizeof(command_sa)) , "command socket bind" , close(command_socket_fd); delete[] root_dir; return -2; )
    CHECK_PERROR( listen(command_socket_fd, COMMAND_QUEUE_SIZE) , "command socket listen" , close(command_socket_fd); delete[] root_dir; return -2; )
    cout << "Ready to receive commands..." << endl;

    // create serving socket
    struct sockaddr_in serving_sa;
    serving_sa.sin_family = AF_INET;
    serving_sa.sin_port = htons(serving_port);
    serving_sa.sin_addr.s_addr = htonl(INADDR_ANY);
    int serving_socket_fd;
    CHECK_PERROR( ( serving_socket_fd = socket(AF_INET, SOCK_STREAM, 0)) , "serving socket" , close(command_socket_fd); delete[] root_dir; return -3; )
    CHECK_PERROR( bind(serving_socket_fd, (struct sockaddr *) &serving_sa, sizeof(serving_sa)) , "serving socket bind" , close(serving_socket_fd); close(command_socket_fd); delete[] root_dir; return -3; )
    CHECK_PERROR( listen(serving_socket_fd, HTTP_REQUEST_QUEUE_SIZE) , "serving socket listen" , close(serving_socket_fd); close(command_socket_fd); delete[] root_dir; return -3; )

    // modify serving socket's options so that closing it will block if there is data not ACKed by peer until ACKed or timeout expires
    struct linger ling;
    ling.l_onoff = 1;              // active
    ling.l_linger = TIME_OUT;      // after TIME_OUT seconds the connection will close by the server
    CHECK_PERROR(setsockopt(serving_socket_fd, SOL_SOCKET, SO_LINGER, (const void *)&ling, sizeof(ling)) , "setsockopt" , close(serving_socket_fd); close(command_socket_fd); delete[] root_dir; return -3; )
    cout << "Ready to receive serving requests..." << endl;

    // create the serve request buffer and thread pool
    serve_request_buffer = new ServeRequestBuffer;
    pthread_t *threadpool = new pthread_t[num_of_threads];      // this table shall store the id of num_of_threads threads created to read and handle HTTP GET requests from the buffer

    // init buffer's cond_t, stat's mutex and THEN create num_of_thread threads
    CHECK( pthread_cond_init(&bufferIsReady, NULL) , "pthread_cond_init", close(serving_socket_fd); delete serve_request_buffer; delete[] threadpool; close(command_socket_fd); delete[] root_dir; return -4; )
    CHECK( pthread_mutex_init(&stat_lock, NULL) , "pthread_mutex_init" , close(serving_socket_fd); delete serve_request_buffer; delete[] threadpool; close(command_socket_fd); delete[] root_dir; return -4; )
    for (int i = 0 ; i < num_of_threads ; i++){
        CHECK( pthread_create(&threadpool[i], NULL, handle_http_requests, NULL) , "pthread_create" , threadpool[i] = 0; )   // (!) threadpool[i] = 0 signifies that this thread was not created
    }

    struct pollfd *pfds = new struct pollfd[3];
    pfds[0].fd = command_socket_fd;
    pfds[1].fd = serving_socket_fd;
    pfds[2].fd = -1;                                // <0 so poll will ignore this at start. The whole point of using this is to be able to handle commands as well as serving pages simultaniously
    pfds[0].events = pfds[1].events = pfds[2].events =  POLLIN;
    pfds[0].revents = pfds[1].revents = pfds[2].revents = 0;
    int retval;
    int k = 0;                                      // index of command string
    char command[MAX_COMMAND_SIZE];
    for (;;) {
        retval = poll(pfds, 3, -1);                 // wait indefinitely until a TCP connection request comes
        if ( retval < 0 ){
            if ( errno == EINTR && server_must_terminate ) {    // terminating signal?
                cerr << "poll interrupted, server must terminate" << endl;
                break;
            } else if ( errno == EINTR ){
                cerr << "poll interrupted. Ignoring this event and blocking again..." << endl;
                continue;
            } else {
                perror("poll() failed");
            }
        }
        else if ( retval == 0 ){                    // should never have as time-out given is < 0
            cerr << "Unexpected return 0 from poll" << endl;
            continue;
        }
        else {                                      // got a TCP connection request
            // if got a command (or part of one) on a command connection
            if (pfds[2].revents & POLLIN) {         // pfds[2].fd must be >= 0
                if ( k >= MAX_COMMAND_SIZE ){       // there is no command that big, reject it, after "flushing it" assuming no more than FLUSH_SIZE data is sent
                    cout << "Received illegal command (too big) " << endl;
                    CHECK_PERROR( write(pfds[2].fd, "Illegal command\n", strlen("Illegal command\n")) , "write response to accepted command socket" , )
                    char trash[FLUSH_SIZE];
                    CHECK_PERROR( read(pfds[2].fd, trash, FLUSH_SIZE) , "read from command socket" , continue; )   // wont block cause poll got us here
                    // shall not read more than one command per connection
                    CHECK_PERROR( close(pfds[2].fd) , "close new (command) connection", );
                    pfds[2].fd = -1;                // reset this to < 0 so that a new connection can be accepted
                    pfds[2].revents = 0;            // reset revents field
                    k = 0;                          // reset k (!)
                }
                else{
                    // read (possibly a part of) command
                    ssize_t nbytes = 0;
                    CHECK_PERROR( ( nbytes = read(pfds[2].fd, command + k, MAX_COMMAND_SIZE - k) ) , "read from command socket" , continue; )
                    bool found_endl = false;
                    int pos = -1;
                    for (int j = k ; j < k + nbytes ; j++){             // search the whole nbytes, not just the end, just to be safe. User may sent garbage after the first '\n'
                        if ( command[j] == '\n' ) {
                            found_endl = true;
                            pos = j;
                            break;
                        }
                    }
                    if ( found_endl ){                                 // if there was an '\n' on what we just read then the command has finished (data after the '\n' will be ignored)
                        if ( pos > 0 && command[pos-1] == '\r' ) command[pos-1] = '\0';
                        else command[pos] = '\0';
                        // handle command
                        if ( strcmp(command, "SHUTDOWN") == 0 ){
                            cout << "received SHUTDOWN command" << endl;
                            server_must_terminate = true;              // set this to true so that other threads know to quit
                            pthread_cond_broadcast(&bufferIsReady);    // and then broadcast a "false" cond_t so that they get unblocked from cond_wait and see that server_must_terminate == true !
                            CHECK_PERROR( close(pfds[2].fd) , "close accepted command connection", );  // close connection
                            break;                                     // and exit the forever loop
                        }
                        else if ( strcmp(command, "STATS") == 0 ){
                            cout << "received STATS command" << endl;
                            time_t Dt = time(NULL) - time_server_started;
                            char response[256];
                            CHECK( pthread_mutex_lock(&stat_lock), "pthread_mutex_lock",  )            // must lock stats' mutex to access them consistently
                            sprintf(response, "Server has been up for %.2zu:%.2zu:%.2zu, served %u pages, %u bytes\n", Dt / 3600, (Dt % 3600) / 60 , (Dt % 60), total_pages_returned, total_bytes_returned);
                            CHECK( pthread_mutex_unlock(&stat_lock), "pthread_mutex_unlock",  )
                            CHECK_PERROR( write(pfds[2].fd, response, strlen(response)) , "write response to accepted command socket" , )
                        }
                        else {   // Note: white spaces sent are also considered illegal
                            cout << "Received illegal command: " << command << endl;
                            CHECK_PERROR( write(pfds[2].fd, "Illegal command\n", strlen("Illegal command\n")) , "write response to accepted command socket" , )
                        }
                        // shall not read more than one command per connection. We accept one later in the code, handle it and then close it here
                        CHECK_PERROR( close(pfds[2].fd) , "close new (command) connection", );         // close connection. If there are others blocked on "listen's" queue they will be accepted later in this loop - before blocking on poll again
                        pfds[2].fd = -1;                // reset this to < 0 so that a new connection can be accepted
                        pfds[2].revents = 0;            // reset revents field
                        k = 0;                          // reset k (!)
                    } else {                            // else keep reading until that '\n' or '\r\n'
                        k += nbytes;
                    }
                }
            }
            // if got a command connection and there is not any other accepted command connection pending
            if ( (pfds[0].revents & POLLIN) && pfds[2].fd < 0 ){
                pfds[0].revents = 0;            // reset revents field
                int new_connection;
                struct sockaddr_in incoming_sa;
                socklen_t len = sizeof(incoming_sa);
                CHECK_PERROR((new_connection = accept(command_socket_fd, (struct sockaddr *) &incoming_sa, &len)), "accept on command socket failed unexpectedly", break; )
                cout << "Server accepted a (command) connection from " << inet_ntoa(incoming_sa.sin_addr) << " : " << incoming_sa.sin_port << endl;
                pfds[2].fd = new_connection;
            }
            // if got a serving connection
            if (pfds[1].revents & POLLIN){
                pfds[1].revents = 0;            // reset revents field
                int new_connection;             // this connection shall be closed by the thread that will handle it (and not here)
                struct sockaddr_in incoming_sa;
                socklen_t len = sizeof(incoming_sa);
                CHECK_PERROR((new_connection = accept(serving_socket_fd, (struct sockaddr *) &incoming_sa, &len)), "accept on serving socket failed unexpectedly", break; )
                cout << "Server accepted a (serving) connection from " << inet_ntoa(incoming_sa.sin_addr) << " : " << incoming_sa.sin_port << endl;

                // modify new socket's options so that closing it will block if there is not ACKed by TCP peer data until ACKed or timeout
                CHECK_PERROR(setsockopt(new_connection, SOL_SOCKET, SO_LINGER, (const void *)&ling, sizeof(ling)) , "setsockopt for SO_LINGER failed" , )

                serve_request_buffer->acquire();             // lock the buffer
                serve_request_buffer->push(new_connection);  // push new connection on the buffer's FIFO queue
                CHECK_PERROR( pthread_cond_signal(&bufferIsReady) , "pthread_cond_signal" , )     // signal the cond_t variable so that a thread can read from the buffer (only one can make progress so signal instead of broadcast)
                serve_request_buffer->release();             // unlock the buffer
            }
        }
    }

    delete[] pfds;

    // close your sockets:
    CHECK_PERROR( close(serving_socket_fd) , "closing serving socket",  )
    CHECK_PERROR( close(command_socket_fd) , "closing command socket",  )

    // join with all threads who should be terminating right about now
    void *status;
    for (int i = 0 ; i < num_of_threads ; i++){
        CHECK( pthread_join(threadpool[i], &status) , "pthread_join" , )
        if ( status != 0 ){ cerr << "thread terminated with an unexpected status" << endl; }
    }

    CHECK( pthread_mutex_destroy(&stat_lock) , "pthread_mutex_destroy" , )
    CHECK( pthread_cond_destroy(&bufferIsReady) , "pthread_cond_destroy", )

    // clean up
    delete serve_request_buffer;
    delete[] threadpool;
    delete[] root_dir;
    return 0;
}


/* Local Functions Implementation */
int parse_arguements(int argc, char *const *argv, uint16_t &serving_port, uint16_t &command_port, int &num_of_threads, char **root_dir) {
    bool vital_params_given[3] = {false, false, false} , num_of_threads_given = false;
    for (int i = 1 ; i < argc ; i += 2){
        if ( strcmp(argv[i], "-p") == 0 && i + 1 < argc && argv[i+1][0] != '-' ){
            serving_port = (uint16_t) atoi(argv[i+1]);
            vital_params_given[0] = true;
        }
        else if ( strcmp(argv[i], "-c") == 0 && i + 1 < argc && argv[i+1][0] != '-' ){
            command_port = (uint16_t) atoi(argv[i+1]);
            vital_params_given[1] = true;
        }
        else if ( strcmp(argv[i], "-t") == 0 && i + 1 < argc && argv[i+1][0] != '-' ){
            num_of_threads = atoi(argv[i+1]);
            num_of_threads_given = true;
        }
        else if ( strcmp(argv[i], "-d") == 0 && i + 1 < argc && argv[i+1][0] != '-' ){
            int add_extra_byte = 1;
            if ( argv[i+1][strlen(argv[i+1]) - 1] == '/' ){       // if root_dir arguement has a '/' at the end
                argv[i+1][strlen(argv[i+1]) - 1] = '\0';          // remove it
                add_extra_byte = 0;
            }
            *root_dir = new char[strlen(argv[i+1]) + add_extra_byte];
            strcpy(*root_dir, argv[i+1]);
            vital_params_given[2] = true;
        }
        else{
            if (vital_params_given[2]){
                delete[] *root_dir;
            }
            return -1;
        }
    }
    if ( !num_of_threads_given ){
        num_of_threads = 4;                // default value
    }
    if ( !vital_params_given[0] || !vital_params_given[1] || !vital_params_given[2] || (num_of_threads_given && num_of_threads <= 0) ){
        if (vital_params_given[2]){
            delete[] *root_dir;
        }
        return -1;
    }
    // check if root_dir exists
    struct stat info;
    if( stat( *root_dir, &info ) != 0 ) {
        cerr << "cannot access root directory" << endl;
        delete[] *root_dir;
        return -2;
    }else if( ! ( info.st_mode & S_IFDIR ) ){
        cerr << "given root directory is not a directory" << endl;
        delete[] *root_dir;
        return -3;
    }
    return 0;
}
