#include <iostream>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstdio>
#include <climits>
#include <signal.h>
#include <wait.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <cerrno>
#include "../headers/util.h"
#include "../headers/underlining.h"


using namespace std;


#define WORKER_PATH "./jobExecutor/bin/worker"
#define DEADLINE 30              // deadline cannot be an argument from the webcrawler so #define it here
#define INPUT_BUFFER_SIZE 256    // the acceptable (by us) size in Bytes for reading words from cin
#define MAX_INPUT_WORD_LEN 512


/* useful macros */
#define CHECK_PERROR(call, callname, handle_code) { if ( ( call ) < 0 ) { perror(callname); handle_code } }
#define CHECK(call, callname, handle_code) { if ( ( call ) < 0 ) { cerr << (callname) << " failed" << endl; handle_code } }


/* Global variables */
char **directories = NULL;       // a table containing all of the docfile's directory paths in C strings
int dirfilesize = -1;            // its count
int *workerNum_to_pid = NULL;    // a table of size numWorkers in which: workerNum_to_pid[*worker_num*] = *its_pid*
int numWorkers = -1;             // number of workers
int term_width = -1;

/* Global variables used by signal handlers */
volatile bool a_worker_died = false;
volatile bool jobExecutor_must_terminate = false;
volatile bool* volatile worker_is_dead = NULL;


/* Local functions */
void distribute_directories_to_workers(int numWorkers, int *Rfds);
bool search_management(const int numWorkers, const int *Afds, const int deadline, char **query, int query_size);
bool replace_worker(int worker_num, char **Rpipes, char **Apipes, int *Rfds, int *Afds, char *range_in_str);


/* Signal handlers */
static void handle_child_termination(int sig);
static void handle_sig_termination(int sig);


