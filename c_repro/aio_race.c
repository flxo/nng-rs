// Reproduction of aio race condition test from anng/tests/aio_race_condition.rs
// This mimics the trigger_aio_cancel_race_via_timeout test
//
// The test creates req/rep sockets, and rapidly:
// 1. Sends a request (waits for completion)
// 2. Starts receiving the reply
// 3. Immediately cancels the receive
// 4. Waits for the cancellation to complete
// 5. Checks that the callback was fired
//
// Build: nix-shell -p nng clang mbedtls --run "clang -O3 -flto -pthread -o aio_race aio_race.c -lnng -lmbedtls -lmbedx509 -lmbedcrypto"
//
// Progress: 960001 iterations, 960001 cancels, 4 callbacks not fired, 0 busy after cancel
// Progress: 970001 iterations, 970001 cancels, 4 callbacks not fired, 0 busy after cancel
// Progress: 980001 iterations, 980001 cancels, 4 callbacks not fired, 0 busy after cancel
// Progress: 990001 iterations, 990001 cancels, 4 callbacks not fired, 0 busy after cancel
// Progress: 1000001 iterations, 1000001 cancels, 4 callbacks not fired, 0 busy after cancel
// Progress: 1010001 iterations, 1010001 cancels, 4 callbacks not fired, 0 busy after cancel
// Progress: 1020001 iterations, 1020001 cancels, 4 callbacks not fired, 0 busy after cancel
// POTENTIAL ISSUE: Callback not fired after cancel! iteration=1020617, result=0 (Hunky dory)
// Progress: 1030001 iterations, 1030001 cancels, 5 callbacks not fired, 0 busy after cancel
// Progress: 1040001 iterations, 1040001 cancels, 5 callbacks not fired, 0 busy after cancel
// POTENTIAL ISSUE: Callback not fired after cancel! iteration=1046301, result=0 (Hunky dory)
// Progress: 1050001 iterations, 1050001 cancels, 6 callbacks not fired, 0 busy after cancel
// Progress: 1060001 iterations, 1060001 cancels, 6 callbacks not fired, 0 busy after cancel
// Progress: 1070001 iterations, 1070001 cancels, 6 callbacks not fired, 0 busy after cancel
// Progress: 1080001 iterations, 1080001 cancels, 6 callbacks not fired, 0 busy after cancel
// Progress: 1090001 iterations, 1090001 cancels, 6 callbacks not fired, 0 busy after cancel
// Progress: 1100001 iterations, 1100001 cancels, 6 callbacks not fired, 0 busy after cancel
// Progress: 1110001 iterations, 1110001 cancels, 6 callbacks not fired, 0 busy after cancel
// POTENTIAL ISSUE: Callback not fired after cancel! iteration=1112786, result=0 (Hunky dory)
// Progress: 1120001 iterations, 1120001 cancels, 7 callbacks not fired, 0 busy after cancel
// Progress: 1130001 iterations, 1130001 cancels, 7 callbacks not fired, 0 busy after cancel
// Progress: 1140001 iterations, 1140001 cancels, 7 callbacks not fired, 0 busy after cancel
// Progress: 1150001 iterations, 1150001 cancels, 7 callbacks not fired, 0 busy after cancel
// Progress: 1160001 iterations, 1160001 cancels, 7 callbacks not fired, 0 busy after cancel

#include <nng/nng.h>
#include <nng/protocol/reqrep0/rep.h>
#include <nng/protocol/reqrep0/req.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define URL "inproc://aio_race_timeout"

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
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    atomic_bool signaled;
} callback_state_t;

static void aio_callback(void *arg)
{
    callback_state_t *state = (callback_state_t *)arg;
    atomic_fetch_add(&state->callback_count, 1);

    pthread_mutex_lock(&state->mutex);
    atomic_store(&state->signaled, true);
    pthread_cond_signal(&state->cond);
    pthread_mutex_unlock(&state->mutex);
}

static void reset_callback_state(callback_state_t *state)
{
    atomic_store(&state->signaled, false);
}

// Responder thread - echoes messages back
static atomic_bool responder_running = true;

static void *responder_thread(void *arg)
{
    nng_socket *rep_sock = (nng_socket *)arg;
    nng_ctx ctx;
    nng_aio *aio;
    callback_state_t state = {
        .callback_count = 0,
        .mutex = PTHREAD_MUTEX_INITIALIZER,
        .cond = PTHREAD_COND_INITIALIZER,
        .signaled = false,
    };

    nng_ctx_open(&ctx, *rep_sock);
    nng_aio_alloc(&aio, aio_callback, &state);

    while (atomic_load(&responder_running))
    {
        // Receive
        reset_callback_state(&state);
        nng_ctx_recv(ctx, aio);
        nng_aio_wait(aio);

        int result = nng_aio_result(aio);
        if (result != 0)
        {
            continue;
        }

        nng_msg *msg = nng_aio_get_msg(aio);
        if (msg == NULL)
        {
            continue;
        }

        // Send reply (echo back)
        reset_callback_state(&state);
        nng_aio_set_msg(aio, msg);
        nng_ctx_send(ctx, aio);
        nng_aio_wait(aio);
    }

    nng_aio_free(aio);
    nng_ctx_close(ctx);
    return NULL;
}

