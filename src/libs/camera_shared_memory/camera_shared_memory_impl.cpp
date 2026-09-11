// Copyright (c) 2024 LG Electronics, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// SPDX-License-Identifier: Apache-2.0

#define LOG_CONTEXT "libs"
#define LOG_TAG "CameraSharedMemoryImpl"
#include "camera_shared_memory_impl.h"
#include "camera_utils_log.h"
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <limits.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace
{
// 7680 * 4320 * 2 : one packed 8K 16bpp frame, same ceiling the legacy SysV
// implementation used.
constexpr size_t kMaxDataSize     = 66355200;
constexpr size_t kMaxMetaSize     = 4 * 1024 * 1024;
constexpr size_t kMaxExtraSize    = 4 * 1024 * 1024;
constexpr size_t kMaxSolutionSize = 4 * 1024 * 1024;
constexpr size_t kMaxBufferCount  = 64;
} // namespace

CameraSharedMemoryImpl::CameraSharedMemoryImpl()
    : shmFd_(-1), shmAddr_(nullptr), shmSize_(0), shmHeader_(nullptr)
{
    PLOGI("");
}

CameraSharedMemoryImpl::~CameraSharedMemoryImpl()
{
    PLOGI("");
    releaseAllSignals();
    close();
}

bool CameraSharedMemoryImpl::validateGeometry(size_t dataSize, size_t metaSize, size_t extraSize,
                                              size_t solutionSize, size_t bufferCount,
                                              size_t *pSectionSize, size_t *pTotalSize)
{
    if (bufferCount == 0 || bufferCount > kMaxBufferCount || dataSize == 0 ||
        dataSize > kMaxDataSize || metaSize > kMaxMetaSize || extraSize > kMaxExtraSize ||
        solutionSize > kMaxSolutionSize)
    {
        PLOGE("geometry out of range: data(%zu) meta(%zu) extra(%zu) solution(%zu) count(%zu)",
              dataSize, metaSize, extraSize, solutionSize, bufferCount);
        return false;
    }

    // With the caps above none of this arithmetic can overflow size_t, but
    // keep it explicit so a cap change cannot silently reintroduce a wrap.
    size_t sectionSize = 0;
    size_t totalSize   = 0;
    if (__builtin_add_overflow(dataSize, metaSize, &sectionSize) ||
        __builtin_add_overflow(sectionSize, extraSize, &sectionSize) ||
        __builtin_add_overflow(sectionSize, solutionSize, &sectionSize) ||
        __builtin_add_overflow(sectionSize, sizeof(size_t) * 4, &sectionSize) ||
        __builtin_mul_overflow(sectionSize, bufferCount, &totalSize) ||
        __builtin_add_overflow(totalSize, sizeof(ShmHeader), &totalSize))
    {
        PLOGE("geometry overflow");
        return false;
    }

    if (pSectionSize)
        *pSectionSize = sectionSize;
    if (pTotalSize)
        *pTotalSize = totalSize;
    return true;
}

void CameraSharedMemoryImpl::resetLocked(void)
{
    shmHeader_ = nullptr;
    shmBuffers_.clear();
    bufferCount_ = dataSize_ = metaSize_ = extraSize_ = solutionSize_ = 0;
    if (shmAddr_)
    {
        munmap(shmAddr_, shmSize_);
        shmAddr_ = nullptr;
    }
    shmSize_ = 0;
    if (shmFd_ != -1)
    {
        ::close(shmFd_);
        shmFd_ = -1;
    }
    if (isCreated_ && !shmName_.empty())
    {
        shm_unlink(shmName_.c_str());
    }
    shmName_   = "";
    isCreated_ = false;
}

