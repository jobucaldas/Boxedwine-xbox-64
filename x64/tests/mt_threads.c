/* x86-64 guest test: pthreads (clone3/clone, futex, TLS), fork/wait, execve. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static long counter;
static __thread int tlsValue;

static void *worker(void *arg) {
    long id = (long)arg;
    tlsValue = (int)id * 100;
    for (int i = 0; i < 100000; i++) {
        pthread_mutex_lock(&lock);
        counter++;
        pthread_mutex_unlock(&lock);
    }
    return (void *)(long)(tlsValue + 1);
}

int main(int argc, char **argv) {
    pthread_t t[4];
    for (long i = 0; i < 4; i++) pthread_create(&t[i], NULL, worker, (void *)i);
    long sum = 0;
    for (int i = 0; i < 4; i++) {
        void *r;
        pthread_join(t[i], &r);
        sum += (long)r;
    }
    printf("threads: counter=%ld tls-sum=%ld\n", counter, sum);
    fflush(stdout);

    pid_t pid = fork();
    if (pid == 0) {
        printf("fork: child running\n");
        fflush(stdout);
        _exit(42);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    printf("fork: child exit status=%d\n", WEXITSTATUS(status));
    fflush(stdout);

    if (argc > 1) {
        pid = fork();
        if (pid == 0) {
            execv(argv[1], &argv[1]);
            perror("execv");
            _exit(1);
        }
        waitpid(pid, &status, 0);
        printf("exec %s: status=%d\n", argv[1], WEXITSTATUS(status));
    }
    return 0;
}