int main(int argc, char **argv)
{
    nng_socket req_sock, rep_sock;
    int rv;

    printf("NNG AIO Race Condition Reproduction Test\n");
    printf("=========================================\n\n");

    // Open sockets
    nng_rep0_open(&rep_sock);
    nng_req0_open(&req_sock);
    nng_listen(rep_sock, URL, NULL, 0);
    nng_dial(req_sock, URL, NULL, 0);

    // Give sockets time to connect
    usleep(10000); // 10ms

    // Start responder thread
    pthread_t responder;
    pthread_create(&responder, NULL, responder_thread, &rep_sock);

    // Requester loop - this is where the race condition manifests
    nng_ctx req_ctx;
    nng_aio *req_aio;
    callback_state_t req_state = {
        .callback_count = 0,
        .mutex = PTHREAD_MUTEX_INITIALIZER,
        .cond = PTHREAD_COND_INITIALIZER,
        .signaled = false,
    };

    nng_ctx_open(&req_ctx, req_sock);
    nng_aio_alloc(&req_aio, aio_callback, &req_state);

    unsigned long iterations = 0;
    unsigned long cancel_count = 0;
    unsigned long callback_not_fired = 0;
    unsigned long busy_after_cancel = 0;

    // Seed random number generator
    srand((unsigned int)time(NULL));

    printf("Running rapid cancel test with random sleeps (0-1000ns)...\n");
    printf("Press Ctrl+C to stop\n\n");

    // Main loop - mimics trigger_aio_cancel_race_via_timeout
    for (unsigned long i = 0; i < 100000000; i++)
    {
        iterations++;

        // Create message
        nng_msg *msg;
        nng_msg_alloc(&msg, 0);
        nng_msg_append_u32(msg, (uint32_t)i);

        // Reset callback state for the send
        reset_callback_state(&req_state);

        // Step 1: Send the request and wait for it to complete
        nng_aio_set_msg(req_aio, msg);
        nng_ctx_send(req_ctx, req_aio);
        nng_aio_wait(req_aio);

        int send_result = nng_aio_result(req_aio);
        if (send_result != 0)
        {
            // Send failed, free message and continue
            msg = nng_aio_get_msg(req_aio);
            if (msg != NULL)
            {
                nng_msg_free(msg);
            }
            nng_aio_set_msg(req_aio, NULL);
            continue;
        }

        // Step 2: Reset callback state for the receive
        reset_callback_state(&req_state);
        int callback_before = atomic_load(&req_state.callback_count);

        // Step 3: Start receiving the reply
        nng_ctx_recv(req_ctx, req_aio);

        random_sleep_ns(); // Random delay before cancel - key race window!

        // Step 4: Immediately cancel the receive
        nng_aio_cancel(req_aio);
        cancel_count++;

        // Step 5: Wait for cancellation to complete
        nng_aio_wait(req_aio);

        // Check if AIO is still busy (should never be after wait)
        if (nng_aio_busy(req_aio))
        {
            busy_after_cancel++;
            printf("ISSUE: busy after cancel+wait! iteration=%lu\n", i);
        }

        // Check result
        int result = nng_aio_result(req_aio);

        // Check if callback was fired
        // Per nng.h: "nng_aio_cancel attempts to cancel any in-progress I/O operation.
        //            The AIO callback will still be executed..."
        int callback_after = atomic_load(&req_state.callback_count);
        bool callback_fired = (callback_after > callback_before);

        if (!callback_fired)
        {
            callback_not_fired++;
            printf("POTENTIAL ISSUE: Callback not fired after cancel! "
                   "iteration=%lu, result=%d (%s)\n",
                   i, result, nng_strerror(result));
        }

        // Handle message cleanup if we got a reply
        if (result == 0)
        {
            msg = nng_aio_get_msg(req_aio);
            if (msg != NULL)
            {
                nng_msg_free(msg);
            }
        }

        nng_aio_set_msg(req_aio, NULL);

        // Progress indicator
        if (i % 10000 == 0)
        {
            printf("Progress: %lu iterations, %lu cancels, %lu callbacks not fired, %lu busy after cancel\n",
                   iterations, cancel_count, callback_not_fired, busy_after_cancel);
        }
    }

    printf("\n=========================================\n");
    printf("Test completed!\n");
    printf("Total iterations: %lu\n", iterations);
    printf("Total cancels: %lu\n", cancel_count);
    printf("Callbacks not fired: %lu\n", callback_not_fired);
    printf("Busy after cancel: %lu\n", busy_after_cancel);

    // Cleanup
    atomic_store(&responder_running, false);

    nng_aio_free(req_aio);
    nng_ctx_close(req_ctx);

    // Force responder to wake up
    nng_close(rep_sock);
    pthread_join(responder, NULL);

    nng_close(req_sock);

    return 0;
}
