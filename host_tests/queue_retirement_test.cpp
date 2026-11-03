#include "graphics/queue_retirement.h"
#include <cassert>
#include <sys/wait.h>
#include <thread>
#include <mutex>

using namespace winehua;
using namespace winehua::queue_retirement;

int main()
{
    { // Legacy consumers negotiate no channel.
        QueueRetirementProducer producer;
        assert(producer.Begin() == QueueRetirementResult::Ready);
        assert(producer.Complete());
    }
    { // Repeated resizes, delayed drains, and resumption on the same queue.
        QueueRetirementConsumer consumer;
        QueueRetirementProducer producer;
        assert(consumer.Create() && producer.SetFd(consumer.OfferedFd()));
        consumer.CloseOffer();
        int drained = 0;
        for (unsigned epoch = 0; epoch < 1000; ++epoch) {
            assert(producer.Begin() == QueueRetirementResult::Pending);
            assert(consumer.Service([] { return false; }));
            assert(producer.Begin() == QueueRetirementResult::Pending);
            assert(consumer.Service([&] { ++drained; return true; }));
            assert(producer.Begin() == QueueRetirementResult::Ready);
            assert(producer.Complete() && !producer.Pending());
            assert(!consumer.Service([] { assert(false); return false; }));
        }
        assert(drained == 1000);
    }
    { // A stale ack must not unlock another retirement generation.
        int pair[2]; assert(!socketpair(AF_UNIX, SOCK_SEQPACKET, 0, pair));
        QueueRetirementProducer producer; assert(producer.SetFd(pair[0])); close(pair[0]);
        assert(producer.Begin() == QueueRetirementResult::Pending);
        Message msg{}; assert(recv(pair[1], &msg, sizeof(msg), 0) == sizeof(msg));
        assert(Send(pair[1], Acknowledge, msg.epoch - 1));
        assert(producer.Begin() == QueueRetirementResult::Pending);
        assert(Send(pair[1], Acknowledge, msg.epoch));
        assert(producer.Begin() == QueueRetirementResult::Ready);
        assert(producer.Complete()); close(pair[1]);
    }
    { // Device teardown timeout is bounded; a late real drain can still recover.
        QueueRetirementConsumer consumer;
        QueueRetirementProducer producer;
        assert(consumer.Create() && producer.SetFd(consumer.OfferedFd())); consumer.CloseOffer();
        const auto start = NowMs();
        assert(!producer.Wait(20)); assert(NowMs() - start < 250);
        assert(consumer.Service([] { return true; }));
        assert(producer.Wait(20)); assert(producer.Complete());
        consumer.Close();
        assert(producer.Begin() == QueueRetirementResult::Ready);
        assert(producer.Complete());
    }
    { // Cross-process endpoint: acknowledgement must follow the actual drain.
        QueueRetirementConsumer consumer;
        assert(consumer.Create());
        const pid_t pid = fork(); assert(pid >= 0);
        if (!pid) {
            QueueRetirementProducer producer;
            assert(producer.SetFd(consumer.OfferedFd()));
            consumer.Close();
            for (int i = 0; i < 50; ++i) { assert(producer.Wait(200)); assert(producer.Complete()); }
            _exit(0);
        }
        consumer.CloseOffer();
        int status = 0, drains = 0;
        const auto deadline = NowMs() + 3000;
        while (waitpid(pid, &status, WNOHANG) == 0) {
            consumer.Service([&] { ++drains; return true; });
            assert(NowMs() < deadline);
            std::this_thread::yield();
        }
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 0 && drains == 50);
    }
}