int main(int argc, char *argv[]) {
    if ( argc < 2 ){
        cerr << "Invalid number of jobExecutor parameters" << endl;
        return -1;
    }
    numWorkers = atoi(argv[1]);
    if (numWorkers <= 0){
        cerr << "Invalid jobExecutor parameters" << endl;
        return -1;
    }

    // read term_width from webcrawler
    CHECK_PERROR(read(STDIN_FILENO, &term_width, sizeof(int)), "read from cin", )

    // read directories from cin (webcrawler)
    CHECK_PERROR(read(STDIN_FILENO, &dirfilesize, sizeof(int)), "read from cin", )
    if (dirfilesize <= 0 ){
    	cerr << "Invalid jobExecutor number of directories (<=0)" << endl;
    	return -1;
    }
    directories = new char*[dirfilesize];
    for (int i = 0 ; i < dirfilesize ; i++){
        int len = 0;
        CHECK_PERROR(read(STDIN_FILENO, &len, sizeof(int)), "read from cin", )
        directories[i] = new char[len+1];
        CHECK_PERROR(read(STDIN_FILENO, directories[i], len), "read from cin", )
        directories[i][len] = '\0';
    }

    if ( numWorkers > dirfilesize ) {             // can't have more workers than directories
        numWorkers = dirfilesize;
    }

    /* Load balancing logic: Each worker gets an (almost) equal number (range) of directories */
    char range_in_str[128];
    const int range = dirfilesize / numWorkers + ((dirfilesize % numWorkers > 0) ? 1 : 0);    // take the ceiling of integer division - numWorkers cant be <= 0
    sprintf(range_in_str, "%d", range);

    /* Pipe names for communicaton with numWorkers workers */
    char **Rpipes = new char*[numWorkers];
    char **Apipes = new char*[numWorkers];
    workerNum_to_pid = new int[numWorkers];
    for (int i = 0 ; i < numWorkers ; i++){
        Rpipes[i] = new char[128];
        Apipes[i] = new char[128];
        workerNum_to_pid[i] = -1;
    }

    /* Initialize worker "dead_or_alive" table */
    worker_is_dead = new volatile bool[numWorkers];
    for (int i = 0 ; i < numWorkers ; i++){
        worker_is_dead[i] = false;
    }

    /* Signal handling */
    struct sigaction act;
    memset(&act, 0, sizeof(act));
    act.sa_handler = handle_child_termination;
    sigaction(SIGCHLD, &act, NULL);


    memset(&act, 0, sizeof(act));
    act.sa_handler = handle_sig_termination;
    sigaction(SIGINT, &act, NULL);
    sigaction(SIGQUIT, &act, NULL);
    sigaction(SIGTERM, &act, NULL);

    memset(&act, 0, sizeof(act));
    act.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &act, NULL);                 // ignore SIGPIPE signal

    /* Create numWorkers workers along with their named pipes */
    for (int  i = 0 ; i < numWorkers ; i++) {
        // the pipe name for each child is unique as it uses the number of the specific worker and it is:
        // "pipe*R/A**worker_num*" where *R/A* is 'R' (Request) or 'A' (Answer) depending on pipe's direction
        char worker_num[128];
        sprintf(worker_num, "%d", i);
        strcpy(Rpipes[i], "pipeR");
        strcat(Rpipes[i], worker_num);
        strcpy(Apipes[i], "pipeA");
        strcat(Apipes[i], worker_num);
        if (mkfifo(Rpipes[i], S_IRUSR | S_IWUSR) < 0) {
            perror("Failed to create a named pipe");
            // cleanup previously allocated resources
            for (int j = 0 ; j < i ; j++){
                unlink(Rpipes[j]);
                unlink(Apipes[j]);
            }
            for (int j = 0 ; j < numWorkers ; j++ ){
                if (workerNum_to_pid[j] != -1) {
                    kill(workerNum_to_pid[j], SIGINT);
                }
                delete[] Rpipes[j];
                delete[] Apipes[j];
            }
            delete[] Rpipes;
            delete[] Apipes;
            for (int j = 0 ; j < dirfilesize ; j++){
                delete[] directories[j];
            }
            delete[] directories;
            delete[] workerNum_to_pid;
            delete[] worker_is_dead;
            int status;
            for (int j = 0 ; j < numWorkers ; j++) wait(&status);    // wait for children to die
            return -2;
        }
        if (mkfifo(Apipes[i], S_IRUSR | S_IWUSR) < 0) {
            perror("Failed to create a named pipe");
            // cleanup previously allocated resources
            for (int j = 0 ; j < i ; j++){
                unlink(Rpipes[j]);
                unlink(Apipes[j]);
            }
            unlink(Rpipes[i]);
            for (int j = 0 ; j < numWorkers ; j++ ){
                if (workerNum_to_pid[j] != -1) {
                    kill(workerNum_to_pid[j], SIGINT);
                }
                delete[] Rpipes[j];
                delete[] Apipes[j];
            }
            delete[] Rpipes;
            delete[] Apipes;
            for (int j = 0 ; j < dirfilesize ; j++){
                delete[] directories[j];
            }
            delete[] directories;
            delete[] workerNum_to_pid;
            delete[] worker_is_dead;
            int status;
            for (int j = 0 ; j < numWorkers ; j++) wait(&status);    // wait for children to die
            return -2;
        }
        pid_t pid = fork();
        if ( pid < 0 ){
            perror("Failed to create a child process");
            break;
        } else if ( pid == 0 ) {         // child process
            execl(WORKER_PATH, "worker", range_in_str, Rpipes[i], Apipes[i], NULL);
            /* Code continues to run only if exec fails: (most likely because the executable file could not be found) */
            perror("exec() failed");
            // cleanup
            for (int j = 0 ; j <= i ; j++){
                unlink(Rpipes[j]);
                unlink(Apipes[j]);
            }
            for (int j = 0 ; j < numWorkers ; j++){
                if (workerNum_to_pid[j] != -1) {
                    kill(workerNum_to_pid[j], SIGINT);
                }
                delete[] Rpipes[j];
                delete[] Apipes[j];
            }
            delete[] Rpipes;
            delete[] Apipes;
            for (int j = 0 ; j < dirfilesize ; j++){
                delete[] directories[j];
            }
            delete[] directories;
            delete[] workerNum_to_pid;
            delete[] worker_is_dead;
            int status;
            for (int j = 0 ; j < numWorkers ; j++) wait(&status);    // wait for children to die
            return -3;
        } else{                          // parent process
            workerNum_to_pid[i] = pid;   // save child's pid
        }
    }   /* Only parent process continues in this point */

    /* Prepare communication pipes */
    int *Rfds = new int[numWorkers];
    int *Afds = new int[numWorkers];

    /* open all Rpipes[i] for O_WRONLY */
    for (int i = 0 ; i < numWorkers ; i++){
        Rfds[i] = open(Rpipes[i], O_WRONLY);                  // Block if a worker is not ready yet should be acceptable. They should all open the pipe asap.
        if ( Rfds[i] < 0 ){
            perror("could not open a request pipe");
            // cleanup
            for (int j = 0 ; j < numWorkers ; j++){
                unlink(Rpipes[j]);
                unlink(Apipes[j]);
            }
            for (int j = 0 ; j < numWorkers ; j++){
                if (workerNum_to_pid[j] != -1) {
                    kill(workerNum_to_pid[j], SIGINT);
                }
                delete[] Rpipes[j];
                delete[] Apipes[j];
            }
            delete[] Rpipes;
            delete[] Apipes;
            for (int j = 0 ; j < dirfilesize ; j++){
                delete[] directories[j];
            }
            delete[] directories;
            delete[] workerNum_to_pid;
            delete[] worker_is_dead;
            int status;
            for (int j = 0 ; j < numWorkers ; j++) wait(&status);    // wait for children to die
            return -4;
        }
    }

    /* load balancing of directories to workers via the Rpipes */
    distribute_directories_to_workers(numWorkers, Rfds);      // this functions opens all Rpipes for O_WRONLY onto the Rfds table

    /* Now open all Apipes[i] for O_RDONLY and block so that the jobExecutor and the workers are "in sync" */
    for (int i = 0 ; i < numWorkers ; i++) {
        Afds[i] = open(Apipes[i], O_RDONLY);
        if ( Afds[i] < 0 ){
            perror("could not open an answer pipe");
            // cleanup
            for (int j = 0 ; j < numWorkers ; j++){
                unlink(Rpipes[j]);
                unlink(Apipes[j]);
            }
            for (int j = 0 ; j < numWorkers ; j++){
                if (workerNum_to_pid[j] != -1) {
                    kill(workerNum_to_pid[j], SIGINT);
                }
                delete[] Rpipes[j];
                delete[] Apipes[j];
            }
            delete[] Rpipes;
            delete[] Apipes;
            for (int j = 0 ; j < dirfilesize ; j++){
                delete[] directories[j];
            }
            delete[] directories;
            delete[] workerNum_to_pid;
            delete[] worker_is_dead;
            int status;
            for (int j = 0 ; j < numWorkers ; j++) wait(&status);    // wait for children to die
            return -5;
        }
    }

    /* Endless loop repeatedly reads a command from the user after prompting him until "/exit" command */
    char command[INPUT_BUFFER_SIZE];
    while( !jobExecutor_must_terminate ){                     // forever loop but stop if signaled to terminate
        cin >> command;
        if (jobExecutor_must_terminate){
            break;
        }
        if ( a_worker_died ) {                                // if a worker died whilst we were blocked on input I/O we can replace him here
            a_worker_died = false;
            // replace all dead workers:
            for (int i = 0 ; i < numWorkers ; i++){
                if (worker_is_dead[i]){
                    worker_is_dead[i] = false;
                    if ( replace_worker(i, Rpipes, Apipes, Rfds, Afds, range_in_str) == false ){
                        cerr << "(1) Failed to replace a worker!" << endl;
                        worker_is_dead[i] = true;
                    }
                }
            }
            if ( cin.fail() ) {                               // fix cin : receiving a signal whilst blocked for I/O makes the I/O fail with cin.fail() true
                cin.clear();
                continue;                                     // re-read a command by skipping this loop
            }
        }
        if ( cin.fail() || strcmp(command, "/exit") == 0 ){   // command "/exit" or cin failed to read a command
            cin.clear();
            break;
        }
        else if ( strcmp(command, "/search") == 0 ){          // maximum words for query are 10 (the rest are ignored)
            int deadline = DEADLINE;
            char **words = new char*[10];
            int i = 0;
            ignore_whitespace(cin);
            while ( cin.peek() != '\n' && i < 10 ) {
                words[i] = new char[MAX_INPUT_WORD_LEN];
                cin >> words[i];
                ignore_whitespace(cin);
                i++;
                if (cin.fail()){
                    cout << "Error: a word was too big?" << endl;
                    cin.clear();
                    break;
                }
            }
            cin.ignore(numeric_limits<streamsize>::max(), '\n');
            if (jobExecutor_must_terminate){
                for (int j = 0 ; j < i ; j++){
                    delete[] words[j];
                }
                delete[] words;
                break;
            }
            // send request message to ALL workers (that contains the query)  (!) must not terminate whilst in the middle of sending a request (either send it all or none)!
            Header header(SEARCH, i);                                             // header for how many words the search query has
            for (int j = 0 ; j < numWorkers && !jobExecutor_must_terminate ; j++){
                if (worker_is_dead[j]){
                    worker_is_dead[j] = false;
                    if ( replace_worker(j, Rpipes, Apipes, Rfds, Afds, range_in_str) == false ){
                        cerr << "Failed to replace a worker!" << endl;
                        worker_is_dead[j] = true;
                    }
                }
                if ( write(Rfds[j], &header, sizeof(Header)) < 0 ) continue;      // worker died immediately?
                for (int k = 0 ; k < i ; k++){
                    Header wordhdr(NOT_USED, strlen(words[k]) + 1);
                    if ( write(Rfds[j], &wordhdr, sizeof(Header)) < 0  ||         // header for each word
                         write(Rfds[j], words[k], strlen(words[k]) + 1) < 0 ) {   // the word itself, where wordhdr.message_size == strlen(words[k]) + 1
                        break;
                    }
                }
            }
            // wait for their responses - but only for deadline seconds - and print each result line:
            bool feedback = search_management(numWorkers, Afds, deadline, words, i);
            if (!feedback){                                                       // then jobExecutor must terminate
                for (int j = 0 ; j < i ; j++){
                    delete[] words[j];
                }
                delete[] words;
                break;
            }
            for (int j = 0 ; j < i ; j++){
                delete[] words[j];
            }
            delete[] words;
        }
        else if ( strcmp(command, "/maxcount") == 0 ){
            char keyword[MAX_INPUT_WORD_LEN];
            cin >> keyword;
            cin.ignore(numeric_limits<streamsize>::max(), '\n');
            if (cin.fail()){
                cout << "Error: input word too big?" << endl;
                cin.clear();
            }
            else{
                // send a request message to each worker
                Header header(MAXCOUNT, strlen(keyword) + 1);
                for (int i = 0 ; i < numWorkers && !jobExecutor_must_terminate ; i++){
                    if (worker_is_dead[i]){                                       // last chance to replace any dead workers
                        worker_is_dead[i] = false;
                        if ( replace_worker(i, Rpipes, Apipes, Rfds, Afds, range_in_str) == false ){
                            cerr << "Failed to replace a worker!" << endl;
                            worker_is_dead[i] = true;
                        }
                    }
                    // if the i-th  worker dies right now the two following writes will fail but that's ok
                    write(Rfds[i], &header, sizeof(Header));
                    write(Rfds[i], keyword, strlen(keyword) + 1);
                }
                if (jobExecutor_must_terminate){
                    break;
                }
                // prepare for poll():
                int deadcount = 0;
                int max = -1;
                char *maxfpath = new char[strlen("There is no text file containing given keyword") + 1];
                strcpy(maxfpath, "There is no text file containing given keyword");
                int retval;
                struct pollfd *pfds = new struct pollfd[numWorkers];
                for (int i = 0 ; i < numWorkers ; i++){
                    pfds[i].fd = Afds[i];
                    pfds[i].events = POLLIN;
                    pfds[i].revents = 0;
                }
                bool *responded = new bool[numWorkers];                          // a bool table for which of the workers have responded
                for (int i = 0 ; i < numWorkers ; i++) {
                    responded[i] = false;
                }
                for (int k = 0 ; k < numWorkers && !jobExecutor_must_terminate; ){
                    for (int i = 0 ; i < numWorkers ; i++){
                        if ( !responded[i] && worker_is_dead[i] ) {
                            // do not expect any answer from them!
                            deadcount++;
                            responded[i] = true;
                            pfds[i].fd = -1;                                    // poll() can now ignore this file descriptor
                            k++;
                        }
                    }
                    if ( k >= numWorkers ){
                        break;
                    }
                    retval = poll(pfds, numWorkers, -1);                        // wait indefinitely until all workers answer
                    if ( retval < 0 ){
                        if ( errno == EINTR && jobExecutor_must_terminate ) {   // terminating signal
                            break;
                        } else if (errno != EINTR) {
                            perror("poll() failed");
                        }
                        // if errno == EINTR then a worker must have died - he will be accounted for in the start of the next loop
                    }
                    else if ( retval == 0 ){        // should never have as time-out given is < 0
                        cerr << "Unexpected return (0) from poll" << endl;
                    }
                    else {                          // somebody answered
                        for (int i = 0 ; i < numWorkers && !jobExecutor_must_terminate ; i++){
                            if ( !responded[i] && (pfds[i].revents & POLLIN) ) {     // i-th worker has answered  (bit-wise and)
                                responded[i] = true;
                                pfds[i].fd = -1;                                     // poll() can now ignore this file descriptor
                                k++;
                                ssize_t feedback;
                                while ( (feedback = read(Afds[i], &header, sizeof(Header))) < 0 && errno == EINTR ) ;              // if interupted by a signal keep trying to read
                                if (feedback > 0 && header.type == MAXCOUNT) {      // worker successfully executed /mincount
                                    char *fpath = new char[header.message_size];
                                    int maxcount = -1;
                                    ssize_t bytes_read;
                                    while ( ( bytes_read = read(Afds[i], fpath,  header.message_size) ) < 0 && errno == EINTR ) ;   // if interupted by a signal keep trying to read
                                    if (bytes_read <= 0){
                                        deadcount++;
                                        delete[] fpath;
                                        continue;
                                    }
                                    while ( ( bytes_read = read(Afds[i], &maxcount, sizeof(int)) ) < 0 && errno == EINTR ) ;        // if interupted by a signal keep trying to read
                                    if (bytes_read <= 0){
                                        deadcount++;
                                        delete[] fpath;
                                        continue;
                                    }
                                    if (maxcount > max || (maxcount == max && strcmp(fpath, maxfpath) < 0)) {
                                        max = maxcount;
                                        delete[] maxfpath;
                                        maxfpath = fpath;
                                    } else {
                                        delete[] fpath;
                                    }
                                } else if ( feedback <= 0 ) {         // worker died
                                    deadcount++;
                                }
                                else if (header.type == ERROR) {     // should not happen
                                    cerr << "maxcount failed on a worker!" << endl;
                                }
                                else if (header.type != END_OF_MESSAGES){
                                    cerr << "Unknown header type at maxcount!" << endl;
                                }
                                // else if header.type == END_OF_MESSAGES then all of this worker's text files did not contain the keyword
                            }
                        }
                    }
                }
                delete[] pfds;
                delete[] responded;
                if ( jobExecutor_must_terminate ){                  // received terminating signal
                    delete[] maxfpath;
                    break;
                }
                // print output
                cout << maxfpath;
                if ( max != -1) cout << "  count: " << max << endl;
                cout << endl;
                delete[] maxfpath;
                if ( deadcount > 0 ) cout << deadcount << " out of " << numWorkers << " workers did not answer because they were forced to terminate" << endl;
            }
        }
        else if ( strcmp(command, "/mincount") == 0 ){
            char keyword[MAX_INPUT_WORD_LEN];
            cin >> keyword;
            cin.ignore(numeric_limits<streamsize>::max(), '\n');
            if (cin.fail()){
                cout << "Error: input word too big?" << endl;
                cin.clear();
            }
            else{
                // send a request message to each worker
                Header header(MINCOUNT, strlen(keyword) + 1);
                for (int i = 0 ; i < numWorkers && !jobExecutor_must_terminate ; i++){
                    if (worker_is_dead[i]){                                    // last chance to replace any dead workers
                        worker_is_dead[i] = false;
                        if ( replace_worker(i, Rpipes, Apipes, Rfds, Afds, range_in_str) == false ){
                            cerr << "Failed to replace a worker!" << endl;
                            worker_is_dead[i] = true;
                        }
                    }
                    // if the i-th  worker dies right now the two following writes will fail but that's ok
                    write(Rfds[i], &header, sizeof(Header));
                    write(Rfds[i], keyword, strlen(keyword) + 1);
                }
                if (jobExecutor_must_terminate){
                    break;
                }
                // prepare for poll():
                int deadcount = 0;
                int min = INT_MAX;
                char *minfpath = new char[strlen("There is no text file containing given keyword") + 1];
                strcpy(minfpath, "There is no text file containing given keyword");
                int retval;
                struct pollfd *pfds = new struct pollfd[numWorkers];
                for (int i = 0 ; i < numWorkers ; i++){
                    pfds[i].fd = Afds[i];
                    pfds[i].events = POLLIN;
                    pfds[i].revents = 0;
                }
                bool *responded = new bool[numWorkers];                       // a bool table for which of the workers have responded
                for (int i = 0 ; i < numWorkers ; i++) {
                    responded[i] = false;
                }
                for (int k = 0 ; k < numWorkers && !jobExecutor_must_terminate ; ){
                    for (int i = 0 ; i < numWorkers ; i++){
                        if ( !responded[i] && worker_is_dead[i] ) {
                            // do not expect any answer from them!
                            deadcount++;
                            responded[i] = true;
                            pfds[i].fd = -1;                                  // poll() can now ignore this file descriptor
                            k++;
                        }
                    }
                    if ( k >= numWorkers ){
                        break;
                    }
                    retval = poll(pfds, numWorkers, -1);                      // wait indefinitely until all workers answer
                    if ( retval < 0 ) {
                        if (errno == EINTR && jobExecutor_must_terminate) {   // terminating signal
                            break;
                        } else if (errno != EINTR) {
                            perror("poll() failed");
                        }
                        // if errno == EINTR then a worker must have died - he will be accounted for in the start of the next loop
                    }
                    else if ( retval == 0 ){                     // should never have as time-out given is < 0
                        cerr << "Unexpected return (0) from poll" << endl;
                    }
                    else {                                       // somebody answered
                        for (int i = 0 ; i < numWorkers && !jobExecutor_must_terminate ; i++){
                            if ( !responded[i] && (pfds[i].revents & POLLIN) ) {     // i-th worker has answered  (bit-wise and)
                                responded[i] = true;
                                pfds[i].fd = -1;                                     // poll() can now ignore this file descriptor
                                k++;                                                 // increment the amount of workers that have answered (k)
                                ssize_t feedback;
                                while ( (feedback = read(Afds[i], &header, sizeof(Header))) < 0 && errno == EINTR ) ;              // if interupted by a signal keep trying to read
                                if (feedback > 0 && header.type == MINCOUNT) {      // worker successfully executed /mincount
                                    char *fpath = new char[header.message_size];
                                    int mincount = -1;
                                    ssize_t bytes_read;
                                    while ( ( bytes_read = read(Afds[i], fpath,  header.message_size) ) < 0 && errno == EINTR ) ;   // if interupted by a signal keep trying to read
                                    if (bytes_read <= 0){
                                        deadcount++;
                                        delete[] fpath;
                                        continue;
                                    }
                                    while ( ( bytes_read = read(Afds[i], &mincount, sizeof(int)) ) < 0 && errno == EINTR ) ;        // if interupted by a signal keep trying to read
                                    if (bytes_read <= 0){
                                        deadcount++;
                                        delete[] fpath;
                                        continue;
                                    }
                                    if (mincount < min || (mincount == min && strcmp(fpath, minfpath) < 0)) {
                                        min = mincount;
                                        delete[] minfpath;
                                        minfpath = fpath;
                                    } else {
                                        delete[] fpath;
                                    }
                                } else if ( feedback <= 0 ) {         // worker died
                                    deadcount++;
                                }
                                else if (header.type == ERROR) {     // should not happen
                                    cerr << "mincount failed on a worker!" << endl;
                                }
                                else if (header.type != END_OF_MESSAGES){
                                    cerr << "Unknown header type at mincount!" << endl;
                                }
                                // else if header.type == END_OF_MESSAGES then all of this worker's text files did not contain the keyword
                            }
                        }
                    }
                }
                delete[] pfds;
                delete[] responded;
                if ( jobExecutor_must_terminate ){                   // received terminating signal
                    delete[] minfpath;
                    break;
                }
                // print output
                cout << minfpath;
                if ( min != INT_MAX ) cout << "  count: " << min << endl;
                cout << endl;
                delete[] minfpath;
                if ( deadcount > 0 ) cout << deadcount << " out of " << numWorkers << " workers did not answer because they were forced to terminate" << endl;
            }
        }
        else if ( strcmp(command, "/wc") == 0 ){
            cin.ignore(numeric_limits<streamsize>::max(), '\n');
            // send a request message to each worker
            Header header(WORDCOUNT, 0);
            for (int i = 0 ; i < numWorkers && !jobExecutor_must_terminate ; i++){
                if (worker_is_dead[i]){             // last chance to replace any dead workers
                    worker_is_dead[i] = false;
                    if ( replace_worker(i, Rpipes, Apipes, Rfds, Afds, range_in_str) == false ){
                        cerr << "Failed to replace a worker!" << endl;
                        worker_is_dead[i] = true;
                    }
                }
                // if the i-th worker dies right now the following write will fail but that's ok
                write(Rfds[i], &header, sizeof(Header));
            }
            // sum all results
            int bytesum = 0, wordsum = 0, linesum = 0;
            int deadcount = 0;
            // wc command for workers is O(1), so any performance boost from using poll instead of this loop would be trivial
            for (int i = 0 ; i < numWorkers && !jobExecutor_must_terminate ; i++){
                if ( !worker_is_dead[i] ) {        // if worker was replaced after writing the request then do not wait for an answer
                    int bytes = 0, words = 0, lines = 0;
                    ssize_t bytes_read;
                    while ( (bytes_read = read(Afds[i], &bytes, sizeof(int))) < 0 && errno == EINTR ) ;    // if interupted by a signal keep trying to read
                    if ( bytes_read <= 0 ){         // if read failed because the pipe is closed at the other end then the worker is probably dead
                        deadcount++;
                        continue;
                    }
                    while ( (bytes_read = read(Afds[i], &words, sizeof(int))) < 0 && errno == EINTR ) ;    // if interupted by a signal keep trying to read
                    if ( bytes_read <= 0 ){
                        deadcount++;
                        continue;
                    }
                    while ( (bytes_read = read(Afds[i], &lines, sizeof(int))) < 0 && errno == EINTR ) ;    // if interupted by a signal keep trying to read
                    if ( bytes_read <= 0 ){
                        deadcount++;
                        continue;
                    }
                    bytesum += bytes;
                    wordsum += words;
                    linesum += lines;

                } else {
                    deadcount++;
                }
            }
            if (jobExecutor_must_terminate){
                break;
            }
            // print output
            cout << "Bytes: " << bytesum << "  "
                 << "Words: " << wordsum << "  "
                 << "Lines: " << linesum << endl;
            if (deadcount > 0) cout << deadcount << " out of " << numWorkers << " workers did not answer because they were forced to terminate" << endl;
        }
        else{
            cout << "Unrecognized command\n";
            cin.ignore(numeric_limits<streamsize>::max(), '\n');
        }

        // VERY IMPORTANT: AT THE END OF EACH COMMAND print "<END>\n" to cout so that webcrawler knows that the command answer has finished!
        cout << "<END>\n";

        // replace all dead workers:
        if ( a_worker_died ) {
            a_worker_died = false;
            for (int i = 0; i < numWorkers; i++) {
                if (worker_is_dead[i]) {
                    worker_is_dead[i] = false;
                    if ( replace_worker(i, Rpipes, Apipes, Rfds, Afds, range_in_str) == false ){
                        cerr << "(2) Failed to replace a worker!" << endl;
                        worker_is_dead[i] = true;
                    }
                }
            }
            if ( cin.fail() ) {                             // fix cin : receiving a signal whilst blocked for I/O makes the I/O fail with cin.fail() true
                cin.clear();
            }
        }
    }
    // notify workers to stop - but only if terminating normally (not via a signal) - otherwise they will terminate via a SIGINT sent by our termination signal handler
    if ( !jobExecutor_must_terminate ){
        memset(&act, 0, sizeof(act));
        act.sa_handler = SIG_IGN;
        sigaction(SIGCHLD, &act, NULL);                     // stop handling SIGCHLD as we do not want to replace any dead workers at this point
        Header header(EXIT, 0);
        for (int i = 0 ; i < numWorkers ; i++){
            write (Rfds[i], &header, sizeof(Header));
            // read from him the number of (different) words that got searched successfully
            int TotalWordsFound = -1;
            read(Afds[i], &TotalWordsFound, sizeof(int));
            cout << "worker " << i + 1 << " found " << TotalWordsFound << " (different) word(s) in /search commands." << endl;
        }
    } else {
        // notify all workers to stop (if they exist) (just in case we were replacing a worker when jobExecutor got a termination signal, this should happen here)
        for (int i = 0 ; i < numWorkers ; i++){
            pid_t pid = workerNum_to_pid[i];
            if (pid != -1) {
                kill(pid, SIGINT);                         // send SIGΙΝΤ to all workers so that they terminate as well - but only if they exist
            }
        }
    }
    // close your end of the pipes
    for (int i = 0 ; i < numWorkers ; i++){
        close(Rfds[i]);
        close(Afds[i]);
    }
    // cleanup time:
    delete[] Rfds;
    delete[] Afds;
    for (int i = 0 ; i < dirfilesize ; i++){
        delete[] directories[i];
    }
    delete[] directories;
    delete[] workerNum_to_pid;
    delete[] worker_is_dead;
    // unlink named pipes created
    for (int i = 0 ; i < numWorkers ; i++){
        unlink(Rpipes[i]);
        unlink(Apipes[i]);
    }
    for (int i = 0 ; i < numWorkers ; i++){
        delete[] Rpipes[i];
        delete[] Apipes[i];
    }
    delete[] Rpipes;
    delete[] Apipes;
    int status;
    for (int i = 0 ; i < numWorkers ; i++) wait(&status);    // wait for children to die
    return ((jobExecutor_must_terminate) ? 1 : 0);
}