int CameraSharedMemoryImpl::create(const std::string &name, size_t dataSize, size_t metaSize,
                                   size_t extraSize, size_t solutionSize, size_t bufferCount)
{
    PLOGI("name(%s) data(%zu) meta(%zu) extra(%zu) solution(%zu) count(%zu)", name.c_str(),
          dataSize, metaSize, extraSize, solutionSize, bufferCount);

    std::lock_guard<std::mutex> lock(m_);

    size_t totalSize = 0;
    if (!validateGeometry(dataSize, metaSize, extraSize, solutionSize, bufferCount, nullptr,
                          &totalSize))
    {
        return -1;
    }

    shmFd_ =
        shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
    if (shmFd_ == -1)
    {
        PLOGE("shm_open failed : %s", name.c_str());
        return -1;
    }
    isCreated_ = true;
    shmName_   = name; // set before the fallible calls so resetLocked() can unlink

    shmSize_ = totalSize;
    PLOGI("headerSize(%zu) shmSize(%zu)", sizeof(ShmHeader), shmSize_);
    if (ftruncate(shmFd_, shmSize_) == -1)
    {
        PLOGE("ftruncate failed");
        resetLocked();
        return -1;
    }

    shmAddr_ = mmap(NULL, shmSize_, PROT_READ | PROT_WRITE, MAP_SHARED, shmFd_, 0);
    if (shmAddr_ == MAP_FAILED)
    {
        PLOGE("mmap failed");
        shmAddr_ = nullptr;
        resetLocked();
        return -1;
    }

    shmHeader_               = static_cast<ShmHeader *>(shmAddr_);
    shmHeader_->writeIndex   = -1;
    shmHeader_->bufferCount  = bufferCount;
    shmHeader_->dataSize     = dataSize;
    shmHeader_->metaSize     = metaSize;
    shmHeader_->extraSize    = extraSize;
    shmHeader_->solutionSize = solutionSize;

    bufferCount_  = bufferCount;
    dataSize_     = dataSize;
    metaSize_     = metaSize;
    extraSize_    = extraSize;
    solutionSize_ = solutionSize;

    initBuffers();
    for (auto &buffer : shmBuffers_)
    {
        *buffer.pDataSize     = 0;
        *buffer.pMetaSize     = 0;
        *buffer.pExtraSize    = 0;
        *buffer.pSolutionSize = 0;
    }

    PLOGI("fd(%d)", shmFd_);
    return shmFd_;
}

int CameraSharedMemoryImpl::open(const std::string &name)
{
    PLOGI("name(%s)", name.c_str());

    std::lock_guard<std::mutex> lock(m_);

    int fd = shm_open(name.c_str(), O_RDWR, 0666);
    if (fd == -1)
    {
        PLOGE("shm_open failed : %s", name.c_str());
        return -1;
    }

    if (!initShmem(fd))
    {
        PLOGE("initShmem Fail!");
        ::close(fd);
        return -1;
    }

    initBuffers();

    PLOGI("fd(%d)", fd);
    return fd;
}

bool CameraSharedMemoryImpl::open(int fd)
{
    PLOGI("fd(%d)", fd);

    std::lock_guard<std::mutex> lock(m_);

    if (!initShmem(fd))
    {
        PLOGE("initShmem Fail!");
        return false;
    }

    initBuffers();

    PLOGI("end");
    return true;
}

bool CameraSharedMemoryImpl::initShmem(int fd)
{
    PLOGI("fd(%d)", fd);

    struct stat sb;
    if (fstat(fd, &sb) == -1)
    {
        PLOGE("Failed to get size of shared memory");
        return false;
    }
    if (sb.st_size < 0 || static_cast<size_t>(sb.st_size) < sizeof(ShmHeader))
    {
        PLOGE("shared memory too small (%lld)", (long long)sb.st_size);
        return false;
    }
    size_t mappedSize = static_cast<size_t>(sb.st_size);
    PLOGI("shm size %zu", mappedSize);

    void *addr = mmap(0, mappedSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED)
    {
        PLOGE("shmem mmap fail!");
        return false;
    }

    // The header was written by the peer process. Validate every field before
    // it is used for pointer arithmetic, and keep validated copies: the peer
    // stays able to rewrite the mapped header afterwards, so it must never be
    // re-trusted (double-fetch).
    ShmHeader *header  = static_cast<ShmHeader *>(addr);
    size_t sectionSize = 0;
    size_t totalSize   = 0;
    if (!validateGeometry(header->dataSize, header->metaSize, header->extraSize,
                          header->solutionSize, header->bufferCount, &sectionSize, &totalSize) ||
        totalSize > mappedSize)
    {
        PLOGE("invalid shared memory header (need %zu, mapped %zu)", totalSize, mappedSize);
        munmap(addr, mappedSize);
        return false;
    }

    shmAddr_      = addr;
    shmSize_      = mappedSize;
    shmHeader_    = header;
    shmFd_        = fd;
    bufferCount_  = header->bufferCount;
    dataSize_     = header->dataSize;
    metaSize_     = header->metaSize;
    extraSize_    = header->extraSize;
    solutionSize_ = header->solutionSize;

    printShmHeader();
    return true;
}

