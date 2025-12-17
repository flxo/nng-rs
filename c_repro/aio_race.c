#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <nng/nng.h>
#include <nng/protocol/reqrep0/rep.h>
#include <nng/protocol/reqrep0/req.h>

#define URL "inproc://callback"

// Random sleep between 0-1000 nanoseconds
static inline void random_sleep_ns(void)
{
    struct timespec ts = {
        .tv_sec = 0,
        .tv_nsec = rand() % 1001,
    };
    nanosleep(&ts, NULL);
}

// Callback state tracking
typedef struct
{
    atomic_int callback_count;
} callback_state_t;

static void aio_callback(void *arg)
{
    callback_state_t *state = (callback_state_t *)arg;
    atomic_fetch_add(&state->callback_count, 1);
}

// just echo any received msg
static void *responder_thread(void *arg)
{
    nng_socket *rep_sock = (nng_socket *)arg;
    nng_msg *msg;
    int rv;

    while (true)
    {
        rv = nng_recvmsg(*rep_sock, &msg, 0);
        if (rv != 0)
        {
            continue;
        }

        rv = nng_sendmsg(*rep_sock, msg, 0);
        if (rv != 0)
        {
            nng_msg_free(msg);
        }
    }

    return NULL;
}

int main(int argc, char **argv)
{
    printf("NNG AIO callback reproduction\n");

    nng_socket req_sock, rep_sock;

    srand((unsigned int)time(NULL));

    // open sockets
    nng_rep0_open(&rep_sock);
    nng_req0_open(&req_sock);
    nng_listen(rep_sock, URL, NULL, 0);
    nng_dial(req_sock, URL, NULL, 0);

    // give sockets time to connect
    usleep(100000);

    // Start responder thread
    pthread_t responder;
    pthread_create(&responder, NULL, responder_thread, &rep_sock);

    // requester loop - this is where the race condition manifests
    nng_ctx req_ctx;
    nng_aio *req_aio;
    callback_state_t req_state = {
        .callback_count = 0,
    };

    nng_ctx_open(&req_ctx, req_sock);
    nng_aio_alloc(&req_aio, aio_callback, &req_state);

    unsigned long iteration = 0;
    unsigned long callback_not_fired = 0;

    for (;; iteration++)
    {

        // create message
        nng_msg *msg;
        nng_msg_alloc(&msg, 0);
        nng_msg_append_u32(msg, (uint32_t)iteration);

        // step 1: Send the request and wait for it to complete
        nng_aio_set_msg(req_aio, msg);
        nng_ctx_send(req_ctx, req_aio);
        nng_aio_wait(req_aio);

        int send_result = nng_aio_result(req_aio);
        if (send_result != 0)
        {
            msg = nng_aio_get_msg(req_aio);
            if (msg != NULL)
            {
                nng_msg_free(msg);
            }
            nng_aio_set_msg(req_aio, NULL);
            continue;
        }

        // step 2: Get the callback counter before receiving
        int callback_before = atomic_load(&req_state.callback_count);

        // Step 3: Start receiving the reply
        nng_ctx_recv(req_ctx, req_aio);

        random_sleep_ns(); // Random delay before cancel

        // Step 4: Immediately cancel the receive
        nng_aio_cancel(req_aio);

        // step 5: Wait for cancellation to complete
        nng_aio_wait(req_aio);

        // check if AIO is still busy (should never be after wait)
        assert(!nng_aio_busy(req_aio));

        // check result
        int result = nng_aio_result(req_aio);

        // check if callback was fired
        int callback_after = atomic_load(&req_state.callback_count);
        bool callback_fired = (callback_after > callback_before);

        if (!callback_fired)
        {
            callback_not_fired++;
            printf("POTENTIAL ISSUE: Callback not fired after cancel! "
                   "iteration=%lu, result=%d (%s)\n",
                   iteration, result, nng_strerror(result));
        }

        // handle message cleanup if we got a reply
        if (result == 0)
        {
            msg = nng_aio_get_msg(req_aio);
            if (msg != NULL)
            {
                nng_msg_free(msg);
            }
        }

        nng_aio_set_msg(req_aio, NULL);

        // just print some progress to see that we're still running
        if (iteration % 10000 == 0)
        {
            printf("Progress: %lu iterations, %lu callbacks not fired\n", iteration, callback_not_fired);
        }
    }

    // Cleanup
    nng_aio_free(req_aio);
    nng_ctx_close(req_ctx);
    nng_close(rep_sock);
    nng_close(req_sock);

    return 0;
}