/* Local functions implementation */
void distribute_directories_to_workers(int numWorkers, int *Rfds) {
    int k = 0;                                       // ID of worker
    for (int i = 0 ; i < dirfilesize ; i++) {        // for each directory cycle through all workers, on each loop the k-th worker gets assigned with the i-th directory
        if (k >= numWorkers) {                       // this makes it a circle
            k = 0;
        }
        Header header(INIT, strlen(directories[i]) + 1);
        if (dirfilesize - i - numWorkers <= 0) {
            header.type = END_OF_MESSAGES;           // type should be END_OF_MESSAGES if we are not going to send another message (aka assign another directory) to this worker
        }
        write(Rfds[k], &header, sizeof(Header));
        write(Rfds[k], directories[i], header.message_size);
        k++;
    }
}


bool search_management(const int numWorkers, const int *Afds, const int deadline, char **query, int query_size) {
    bool *responded = new bool[numWorkers];                         // a bool table for which of the workers have responded
    for (int j = 0 ; j < numWorkers ; j++) responded[j] = false;
    int retval;
    struct pollfd *pfds = new struct pollfd[numWorkers];
    for (int i = 0 ; i < numWorkers ; i++){
        pfds[i].fd = Afds[i];
        pfds[i].events = POLLIN;
        pfds[i].revents = 0;
    }
    int print_counter = 1;
    int time_passed = 0;
    int deadcount = 0;
    bool no_results = true;
    for (int k = 0 ; k < numWorkers ; ) {
        // Check if jobExecutor received a terminating signal
        if ( jobExecutor_must_terminate ) {
            delete[] pfds;
            delete[] responded;
            return false;
        }
        for (int i = 0 ; i < numWorkers ; i++){
            if ( !responded[i] && worker_is_dead[i] ) {
                // do not expect any answer from them!
                deadcount++;
                responded[i] = true;
                pfds[i].fd = -1;                                    // poll() can now ignore this file descriptor
                k++;
            }
        }
        if ( k >= numWorkers ){
            break;
        }
        // in each loop I wait for one or more workers to be ready to answer
        // until all workers have answered or the given deadline has been exhausted
        time_t t0 = time(NULL);
        retval = poll(pfds, numWorkers, (deadline - time_passed) * 1000);     // wait for deadline - time_passed seconds (=> deadline seconds throughout this for-loop)
        if (retval < 0) {
            if ( errno == EINTR && jobExecutor_must_terminate ) {    // terminating signal
                delete[] pfds;
                delete[] responded;
                return false;
            } else if ( errno != EINTR ) {
                perror("poll() failed");
            }
            // if errno == EINTR then a worker must have died - he will be accounted for in the start of the next loop
        } else if (retval == 0) {                                    // No data within deadline (throughout this for-loop) seconds
            for (int j = 0 ; j < numWorkers ; j++){
                if ( !responded[j] ){
                    cout << "worker " << j + 1 << " did not respond in time" << endl;
                }
            }
            // signal workers to stop searching
            for (int j = 0 ; j < numWorkers ; j++){
                if (!worker_is_dead[j]) kill(workerNum_to_pid[j], SIGUSR1);
                // each user should now send an END_OF_MESSAGED header as soon as possible and then stop /searching (unless he is dead)
            }
            // Wait for all of the remaining workers to finish before letting the user issue a new command:
            for (int j = 0 ; j < numWorkers ; j++){
                if ( !responded[j] ) {   // j-th worker has answered
                    k++;                                             // increment the amount of workers that have answered (k)
                    // responded[j] = true;                          // this is unnecessary
                    for(;;) {                                        // read each line of the output repeatedly
                        Header queryhdr;
                        ssize_t bytes_read;
                        while ( ( bytes_read =  read(Afds[j], &queryhdr, sizeof(Header)) ) < 0 && errno == EINTR ) ;          // if we get interupted by a signal keep trying to read
                        if ( bytes_read <= 0 ){
                            deadcount++;
                            break;
                        }
                        if (queryhdr.type == END_OF_MESSAGES || queryhdr.message_size == 0) {     // until worker "says" he has finished
                            break;
                        } else {
                            char *out_line = new char[queryhdr.message_size];
                            while ( (bytes_read = read(Afds[j], out_line, queryhdr.message_size)) < 0 && errno == EINTR ) ;   // if we get interupted by a signal keep trying to read
                            if ( bytes_read <= 0 ){
                                deadcount++;
                                delete[] out_line;
                                break;
                            }
                            delete[] out_line;
                        }
                    }
                }
            }
            break;
        } else {                                                     // a worker answered in time:
            for (int j = 0 ; j < numWorkers && !jobExecutor_must_terminate ; j++){
                if ( !responded[j] && (pfds[j].revents & POLLIN) ) { // j-th worker has answered  (bit-wise and)
                    pfds[j].fd = -1;                                 // poll() can now ignore this file descriptor
                    k++;                                             // increment the amount of workers that have answered (k)
                    responded[j] = true;
                    for(;;) {                                        // read each line of the output repeatedly
                        Header queryhdr;
                        ssize_t bytes_read;
                        while ( ( bytes_read =  read(Afds[j], &queryhdr, sizeof(Header)) ) < 0 && errno == EINTR ) ;         // if we get interupted by a signal keep trying to read
                        if ( bytes_read <= 0 ){
                            deadcount++;
                            break;
                        }
                        if (queryhdr.type == END_OF_MESSAGES || queryhdr.message_size == 0) {      // until worker "says" he has finished
                            break;
                        } else {
                            char *out_line = new char[queryhdr.message_size];
                            while ( (bytes_read = read(Afds[j], out_line, queryhdr.message_size)) < 0 && errno == EINTR ) ;  // if we get interupted by a signal keep trying to read
                            if ( bytes_read <= 0 ){
                                deadcount++;
                                delete[] out_line;
                                break;
                            }

                            no_results = false;

                            // Without underlining:
                            // cout << (print_counter++) << ". " << out_line << endl << endl;

                            // With underlining:
                            //struct winsize wsize;
                            //ioctl(STDOUT_FILENO, TIOCGWINSZ, &wsize);  // (!) cannot get terminal width here because STDOUT_FILENO is redirected to a pipe - instead the webcrawler sends that at start up
                            char *line = strtok(out_line, "\n");         // split line into two parts
                            cout << (print_counter++) << ". " << line << endl;
                            line = strtok(NULL, "\n");
                            print_text(line, query, query_size, term_width);
                            cout << endl;
                            delete[] out_line;
                        }
                    }
                }
            }
        }
        // calculate time passed since last select() call
        time_t t1 = time(NULL);
        time_passed += t1 - t0;
        if ( time_passed >= deadline ){
            time_passed = deadline;         // so that the next poll() call will be with time-out 0 and hence not block
            // this way we give one last chance to check if somebody answered within the deadline (more or less)
        }
    }
    if ( no_results ) cout << "Given word" << ((query_size > 1) ? "s do" : " does") <<  " not exist in any text file" << endl;
    if ( deadcount > 0 ) cout << deadcount << " workers did not answer because they were forced to terminate" << endl;
    delete[] pfds;
    delete[] responded;
    return true;
}