void CameraSharedMemoryImpl::initBuffers(void)
{
    size_t headerSize      = sizeof(ShmHeader);
    size_t dataSectionSize = dataSize_ + metaSize_ + extraSize_ + solutionSize_ + sizeof(size_t) * 4;

    shmBuffers_.resize(bufferCount_);
    for (size_t i = 0; i < bufferCount_; ++i)
    {
        unsigned char *base =
            static_cast<unsigned char *>(shmAddr_) + headerSize + i * dataSectionSize;

        shmBuffers_[i].pDataSize = reinterpret_cast<size_t *>(base);
        shmBuffers_[i].pData =
            reinterpret_cast<unsigned char *>(shmBuffers_[i].pDataSize) + sizeof(size_t);
        shmBuffers_[i].pMetaSize = reinterpret_cast<size_t *>(shmBuffers_[i].pData + dataSize_);
        shmBuffers_[i].pMeta =
            reinterpret_cast<unsigned char *>(shmBuffers_[i].pMetaSize) + sizeof(size_t);
        shmBuffers_[i].pExtraSize = reinterpret_cast<size_t *>(shmBuffers_[i].pMeta + metaSize_);
        shmBuffers_[i].pExtra =
            reinterpret_cast<unsigned char *>(shmBuffers_[i].pExtraSize) + sizeof(size_t);
        shmBuffers_[i].pSolutionSize =
            reinterpret_cast<size_t *>(shmBuffers_[i].pExtra + extraSize_);
        shmBuffers_[i].pSolution =
            reinterpret_cast<unsigned char *>(shmBuffers_[i].pSolutionSize) + sizeof(size_t);
    }
}

void CameraSharedMemoryImpl::printShmHeader(void)
{
    PLOGI("bufferCount  : %zu", shmHeader_->bufferCount);
    PLOGI("writeIndex   : %d", shmHeader_->writeIndex);
    PLOGI("dataSize     : %zu", shmHeader_->dataSize);
    PLOGI("metaSize     : %zu", shmHeader_->metaSize);
    PLOGI("extraSize    : %zu", shmHeader_->extraSize);
    PLOGI("solutionSize : %zu", shmHeader_->solutionSize);
}

void CameraSharedMemoryImpl::close(void)
{
    PLOGI("start");

    std::lock_guard<std::mutex> lock(m_);
    resetLocked();

    PLOGI("end");
}

bool CameraSharedMemoryImpl::incrementWriteIndex(void)
{
    std::lock_guard<std::mutex> lock(m_);

    if (!shmHeader_ || shmBuffers_.empty())
    {
        PLOGE("shmHeader_ is NULL");
        return false;
    }

    int index = shmHeader_->writeIndex;
    if (index < 0 || index >= (int)shmBuffers_.size() - 1)
        index = 0;
    else
        index += 1;
    shmHeader_->writeIndex = index;

    PLOGD("writeIndex(%d)", shmHeader_->writeIndex);
    return true;
}

bool CameraSharedMemoryImpl::writeHeader(int index, size_t dataSize)
{
    PLOGD("index(%d) dataSize(%zu)", index, dataSize);

    std::lock_guard<std::mutex> lock(m_);

    if (!shmHeader_)
    {
        PLOGE("shmHeader_ is NULL");
        return false;
    }

    if (index < 0 || index >= (int)shmBuffers_.size())
    {
        PLOGE("index is out of range (%d)", index);
        return false;
    }

    if (dataSize > dataSize_)
    {
        PLOGE("dataSize(%zu) exceeds buffer capacity(%zu)", dataSize, dataSize_);
        return false;
    }

    shmHeader_->writeIndex        = index;
    *shmBuffers_[index].pDataSize = dataSize;

    return true;
}

