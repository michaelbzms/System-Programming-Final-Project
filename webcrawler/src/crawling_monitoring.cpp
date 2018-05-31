#include <iostream>
#include <pthread.h>
#include <cstdio>
#include <unistd.h>
#include <cstring>
#include <cstdlib>
#include <csignal>
#include <sys/ioctl.h>
#include "../headers/FIFO_Queue.h"
#include "../headers/crawling_monitoring.h"
#include "../headers/str_history.h"


using namespace std;


#define JOBEXECUTOR_PATH "./jobExecutor/bin/jobExecutor"


/* useful macros */
#define CHECK_PERROR(call, callname, handle_code) { if ( ( call ) < 0 ) { perror(callname); handle_code } }
#define CHECK(call, callname, handle_code) { if ( ( call ) < 0 ) { cerr << (callname) << " failed" << endl; handle_code } }


/* Global Variables */
extern bool crawling_has_finished;
extern pthread_cond_t crawlingFinished;
extern pthread_mutex_t crawlingFinishedLock;
extern int num_of_threads_blocked;
extern char *save_dir;
extern FIFO_Queue *urlQueue;
extern bool threads_must_terminate;
extern pthread_cond_t QueueIsEmpty;
extern bool monitor_forced_exit;
extern bool jobExecutorReadyForCommands;
extern pid_t jobExecutor_pid;
extern str_history *alldirs;
int toJobExecutor_pipe = -1, fromJobExecutor_pipe = -1;


/* Local Functions */
void init_jobExecutor(int numOfWorkers, const char *docfile);


void *monitor_crawling(void *args){
    struct monitor_args *arguements = (struct monitor_args *) args;

    // make sure to ignore SIGPIPE
    struct sigaction act;
    memset(&act, 0, sizeof(act));
    act.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &act, NULL);

    // Step1: block on cond_wait until crawling has finished, signaled every time it's possible for crawling to have finished
    CHECK( pthread_mutex_lock(&crawlingFinishedLock) , "pthread_mutex_lock", )        // lock crawling_has_finished's mutex
    // webcrawling has finished when:
    // 1) urlQueue is empty, and
    // 2) all threads are blocked on cond_wait, aka num_of_threads_blocked == num_of_threads
    // This is computed in crawl.cpp by those threads and if the last one finds out it's true, it will signal us
    while (!crawling_has_finished){
        CHECK( pthread_cond_wait(&crawlingFinished, &crawlingFinishedLock) , "pthread_cond_wait on crawling finish", )
        if ( monitor_forced_exit ) break;          // if forced to exit before crawling has finished
    }
    CHECK( pthread_mutex_unlock(&crawlingFinishedLock) , "pthread_mutex_unlock", )    // unlock crawling_has_finished's mutex

    cout << ((monitor_forced_exit) ? "Web crawling did not finish in time but forced to shutdown..." : "Web crawling finished!") << endl;

    // Step2: join with the num_of_threads threads, whose job is either finished or forced to finish via early SHUTDOWN command
    threads_must_terminate = true;
    urlQueue->acquire();                           // (!) locking urlQueue's lock before the broadcast is important to avoid a thread missing it!
    CHECK( pthread_cond_broadcast(&QueueIsEmpty), "pthread_cond_broadcast",  )        // broadcast all threads so they can get unstuck from cond_wait and terminate
    urlQueue->release();
    void *status;
    for (int i = 0; i < arguements->num_of_threads; i++) {
        CHECK(pthread_join(arguements->threadpool[i], &status), "pthread_join in monitor_crawling", )
        if (status != NULL) { cerr << "thread terminated with an unexpected status" << endl; }
    }

    // Step3: initiate the jobExecutor, but only if there are directories for him to index
    if (!monitor_forced_exit && alldirs->get_size() > 0){                             // web crawling has finished here so get_size() is "atomic"
        cout << "Initiating jobExecutor..." << endl;
        init_jobExecutor(arguements->num_of_workers, save_dir);
        cout << "jobExecutor ready for commands" << endl;
        jobExecutorReadyForCommands = true;
    }

    return NULL;
}


void init_jobExecutor(int numOfWorkers, const char *docfile){        // called by monitor thead when it's time to initiate the jobExecutor
    int crawler_to_jobExectutor_pipe[2];
    int jobExectutor_to_crawler_pipe[2];
    CHECK_PERROR( pipe(crawler_to_jobExectutor_pipe) , "pipe creation", return; )
    CHECK_PERROR( pipe(jobExectutor_to_crawler_pipe), "pipe creation", return; )
    char numWorkers_str[64];
    sprintf(numWorkers_str, "%d", numOfWorkers);
    pid_t pid;
    CHECK_PERROR( (pid = fork()), "fork jobExecutor", cerr << "Warning: jobExecutor could not be initialized" << endl; return; )
    if ( pid == 0 ){       // child
        dup2(crawler_to_jobExectutor_pipe[0], STDIN_FILENO);        // make stdin the 1st pipe's read end for the about-to-be-execed process
        close(crawler_to_jobExectutor_pipe[0]);
        close(crawler_to_jobExectutor_pipe[1]);
        dup2(jobExectutor_to_crawler_pipe[1], STDOUT_FILENO);       // make stdoit the 2nd pipe's write end for the about-to-be-execed process
        close(jobExectutor_to_crawler_pipe[0]);
        close(jobExectutor_to_crawler_pipe[1]);
        execl(JOBEXECUTOR_PATH, "jobExecutor", numWorkers_str, NULL);
        /* Code continues to run only if exec fails: (most likely because the executable file could not be found) */
        perror("exec() failed");
        exit(-1);          // this exit may have leaks, but we do not want the fork process to continue as is (so make sure exec's path is correct)
    } else {               // parent
        jobExecutor_pid = pid;
        toJobExecutor_pipe = dup(crawler_to_jobExectutor_pipe[1]);
        close(crawler_to_jobExectutor_pipe[0]);
        close(crawler_to_jobExectutor_pipe[1]);
        fromJobExecutor_pipe = dup(jobExectutor_to_crawler_pipe[0]);
        close(jobExectutor_to_crawler_pipe[0]);
        close(jobExectutor_to_crawler_pipe[1]);
        // send jobExecutor terminal width (only possible at start)
        struct winsize wsize;
        CHECK_PERROR(ioctl(STDOUT_FILENO, TIOCGWINSZ, &wsize) , "ioctl for getting terminal width failed", wsize.ws_col = 120; /*default value*/ )
        int term_width = wsize.ws_col;
        CHECK_PERROR(write(toJobExecutor_pipe, &term_width, sizeof(int)), "write to jobExecutor", )
        // send jobExecutor the directories he ll be responsible for
        cout << "Initiating jobExecutor with " << alldirs->get_size() << " site directories..." << endl;    // web crawling has finished here so get_size() is "atomic"
        char **directories = alldirs->get_all_strings_as_table();
        int dirsize = alldirs->get_size();
        CHECK_PERROR(write(toJobExecutor_pipe, &dirsize, sizeof(int)), "write to jobExecutor", )
        for (int i = 0 ; i < dirsize ; i++){
            cout << "- " << directories[i] << endl;
            int this_dir_len = (int) strlen(directories[i]);
            CHECK_PERROR(write(toJobExecutor_pipe, &this_dir_len, sizeof(int)), "write to jobExecutor", )
            CHECK_PERROR(write(toJobExecutor_pipe, directories[i], this_dir_len), "write to jobExecutor", )
            delete[] directories[i];
        }
        delete[] directories;
    }
}
