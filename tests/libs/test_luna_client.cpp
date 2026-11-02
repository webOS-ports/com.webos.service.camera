// Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
//
// SPDX-License-Identifier: Apache-2.0

// LunaClient::callSync() against a fake libluna. Replies arrive on their own
// thread, as they do for camera2's CameraHalProxy and luneos-faced's worker.

#include "luna_client.h"
#include <atomic>
#include <chrono>
#include <cstring>
#include <gtest/gtest.h>
#include <string>
#include <thread>
#include <unistd.h>

namespace
{
struct FakeBus
{
    bool setTimeoutWorks = true;
    int setTimeoutCalls  = 0;
    int cancelCalls      = 0;
    int payloadFd        = -1;
    int fdDelayMs        = 0;
    void *ctx{nullptr};
} bus;

// Published last by LSCallOneReply(), which is what orders the replier thread
// behind everything callSync() set up.
std::atomic<LSFilterFunc> pendingCallback{nullptr};

const char *const REPLY_PAYLOAD = "{\"returnValue\":true}";
LSMessage *const REPLY          = reinterpret_cast<LSMessage *>(0x1);
LSHandle *const HANDLE          = reinterpret_cast<LSHandle *>(0x2);

void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
} // namespace

extern "C"
{
    bool LSCallOneReply(LSHandle *, const char *, const char *, LSFilterFunc callback, void *ctx,
                        LSMessageToken *token, LSError *)
    {
        *token  = 1;
        bus.ctx = ctx;
        pendingCallback.store(callback);
        return true;
    }
    bool LSCallSetTimeout(LSHandle *, LSMessageToken, int, LSError *)
    {
        bus.setTimeoutCalls++;
        return bus.setTimeoutWorks;
    }
    bool LSCallCancel(LSHandle *, LSMessageToken, LSError *)
    {
        bus.cancelCalls++;
        return true;
    }
    bool LSMessageIsHubErrorMessage(LSMessage *) { return false; }
    const char *LSMessageGetPayload(LSMessage *) { return REPLY_PAYLOAD; }
    LSPayload *LSMessageAccessPayload(LSMessage *) { return nullptr; }
    int LSPayloadGetFd(const LSPayload *)
    {
        sleepMs(bus.fdDelayMs);
        return bus.payloadFd;
    }
    bool LSErrorInit(LSError *) { return true; }
    void LSErrorFree(LSError *) {}
    bool LSRegister(const char *, LSHandle **, LSError *) { return true; }
    bool LSUnregister(LSHandle *, LSError *) { return true; }
    bool LSGmainContextAttach(LSHandle *, GMainContext *, LSError *) { return true; }
    bool LSCall(LSHandle *, const char *, const char *, LSFilterFunc, void *, LSMessageToken *,
                LSError *)
    {
        return true;
    }
    bool LSCallFromApplication(LSHandle *, const char *, const char *, const char *, LSFilterFunc,
                               void *, LSMessageToken *, LSError *)
    {
        return true;
    }
    bool LSRegisterServerStatusEx(LSHandle *, const char *, LSServerStatusFunc, void *, void **,
                                  LSError *)
    {
        return true;
    }
    bool LSCancelServerStatus(LSHandle *, void *, LSError *) { return true; }
}

class CallSync : public ::testing::Test
{
protected:
    void SetUp() override
    {
        bus = FakeBus();
        pendingCallback.store(nullptr);
    }

    // Runs callSync() with a reply delivered replyDelayMs after the call was
    // sent. *fdOnReturn is what callSync() left in the fd at the moment it
    // returned, so a late write by the reply thread does not count.
    bool callWithReply(int replyDelayMs, std::string *result, int *fd = nullptr,
                       int *fdOnReturn = nullptr)
    {
        std::atomic<bool> stop{false};
        std::thread replier(
            [&]
            {
                LSFilterFunc callback = nullptr;
                while (!stop && !(callback = pendingCallback.load()))
                    sleepMs(1);
                if (stop)
                    return;
                sleepMs(replyDelayMs);
                callback(HANDLE, REPLY, bus.ctx);
            });
        const bool ok = client.callSync("luna://fake/close", "{}", result, 2000, fd);
        if (fdOnReturn)
            *fdOnReturn = *fd;
        stop = true;
        replier.join();
        return ok;
    }

    LunaClient client{HANDLE};
};

TEST_F(CallSync, AnOrdinaryReplyIsASuccess)
{
    std::string result;
    EXPECT_TRUE(callWithReply(5, &result));
    EXPECT_EQ(REPLY_PAYLOAD, result);
    EXPECT_EQ(0, bus.cancelCalls);
}

// With the reply on another thread, LSCallSetTimeout() races it. Losing that
// race deadlocks against libluna's reply path (luneos-faced hung for good) or
// fails because the call is gone (camera2 took that for a failed close() and
// kept the camera marked open). So the waiting side must never call it.
TEST_F(CallSync, WaitingOnAnotherThreadLeavesTheCallTimeoutAlone)
{
    std::string result;
    EXPECT_TRUE(callWithReply(5, &result));
    EXPECT_EQ(0, bus.setTimeoutCalls);
}

// On the default context the reply is dispatched by the wait itself, so there
// is nothing to race and libluna's timeout is what ends the wait.
TEST_F(CallSync, OnTheDefaultContextTheCallTimeoutIsSet)
{
    LunaClient onDefault("fake.service");
    std::string result;
    g_idle_add(
        +[](gpointer) -> gboolean
        {
            pendingCallback.load()(HANDLE, REPLY, bus.ctx);
            return G_SOURCE_REMOVE;
        },
        nullptr);
    EXPECT_TRUE(onDefault.callSync("luna://fake/close", "{}", &result, 2000));
    EXPECT_EQ(1, bus.setTimeoutCalls);
    EXPECT_EQ(REPLY_PAYLOAD, result);
}

TEST_F(CallSync, OnTheDefaultContextAFailedTimeoutCancelsTheCall)
{
    bus.setTimeoutWorks = false;
    LunaClient onDefault("fake.service");
    std::string result;
    EXPECT_FALSE(onDefault.callSync("luna://fake/close", "{}", &result, 2000));
    EXPECT_EQ(1, bus.cancelCalls);
}

TEST_F(CallSync, NoReplyTimesOutAndCancels)
{
    std::string result;
    EXPECT_FALSE(client.callSync("luna://fake/close", "{}", &result, 20));
    EXPECT_EQ(1, bus.cancelCalls);
}

// The fd is written after the payload: callSync() must not return before that.
TEST_F(CallSync, TheFdIsThereWhenTheCallReturns)
{
    int pipefd[2];
    ASSERT_EQ(0, pipe(pipefd));
    bus.payloadFd = pipefd[0];
    bus.fdDelayMs = 30;
    int fd        = -1;
    std::string result;
    int fdOnReturn = -1;
    EXPECT_TRUE(callWithReply(0, &result, &fd, &fdOnReturn));
    EXPECT_GE(fdOnReturn, 0);
    if (fd >= 0)
        close(fd);
    close(pipefd[0]);
    close(pipefd[1]);
}