bool CameraSharedMemoryImpl::getBufferList(std::vector<void *> *pDataList,
                                           std::vector<void *> *pMetaList,
                                           std::vector<void *> *pExtraList,
                                           std::vector<void *> *pSolutionList)
{
    PLOGI("");

    std::lock_guard<std::mutex> lock(m_);

    if (!shmHeader_)
    {
        PLOGE("shmHeader_ is NULL");
        return false;
    }

    for (auto &buffer : shmBuffers_)
    {
        if (pDataList)
            pDataList->push_back(reinterpret_cast<void *>(buffer.pData));
        if (pMetaList)
            pMetaList->push_back(reinterpret_cast<void *>(buffer.pMeta));
        if (pExtraList)
            pExtraList->push_back(reinterpret_cast<void *>(buffer.pExtra));
        if (pSolutionList)
            pSolutionList->push_back(reinterpret_cast<void *>(buffer.pSolution));
    }

    return true;
}

bool CameraSharedMemoryImpl::getBufferInfo(size_t *pBufferCount, size_t *pDataSize,
                                           size_t *pMetaSize, size_t *pExtraSize,
                                           size_t *pSolutionSize)
{
    PLOGI("");

    std::lock_guard<std::mutex> lock(m_);

    if (!shmHeader_)
    {
        PLOGE("shmHeader_ is NULL");
        return false;
    }

    if (pBufferCount)
        *pBufferCount = bufferCount_;
    if (pDataSize)
        *pDataSize = dataSize_;
    if (pMetaSize)
        *pMetaSize = metaSize_;
    if (pExtraSize)
        *pExtraSize = extraSize_;
    if (pSolutionSize)
        *pSolutionSize = solutionSize_;

    return true;
}

bool CameraSharedMemoryImpl::read(unsigned char **ppData, size_t *pDataSize, unsigned char **ppMeta,
                                  size_t *pMetaSize, unsigned char **ppExtra, size_t *pExtraSize,
                                  unsigned char **ppSolution, size_t *pSolutionSize, int timeoutMs,
                                  bool skipSignal)
{
    PLOGD("timeout %d ms", timeoutMs);

    if (!skipSignal)
    {
        if (!waitForSignal(timeoutMs))
        {
            PLOGE("waitForSignal() fail");
            return false;
        }
    }

    const int maxRetries = 100;
    for (int retry = 0; retry <= maxRetries; retry++)
    {
        if (readData(ppData, pDataSize, ppMeta, pMetaSize, ppExtra, pExtraSize, ppSolution,
                     pSolutionSize))
        {
            if (ppData && pDataSize)
                PLOGD("read done! data(%p) length(%zu)", *ppData, *pDataSize);
            return true;
        }

        usleep(10000);
        PLOGI("readData Fail! retry(%d/%d)", retry, maxRetries);
    }

    return false;
}

bool CameraSharedMemoryImpl::readData(unsigned char **ppData, size_t *pDataSize,
                                      unsigned char **ppMeta, size_t *pMetaSize,
                                      unsigned char **ppExtra, size_t *pExtraSize,
                                      unsigned char **ppSolution, size_t *pSolutionSize)
{
    std::lock_guard<std::mutex> lock(m_);

    if (!shmHeader_ || shmBuffers_.empty())
    {
        PLOGE("shmHeader_ is NULL");
        return false;
    }

    // writeIndex lives in the shared header: snapshot it once and bound it by
    // our own (peer-immutable) buffer vector, not by the shared bufferCount.
    int writeIndex = shmHeader_->writeIndex;
    if (writeIndex == -1)
    {
        PLOGE("No data has been written yet.");
        return false;
    }
    size_t count = shmBuffers_.size();
    if (writeIndex < 0 || static_cast<size_t>(writeIndex) >= count)
    {
        PLOGE("corrupt writeIndex(%d)", writeIndex);
        return false;
    }

    size_t readIndex = (static_cast<size_t>(writeIndex) + count - 1) % count;
    PLOGD("writeIndex(%d) readIndex(%zu)", writeIndex, readIndex);
    const ShmBuffer &buffer = shmBuffers_[readIndex];

    if (ppData)
        *ppData = buffer.pData;
    if (pDataSize)
    {
        // The per-buffer size slot is peer-writable too: clamp to capacity.
        size_t dataSize = *buffer.pDataSize;
        *pDataSize      = (dataSize <= dataSize_) ? dataSize : dataSize_;
    }
    if (ppMeta)
        *ppMeta = buffer.pMeta;
    if (pMetaSize)
        *pMetaSize = metaSize_;
    if (ppExtra)
        *ppExtra = buffer.pExtra;
    if (pExtraSize)
        *pExtraSize = extraSize_;
    if (ppSolution)
        *ppSolution = buffer.pSolution;
    if (pSolutionSize)
        *pSolutionSize = solutionSize_;

    return true;
}

