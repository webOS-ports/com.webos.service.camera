// @@@LICENSE
//
// Copyright (C) 2021, LG Electronics, All Right Reserved.
//
// No part of this source code may be communicated, distributed, reproduced
// or transmitted in any form or by any means, electronic or mechanical or
// otherwise, for any purpose, without the prior written permission of
// LG Electronics.
//
// LICENSE@@@

#define LOG_CONTEXT "libs"
#define LOG_TAG "LunaClient"
#include "luna_client.h"
#include "camera_utils_log.h"
#include <atomic>
#include <glib.h>
#include <ios>
#include <system_error>

struct AutoLSError : LSError
{
    AutoLSError(void)
    {
        try
        {
            LSErrorInit(this);
        }
        catch (const std::ios::failure &e)
        {
            PLOGE("Caught a std::ios::failure %s", e.what());
        }
        catch (std::bad_cast &e)
        {
            PLOGE("Caught a bad_cast %s", e.what());
        }
    }
    ~AutoLSError(void)
    {
        try
        {
            LSErrorFree(this);
        }
        catch (const std::system_error &e)
        {
            PLOGE("Caught a system_error with code %d meaning %s", e.code().value(), e.what());
        }
        catch (std::bad_cast &err)
        {
            PLOGE("Caught a bad_cast %s", err.what());
        }
    }
};

LunaClient::LunaClient(void)
{
    AutoLSError error = {};
    error.message     = nullptr;
    if (!LSRegister(nullptr, &pHandle_, &error))
    {
        PLOGE("LunaClient ERROR: %s\n", error.message);
    }

    if (!LSGmainContextAttach(pHandle_, g_main_context_default(), &error))
    {
        PLOGE("LunaClient ERROR: %s\n", error.message);
    }
}

LunaClient::LunaClient(LSHandle *handle)
{
    pHandle_       = handle;
    needUnregister = false;
}

LunaClient::LunaClient(const char *serviceName, GMainContext *ctx)
{
    AutoLSError error = {};
    error.message     = nullptr;
    if (!LSRegister(serviceName, &pHandle_, &error))
    {
        PLOGE("LunaClient ERROR: %s\n", error.message);
    }

    if (ctx == nullptr)
        pContext_ = g_main_context_default();
    else
        pContext_ = ctx;

    if (!LSGmainContextAttach(pHandle_, pContext_, &error))
    {
        PLOGE("LunaClient ERROR: %s\n", error.message);
    }
}

LunaClient::~LunaClient(void)
{
    try
    {
        if (pHandle_ != nullptr)
        {
            // Cancel live subscriptions so their callbacks can no longer reference
            // the HandlerWrapper objects owned by handlers_.
            for (auto &handler : handlers_)
            {
                AutoLSError error = {};
                if (!LSCallCancel(pHandle_, handler.first, &error))
                {
                    PLOGE("LunaClient ERROR: LSCallCancel failed for token %ld", handler.first);
                }
            }

            // Cancel server status registrations so their callbacks can no longer
            // reference the RegisterHandlerWrapper objects owned by registerHandlers_.
            for (auto &registerHandler : registerHandlers_)
            {
                if (registerHandler.second && registerHandler.second->cookie)
                {
                    AutoLSError error = {};
                    if (!LSCancelServerStatus(pHandle_, registerHandler.second->cookie, &error))
                    {
                        PLOGE("LunaClient ERROR: LSCancelServerStatus failed for %s",
                              registerHandler.first.c_str());
                    }
                }
            }

            if (needUnregister)
            {
                AutoLSError error = {};
                LSUnregister(pHandle_, &error);
            }
        }
    }
    catch (const std::exception &e)
    {
        return;
    }
}

