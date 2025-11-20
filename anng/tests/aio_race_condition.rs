/// Test to reproduce the race condition in aio.rs line 130 that triggers:
/// unreachable!("cancel guarantees that callback will be fired")
///
/// IMPORTANT: Per nng.h line 568-569:
/// "nng_aio_cancel attempts to cancel any in-progress I/O operation.
///  The AIO callback will still be executed..."
///
/// The race condition occurs due to tokio::sync::Notify semantics:
///
/// 1. Thread A: Operation starts, `notify.notified()` future is created and starts polling
/// 2. Thread B: Operation completes very quickly, callback fires → `notify.notify_one()` called
/// 3. Thread A: The notified() future receives the notification but gets CANCELLED
///    before returning from .await (e.g., due to tokio::select! or timeout)
/// 4. Thread A: CancellationGuard::drop runs, calls nng_aio_cancel + nng_aio_wait
/// 5. Thread B: Callback has already fired (guaranteed by nng)
/// 6. Thread A: Creates a NEW notified() future in CancellationGuard::drop (line 120)
/// 7. Thread A: Polls this new future - returns Pending because:
///    - notify_one() was already called and consumed by the first (now-cancelled) future
///    - The new future wasn't registered in time to receive that notification
///    - notify_one() only wakes ONE waiter, not all future waiters
/// 8. UNREACHABLE! The code assumes the notification must be ready
///
/// To maximize chances of hitting this race:
/// - Use many concurrent operations to increase contention
/// - Use inproc transport for minimal latency (operations complete very fast)
/// - Frequently cancel operations via tokio::time::timeout with very short timeouts
/// - Use tokio::select! to race operations against immediate cancellation
use anng::Message;
use std::{io::Write, time::Duration};

#[tokio::test(flavor = "multi_thread", worker_threads = 8)]
async fn trigger_aio_cancel_race() {
    tracing_subscriber::fmt()
        .with_test_writer()
        .with_max_level(tracing::Level::TRACE)
        .try_init()
        .ok();

    let rep0 = anng::protocols::reqrep0::Rep0::listen(c"inproc://aio_race_test")
        .await
        .unwrap();
    let req0 = anng::protocols::reqrep0::Req0::dial(c"inproc://aio_race_test")
        .await
        .unwrap();

    // Give sockets time to connect
    tokio::time::sleep(Duration::from_millis(10)).await;

    const ITERATIONS: usize = 10000;

    let responder = async {
        let mut rep0_ctx = rep0.context();
        for _ in 0..ITERATIONS {
            // Try to receive with aggressive cancellation
            let result = tokio::select! {
                biased;
                // Immediately ready branch to maximize cancellation likelihood
                _ = tokio::task::yield_now() => {
                    // Cancel the receive
                    continue;
                }
                result = rep0_ctx.receive() => {
                    result
                }
            };

            if let Ok((msg, reply)) = result {
                // Echo back immediately
                let _ = reply.reply(msg).await;
            }
        }
    };

    let requester = async {
        let mut req0_ctx = req0.context();
        for i in 0..u64::MAX {
            let mut msg = Message::with_capacity(4);
            let _ = msg.write(&(i as u32).to_be_bytes()).unwrap();

            // Try to send with aggressive cancellation via select!
            let send_result = tokio::select! {
                biased;
                // Immediately ready branch to maximize cancellation likelihood
                _ = tokio::task::yield_now() => {
                    // Cancel the send that might be in progress
                    continue;
                }
                result = req0_ctx.request(msg) => {
                    result
                }
            };

            if let Ok(reply_future) = send_result {
                // Now try to receive the reply with aggressive cancellation
                tokio::select! {
                    biased;
                    _ = tokio::task::yield_now() => {
                        // Cancel the receive
                    }
                    _ = reply_future => {
                        // Got reply
                    }
                }
            }
        }
    };

    // Run with a timeout
    let result = tokio::time::timeout(Duration::from_secs(30), async {
        tokio::join!(responder, requester);
    })
    .await;

    match result {
        Ok(_) => println!("Test completed without hitting the race condition"),
        Err(_) => println!("Test timed out"),
    }
}

/// Alternative test: rapid context dropping to force cancellations
#[tokio::test(flavor = "multi_thread", worker_threads = 8)]
async fn trigger_aio_cancel_race_via_drop() {
    tracing_subscriber::fmt()
        .with_test_writer()
        .with_max_level(tracing::Level::TRACE)
        .try_init()
        .ok();

    let rep0 = anng::protocols::reqrep0::Rep0::listen(c"inproc://aio_race_drop")
        .await
        .unwrap();
    let req0 = anng::protocols::reqrep0::Req0::dial(c"inproc://aio_race_drop")
        .await
        .unwrap();

    tokio::time::sleep(Duration::from_millis(10)).await;

    // Background responder
    let _responder = tokio::spawn({
        async move {
            loop {
                let mut ctx = rep0.context();
                if let Ok((msg, reply)) = ctx.receive().await {
                    let _ = reply.reply(msg).await;
                }
                tokio::task::yield_now().await;
            }
        }
    });

    // Just do it in a single task to avoid moving req0
    for i in 0..u64::MAX {
        // Create context, start operation, immediately drop context
        {
            let mut ctx = req0.context();
            let msg = Message::new();

            // Start request but don't await - drop the future immediately
            let request_fut = ctx.request(msg);
            drop(request_fut);
        } // Context dropped here while operation may be in flight

        // Yield to allow other tasks to run
        if i % 10 == 0 {
            tokio::task::yield_now().await;
        }
    }
}

/// Test using tokio::time::timeout with zero duration to force immediate cancellation
#[tokio::test(flavor = "multi_thread", worker_threads = 8)]
async fn trigger_aio_cancel_race_via_timeout() {
    tracing_subscriber::fmt()
        .with_test_writer()
        .with_max_level(tracing::Level::TRACE)
        .try_init()
        .ok();

    let rep0 = anng::protocols::reqrep0::Rep0::listen(c"inproc://aio_race_timeout")
        .await
        .unwrap();
    let req0 = anng::protocols::reqrep0::Req0::dial(c"inproc://aio_race_timeout")
        .await
        .unwrap();

    tokio::time::sleep(Duration::from_millis(10)).await;

    // Background responder
    let _responder = tokio::spawn({
        async move {
            loop {
                let mut ctx = rep0.context();
                if let Ok((msg, reply)) = ctx.receive().await {
                    let _ = reply.reply(msg).await;
                }
            }
        }
    });

    for i in 0..u64::MAX {
        let mut ctx = req0.context();
        let msg = Message::new();

        // Use timeout with zero duration to force immediate cancellation
        let _ = tokio::time::timeout(Duration::from_nanos(1), async {
            let reply = ctx.request(msg).await.unwrap();
            reply.await.unwrap();
        })
        .await;

        // Yield occasionally
        if i % 100 == 0 {
            tokio::task::yield_now().await;
        }
    }
}