bool CameraSharedMemoryImpl::write(const unsigned char *pData, size_t dataSize,
                                   const unsigned char *pMeta, size_t metaSize,
                                   const unsigned char *pExtra, size_t extraSize,
                                   const unsigned char *pSolution, size_t solutionSize)
{
    std::lock_guard<std::mutex> lock(m_);

    if (!shmHeader_ || shmBuffers_.empty())
    {
        PLOGE("shmHeader_ is NULL");
        return false;
    }

    if ((pData && dataSize > dataSize_) || (pMeta && metaSize > metaSize_) ||
        (pExtra && extraSize > extraSize_) || (pSolution && solutionSize > solutionSize_))
    {
        PLOGE("write size exceeds capacity: data(%zu/%zu) meta(%zu/%zu) extra(%zu/%zu) "
              "solution(%zu/%zu)",
              dataSize, dataSize_, metaSize, metaSize_, extraSize, extraSize_, solutionSize,
              solutionSize_);
        return false;
    }

    int writeIndex = shmHeader_->writeIndex;
    if (writeIndex < 0 || static_cast<size_t>(writeIndex) >= shmBuffers_.size())
        writeIndex = 0; // first write, or index corrupted by a peer
    ShmBuffer &buffer = shmBuffers_[writeIndex];

    if (pData)
    {
        *buffer.pDataSize = dataSize;
        memcpy(buffer.pData, pData, dataSize);
    }

    if (pMeta)
    {
        *buffer.pMetaSize = metaSize;
        memcpy(buffer.pMeta, pMeta, metaSize);
    }

    if (pExtra)
    {
        *buffer.pExtraSize = extraSize;
        memcpy(buffer.pExtra, pExtra, extraSize);
    }

    if (pSolution)
    {
        *buffer.pSolutionSize = solutionSize;
        memcpy(buffer.pSolution, pSolution, solutionSize);
    }

    shmHeader_->writeIndex = (writeIndex + 1) % static_cast<int>(shmBuffers_.size());

    return true;
}

int CameraSharedMemoryImpl::createSignal(const std::string &name)
{
    PLOGI("name(%s)", name.c_str());

    int efd = eventfd(0, EFD_NONBLOCK);
    if (efd == -1)
    {
        PLOGE("Fail to create eventfd");
        return -1;
    }

    if (!attachSignal(efd, name))
    {
        PLOGE("Fail to attach");
        ::close(efd);
        return -1;
    }

    PLOGI("eventfd(%d)", efd);
    return efd;
}

bool CameraSharedMemoryImpl::attachSignal(int fd, const std::string &name)
{
    PLOGI("fd(%d) name(%s)", fd, name.c_str());

    std::lock_guard<std::mutex> lock(m_);

    if (fcntl(fd, F_GETFD) == -1)
    {
        PLOGE("invalid fd %d", fd);
        return false;
    }

    auto it = signalFdMap_.find(name);
    if (it != signalFdMap_.end() && it->second != fd)
    {
        PLOGI("replacing fd(%d) for name(%s)", it->second, name.c_str());
        ::close(it->second);
    }
    signalFdMap_[name] = fd;
    PLOGI("attached fd(%d)", fd);
    return true;
}

