#ifndef CRAWL_H
#define CRAWL_H


struct args{
    const struct sockaddr_in *server_sa;
    int num_of_threads;
    args(struct sockaddr_in *param, int threads_num) : server_sa(param), num_of_threads(threads_num) {}
};


void *crawl(void *arguement);

#endif //CRAWL_H