bool LunaClient::callSync(const char *uri, const char *param, std::string *result, int timeout,
                          int *fd)
{
    AutoLSError error  = {};
    error.message      = nullptr;
    bool ret           = false;
    LSMessageToken tok = 0;

    struct Ctx
    {
        Ctx(std::string *pstrResult, int *fd) : pstrResult_(pstrResult), pFd_(fd) {}
        std::atomic<bool> bRet_{false};
        std::string *pstrResult_{nullptr};
        std::atomic<bool> bDone_{false};
        int *pFd_{nullptr};
    } ctx(result, fd);

    PLOGD("[%p] uri=%s, param=%s, timeout=%d", g_thread_self(), uri, param, timeout);
    ret = LSCallOneReply(
        pHandle_, uri, param,
        +[](LSHandle *h, LSMessage *m, void *d)
        {
            Ctx *pCtx = static_cast<Ctx *>(d);
            // 1. Check whether error with including time out.
            if (!LSMessageIsHubErrorMessage(m))
                pCtx->bRet_ = true;
            // 2. Processing message
            const auto *payload = LSMessageGetPayload(m);
            if (payload)
                pCtx->pstrResult_->assign(payload);
            // 3. Notify
            pCtx->bDone_ = true;
            // 4. getFd
            if (pCtx->pFd_)
            {
                int fd = LSPayloadGetFd(LSMessageAccessPayload(m));
                if (fd >= 0)
                {
                    *pCtx->pFd_ = dup(fd);
                    PLOGI("fd(%d) dup(%d)", fd, *pCtx->pFd_);
                }
                else
                {
                    PLOGE("invalid fd(%d)", fd);
                }
            }
            PLOGD("[%p] reply\n", g_thread_self());
            return pCtx->bRet_.load();
        },
        &ctx, &tok, &error);

    if (ret != true)
    {
        PLOGE("[%p] LunaClient ERROR: %s\n", g_thread_self(), error.message);
    }

    if (ret == true)
    {
        ret = LSCallSetTimeout(pHandle_, tok, timeout, nullptr);
        if (ret != true)
        {
            // The call is still pending but will never be waited on below.
            // Cancel it so the reply callback cannot touch the stack context
            // after this function returns.
            PLOGE("[%p] LunaClient ERROR: LSCallSetTimeout failed", g_thread_self());
            AutoLSError cancelError = {};
            LSCallCancel(pHandle_, tok, &cancelError);
        }
    }

    if (ret == true)
    {
        if (pContext_ == g_main_context_default())
        {
            while (!ctx.bDone_)
            {
                // Block until a source is dispatched; LSCallSetTimeout above
                // guarantees the reply callback fires eventually.
                g_main_context_iteration(pContext_, TRUE);
            }
        }
        else
        {
            int cnt = 0;
            while (!ctx.bDone_ && cnt < timeout)
            {
                g_usleep(1000);
                cnt++;
            }
            if (!ctx.bDone_)
            {
                // Timed out waiting; cancel the pending call so the reply
                // callback cannot touch the stack context after this
                // function returns.
                PLOGE("[%p] LunaClient ERROR: callSync timed out, token %ld", g_thread_self(), tok);
                AutoLSError cancelError = {};
                LSCallCancel(pHandle_, tok, &cancelError);
            }
        }
    }

    PLOGD("[%p] ret=%d, bRet_=%d", g_thread_self(), ret, ctx.bRet_.load());
    return ret && ctx.bRet_;
}

bool LunaClient::callAsync(const char *uri, const char *param, Handler handler, void *data)
{
    AutoLSError error       = {};
    error.message           = nullptr;
    bool ret                = false;
    HandlerWrapper *wrapper = new HandlerWrapper;
    wrapper->callback       = handler;
    wrapper->data           = data;

    PLOGD("uri=%s, param=%s", uri, param);
    ret = LSCallOneReply(
        pHandle_, uri, param,
        +[](LSHandle *h, LSMessage *m, void *d)
        {
            HandlerWrapper *wrapper = (HandlerWrapper *)d;
            const char *payload     = LSMessageGetPayload(m);
            if (payload)
                wrapper->callback(payload, wrapper->data);
            delete wrapper;
            return true;
        },
        (void *)wrapper, NULL, &error);

    if (!ret)
    {
        PLOGE("LunaClient ERROR: %s\n", error.message);
        delete wrapper;
    }

    PLOGD("ret=%d", ret);
    return ret;
}