bool CameraSharedMemoryImpl::notifySignal(void)
{
    std::lock_guard<std::mutex> lock(m_);

    // eventfd read() returns the sum of the written values; a constant 1 per
    // notification is all a level-style wakeup needs (writing an incrementing
    // counter made the very first notification write 0, which never wakes the
    // poller).
    const uint64_t one = 1;
    for (const auto &[name, fd] : signalFdMap_)
    {
        if (::write(fd, &one, sizeof(one)) != sizeof(one))
        {
            PLOGE("efd write error, fd %d", fd);
        }
    }

    return true;
}

bool CameraSharedMemoryImpl::detachSignal(const std::string &name)
{
    PLOGI("name(%s)", name.c_str());

    std::lock_guard<std::mutex> lock(m_);

    if (signalFdMap_.find(name) != signalFdMap_.end())
    {
        PLOGI("detached fd(%d)", signalFdMap_[name]);
        ::close(signalFdMap_[name]);
        signalFdMap_.erase(name);
    }

    return true;
}

void CameraSharedMemoryImpl::releaseAllSignals(void)
{
    PLOGI("");

    std::lock_guard<std::mutex> lock(m_);

    for (auto it = signalFdMap_.begin(); it != signalFdMap_.end();)
    {
        PLOGI("released fd(%d), name(%s)", it->second, it->first.c_str());
        ::close(it->second);
        it = signalFdMap_.erase(it);
    }
}

bool CameraSharedMemoryImpl::waitForSignal(int timeoutMs, const std::string &name)
{
    std::unique_lock<std::mutex> lock(m_);

    PLOGD("name(%s) (timeout %d ms)", name.c_str(), timeoutMs);

    auto it = signalFdMap_.find(name);
    if (it == signalFdMap_.end())
    {
        PLOGE("unknown signal name(%s)", name.c_str());
        return false;
    }
    int efd = it->second;

    struct pollfd fds = {efd, POLLIN, 0};

    PLOGD("name(%s) start waiting for eventfd (%d) (timeout %d ms)", name.c_str(), efd, timeoutMs);

    // Poll without holding the object lock: a wait of up to timeoutMs must not
    // block writers/notifiers using this object from other threads. If the fd
    // is detached while unlocked, poll reports POLLNVAL and we bail out.
    lock.unlock();
    int ret = poll(&fds, 1, timeoutMs);
    lock.lock();

    if (ret == -1)
    {
        PLOGE("Poll failure: %s", strerror(errno));
        return false;
    }
    else if (ret == 0)
    {
        PLOGE("Timeout! No data available to read from eventfd");
        return false;
    }

    if (fds.revents & (POLLNVAL | POLLERR))
    {
        PLOGE("eventfd (%d) no longer valid", efd);
        return false;
    }

    if (!(fds.revents & POLLIN))
    {
        PLOGE("POLLIN event did not occur!");
        return false;
    }

    // Make sure the fd was not detached (and possibly reused) while unlocked.
    it = signalFdMap_.find(name);
    if (it == signalFdMap_.end() || it->second != efd)
    {
        PLOGE("signal (%s) detached while waiting", name.c_str());
        return false;
    }

    uint64_t value;
    for (int retry = 0; retry < 100; ++retry)
    {
        if (::read(efd, &value, sizeof(value)) == sizeof(value))
        {
            PLOGD("Read %llu from eventfd (%d)", (unsigned long long)value, efd);
            return true;
        }
        PLOGE("Read failure, retrying...");

        lock.unlock(); // Unlock the mutex before sleeping
        usleep(1000);
        lock.lock(); // Re-lock the mutex after sleeping
    }

    PLOGE("Read retry failed!");
    return false;
}

int CameraSharedMemoryImpl::getWriteIndex(void)
{
    std::lock_guard<std::mutex> lock(m_);

    if (!shmHeader_)
    {
        PLOGE("shmHeader_ is NULL");
        return -1;
    }

    PLOGD("writeIndex(%d)", shmHeader_->writeIndex);
    return shmHeader_->writeIndex;
}