bool replace_worker(int worker_num, char **Rpipes, char **Apipes, int *Rfds, int *Afds, char *range_in_str) {
    // close previous named pipes before forking (!!!)
    if (Rfds[worker_num] != -1) {
        close(Rfds[worker_num]);
    }
    if (Afds[worker_num] != -1) {   // empty previous answer pipe contents - if any - because worker might have terminated after writing part of an answer for an operation
        int FLAGS = fcntl(Afds[worker_num], F_GETFL);
        int NEW_TEMP_FLAGS = FLAGS | O_NONBLOCK;
        fcntl(Afds[worker_num], F_SETFL, NEW_TEMP_FLAGS);    // make the following read O_NONBLOCK
        char *trash = new char[PIPE_BUF];
        ssize_t bytes_read;
        bytes_read = read(Afds[worker_num], trash, PIPE_BUF);    // if pipe is empty we will get EAGAIN and fail because a blocking read would block
        if ( bytes_read < 0 && errno != EAGAIN){                 // but that is ok so do not perror for EAGAIN that.
            perror("Warning: could not empty possible answer pipe contents?");
        }
        delete[] trash;
        fcntl(Afds[worker_num], F_SETFL, FLAGS);             // return to the old flags where reading was blocking
        close(Afds[worker_num]);
    }
    /* Now we can fork + exec the new worker */
    pid_t pid = fork();
    if ( pid < 0 ){
        perror("fork failed");
        worker_is_dead[worker_num] = true;
        Rfds[worker_num] = -1;  // will cause an error
        Afds[worker_num] = -1;  // ^^
        return false;
    } else if ( pid == 0 ){    // child process
        // Should I flush Rpipe here? I though I should but it broke stuff so instead
        // I made the worker not terminate in the middle of reading a request
        execl("./bin/worker", "worker", range_in_str, Rpipes[worker_num], Apipes[worker_num], NULL);
        /* Code continues to run only if exec fails: (most likely because the executable file could not be found) */
        perror("exec() failed");
        exit(-4);             // in this case the child process will exit with memory leaks but this is not supposed to happen normally anyway
    }   // else: (only parent process continues here)
    // update information on jobExecutor about the new worker
    workerNum_to_pid[worker_num] = pid;              // change this so it is correct
    // reopen previous name pipes (with blocking) to make sure that the worker is ready for communication
    while ( ( Rfds[worker_num] = open(Rpipes[worker_num], O_WRONLY) ) < 0 && errno == EINTR ) ;        // if interrupted try again
    if (Rfds[worker_num] < 0){
        perror("Could not re-open new worker's answer pipe");
        kill(SIGINT, pid);
        return false;
    }
    // find which subset of all the directories were allocated to the previous
    // worker_num-th worker and allocate them to his replacement (worker)
    int k = worker_num;
    while ( k < dirfilesize ){                       // each directory[k] used to belong to the previous worker
        Header header(INIT, strlen(directories[k]) + 1);
        if (dirfilesize - k - numWorkers <= 0) {
            header.type = END_OF_MESSAGES;           // type should be END_OF_MESSAGES if we are not going to send another message (aka assign another directory) to this worker
        }
        if ( write(Rfds[worker_num], &header, sizeof(Header)) < 0 ||
             write(Rfds[worker_num], directories[k], header.message_size) < 0 ){
            worker_is_dead[worker_num] = true;
            return false;
        }
        k += numWorkers;
    }
    // reopen previous name pipes (with blocking) to make sure that the worker is ready for communication
    while ( ( Afds[worker_num] = open(Apipes[worker_num], O_RDONLY) ) < 0 && errno == EINTR ) ;    // if interrupted try again
    if (Afds[worker_num] < 0){
        perror("Could not re-open new worker's answer pipe");
        kill(SIGINT, pid);
        return false;
    }
    return true;
}


/* Signal handlers' Implementation */
static void handle_child_termination(int sig) {              // called when one or more children get killed
    // if children have to die because jobExecutor will exit then this handler will not be called (see handle_sig_termination())
    pid_t pid;
    int status;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {      // while there is a child who was terminated and not "waited" for yet (aka for each terminated child)
        // then we need to replace this child with another
        int workerNum = -1;
        for (int i = 0 ; i < numWorkers ; i++){              // search the workerNum_to_pid table for our pid
            if ( workerNum_to_pid[i] == pid ){
                workerNum = i;
                break;
            }
        }
        if (workerNum == -1){
            write(2, "unknown pid exited\n", 20);            // write to stderr because I cannot call "printf" here
        } else{
            worker_is_dead[workerNum] = true;
        }
    }
    a_worker_died = true;
}


static void handle_sig_termination(int sig){                // called to terminate the children of this process when we get termination signal and tell jobExecutor to terminate asap
    struct sigaction act;
    memset(&act, 0, sizeof(act));
    act.sa_handler = SIG_IGN;
    sigaction(SIGCHLD, &act, NULL);                         // stop handling SIGCHLD as we do not want to replace any dead workers at this point
    jobExecutor_must_terminate = true;
}