bool LunaClient::registerToService(const char *serviceName, RegisterHandler handler, void *data)
{
    AutoLSError error               = {};
    error.message                   = nullptr;
    bool ret                        = false;
    RegisterHandlerWrapper *wrapper = new RegisterHandlerWrapper;
    wrapper->callback               = handler;
    wrapper->data                   = data;

    PLOGD("serviceName=%s", serviceName);

    // If a registration for this service already exists, cancel it before
    // replacing its wrapper so the old callback can no longer be invoked.
    auto it = registerHandlers_.find(serviceName);
    if (it != registerHandlers_.end())
    {
        if (it->second && it->second->cookie)
        {
            AutoLSError cancelError = {};
            if (!LSCancelServerStatus(pHandle_, it->second->cookie, &cancelError))
            {
                PLOGE("LunaClient ERROR: LSCancelServerStatus failed for %s", serviceName);
            }
        }
        registerHandlers_.erase(it);
    }

    ret = LSRegisterServerStatusEx(
        pHandle_, serviceName,
        +[](LSHandle *h, const char *s, bool b, void *d)
        {
            RegisterHandlerWrapper *wrapper = (RegisterHandlerWrapper *)d;
            wrapper->callback(s, b, wrapper->data);
            return true;
        },
        (void *)wrapper, &wrapper->cookie, &error);

    if (!ret)
    {
        PLOGE("LunaClient ERROR: %s\n", error.message);
        delete wrapper;
    }
    else
    {
        registerHandlers_[serviceName] = std::unique_ptr<RegisterHandlerWrapper>(wrapper);
    }

    PLOGD("ret=%d", ret);
    return ret;
}

bool LunaClient::subscribe(const char *uri, const char *param, unsigned long *subscribeKey,
                           Handler handler, void *data, const char *appId)
{
    AutoLSError error       = {};
    error.message           = nullptr;
    bool ret                = false;
    HandlerWrapper *wrapper = new HandlerWrapper;
    wrapper->callback       = handler;
    wrapper->data           = data;

    PLOGD("uri=%s, param=%s, appId=%s", uri, param, appId);
    auto cb = +[](LSHandle *h, LSMessage *m, void *d)
    {
        const char *payload = LSMessageGetPayload(m);
        if (!payload)
            return true;
        HandlerWrapper *wrapper = (HandlerWrapper *)d;
        wrapper->callback(payload, wrapper->data);
        return true;
    };

    if (appId)
    {
        ret = LSCallFromApplication(pHandle_, uri, param, appId, cb, (void *)wrapper, subscribeKey,
                                    &error);
    }
    else
    {
        ret = LSCall(pHandle_, uri, param, cb, (void *)wrapper, subscribeKey, &error);
    }

    if (!ret)
    {
        PLOGE("LunaClient ERROR: %s\n", error.message);
        delete wrapper;
        return false;
    }

    handlers_[*subscribeKey] = std::unique_ptr<HandlerWrapper>(wrapper);

    PLOGD("ret=%d, subscribeKey=%ld", ret, *subscribeKey);
    return ret;
}

bool LunaClient::unsubscribe(unsigned long subscribeKey)
{
    AutoLSError error = {};

    PLOGD("subscribeKey=%ld", subscribeKey);
    if (!LSCallCancel(pHandle_, subscribeKey, &error))
    {
        PLOGE("LunaClient ERROR: subscribeKey = %ld", subscribeKey);
        handlers_.erase(subscribeKey);
        return false;
    }

    handlers_.erase(subscribeKey);
    PLOGD("ret=%d", true);
    return true;
}

LSHandle *LunaClient::get() { return pHandle_; }
