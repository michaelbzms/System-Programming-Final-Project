#ifndef CRAWLING_MONITORING_H
#define CRAWLING_MONITORING_H


struct monitor_args{
    int num_of_threads, num_of_workers;
    pthread_t *threadpool;
    char *save_dir;
};


void *monitor_crawling(void *args);        // monitor thread


#endif //CRAWLING_MONITORING_H
