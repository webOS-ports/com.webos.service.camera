// Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
//
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the CameraSharedMemoryEx ring buffer. The interesting cases
// are the hostile ones: a consumer must survive mapping a segment whose
// header was written (or later corrupted) by a malicious or crashed peer.

#include <gtest/gtest.h>

#include "camera_shared_memory_ex.h"

#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace
{

// Mirror of the shared header layout (cross-process ABI, see
// camera_shared_memory_impl.h). Redeclared here so tests can corrupt it
// through a raw mapping the way a hostile peer would.
#pragma pack(push, 4)
struct TestShmHeader
{
    int writeIndex;
    size_t bufferCount;
    size_t dataSize;
    size_t metaSize;
    size_t extraSize;
    size_t solutionSize;
};
#pragma pack(pop)

std::string uniqueName(const char *tag)
{
    return std::string("/camtest-") + tag + "-" + std::to_string(getpid());
}

class ScopedShm
{
public:
    explicit ScopedShm(const std::string &name) : name_(name) {}
    ~ScopedShm() { shm_unlink(name_.c_str()); }

private:
    std::string name_;
};

// Creates a raw shm object holding the given header plus `extraBytes` of
// zeroed payload and returns an fd, or -1.
int makeRawSegment(const std::string &name, const TestShmHeader &header, size_t extraBytes)
{
    shm_unlink(name.c_str());
    int fd = shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0)
        return -1;
    size_t total = sizeof(header) + extraBytes;
    if (ftruncate(fd, total) != 0)
    {
        close(fd);
        return -1;
    }
    void *addr = mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED)
    {
        close(fd);
        return -1;
    }
    memcpy(addr, &header, sizeof(header));
    munmap(addr, total);
    return fd;
}

constexpr size_t kData     = 4096;
constexpr size_t kMeta     = 256;
constexpr size_t kExtra    = 64;
constexpr size_t kSolution = 128;
constexpr size_t kCount    = 4;
constexpr size_t kSection  = kData + kMeta + kExtra + kSolution + sizeof(size_t) * 4;

} // namespace

TEST(CameraSharedMemory, CreateAndInfoRoundtrip)
{
    std::string name = uniqueName("info");
    ScopedShm guard(name);

    CameraSharedMemoryEx shm;
    int fd = shm.create(name, kData, kMeta, kExtra, kSolution, kCount);
    ASSERT_GE(fd, 0);

    size_t count = 0, data = 0, meta = 0, extra = 0, solution = 0;
    ASSERT_TRUE(shm.getBufferInfo(&count, &data, &meta, &extra, &solution));
    EXPECT_EQ(count, kCount);
    EXPECT_EQ(data, kData);
    EXPECT_EQ(meta, kMeta);
    EXPECT_EQ(extra, kExtra);
    EXPECT_EQ(solution, kSolution);
}

TEST(CameraSharedMemory, CreateRejectsBadGeometry)
{
    std::string name = uniqueName("badgeo");
    ScopedShm guard(name);

    CameraSharedMemoryEx shm;
    EXPECT_LT(shm.create(name, 0, kMeta, kExtra, kSolution, kCount), 0);      // no data
    EXPECT_LT(shm.create(name, kData, kMeta, kExtra, kSolution, 0), 0);       // no buffers
    EXPECT_LT(shm.create(name, kData, kMeta, kExtra, kSolution, 100000), 0);  // absurd count
    EXPECT_LT(shm.create(name, SIZE_MAX / 2, kMeta, kExtra, kSolution, 8), 0); // overflow
    // A rejected create must not leave the name behind.
    int fd = shm_open(name.c_str(), O_RDWR, 0600);
    EXPECT_EQ(fd, -1);
}

TEST(CameraSharedMemory, WriteReadRoundtrip)
{
    std::string name = uniqueName("rw");
    ScopedShm guard(name);

    CameraSharedMemoryEx writer;
    int fd = writer.create(name, kData, kMeta, kExtra, kSolution, kCount);
    ASSERT_GE(fd, 0);

    unsigned char frame[kData];
    memset(frame, 0xA5, sizeof(frame));
    const unsigned char meta[] = "{\"ts\":1}";
    ASSERT_TRUE(writer.write(frame, sizeof(frame), meta, sizeof(meta), nullptr, 0, nullptr, 0));

    CameraSharedMemoryEx reader;
    ASSERT_TRUE(reader.open(fd));

    unsigned char *pData = nullptr;
    size_t dataSize      = 0;
    ASSERT_TRUE(reader.read(&pData, &dataSize, nullptr, nullptr, nullptr, nullptr, nullptr,
                            nullptr, 0, true /* skipSignal */));
    ASSERT_NE(pData, nullptr);
    ASSERT_EQ(dataSize, sizeof(frame));
    EXPECT_EQ(memcmp(pData, frame, dataSize), 0);
}

TEST(CameraSharedMemory, WriteRejectsOversizedPayload)
{
    std::string name = uniqueName("oversize");
    ScopedShm guard(name);

    CameraSharedMemoryEx shm;
    ASSERT_GE(shm.create(name, kData, kMeta, kExtra, kSolution, kCount), 0);

    std::vector<unsigned char> big(kData + 1, 0xFF);
    EXPECT_FALSE(shm.write(big.data(), big.size(), nullptr, 0, nullptr, 0, nullptr, 0));

    std::vector<unsigned char> bigMeta(kMeta + 1, 0xFF);
    unsigned char frame[16] = {};
    EXPECT_FALSE(shm.write(frame, sizeof(frame), bigMeta.data(), bigMeta.size(), nullptr, 0,
                           nullptr, 0));
}

TEST(CameraSharedMemory, WriteHeaderValidatesIndexAndSize)
{
    std::string name = uniqueName("hdr");
    ScopedShm guard(name);

    CameraSharedMemoryEx shm;
    ASSERT_GE(shm.create(name, kData, kMeta, kExtra, kSolution, kCount), 0);

    EXPECT_TRUE(shm.writeHeader(0, kData));
    EXPECT_FALSE(shm.writeHeader(-1, 16));
    EXPECT_FALSE(shm.writeHeader((int)kCount, 16));
    EXPECT_FALSE(shm.writeHeader(1000000, 16));
    EXPECT_FALSE(shm.writeHeader(0, kData + 1));
}

TEST(CameraSharedMemory, OpenRejectsTruncatedSegment)
{
    std::string name = uniqueName("trunc");
    ScopedShm guard(name);

    shm_unlink(name.c_str());
    int fd = shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(ftruncate(fd, 8), 0); // smaller than any header

    CameraSharedMemoryEx shm;
    EXPECT_FALSE(shm.open(fd));
    close(fd);
}

TEST(CameraSharedMemory, OpenRejectsHostileHeader)
{
    struct Case
    {
        const char *tag;
        TestShmHeader header;
        size_t extraBytes;
    };
    const Case cases[] = {
        {"count0", {-1, 0, kData, kMeta, kExtra, kSolution}, kSection * kCount},
        {"hugecount", {-1, SIZE_MAX / 8, kData, kMeta, kExtra, kSolution}, kSection * kCount},
        {"overflow", {-1, 8, SIZE_MAX / 2, 0, 0, 0}, kSection * kCount},
        // Header claims more payload than the file actually holds.
        {"short", {-1, kCount, kData, kMeta, kExtra, kSolution}, kSection * kCount - 64},
    };

    for (const auto &c : cases)
    {
        std::string name = uniqueName(c.tag);
        ScopedShm guard(name);
        int fd = makeRawSegment(name, c.header, c.extraBytes);
        ASSERT_GE(fd, 0) << c.tag;

        CameraSharedMemoryEx shm;
        EXPECT_FALSE(shm.open(fd)) << c.tag;
        close(fd);
    }
}

TEST(CameraSharedMemory, ReadSurvivesCorruptedWriteIndex)
{
    std::string name = uniqueName("poison");
    ScopedShm guard(name);

    CameraSharedMemoryEx writer;
    int fd = writer.create(name, kData, kMeta, kExtra, kSolution, kCount);
    ASSERT_GE(fd, 0);

    unsigned char frame[64] = {1, 2, 3};
    ASSERT_TRUE(writer.write(frame, sizeof(frame), nullptr, 0, nullptr, 0, nullptr, 0));

    CameraSharedMemoryEx reader;
    ASSERT_TRUE(reader.open(fd));

    // Corrupt the shared writeIndex the way a hostile peer would.
    size_t mapLen = sizeof(TestShmHeader);
    void *addr    = mmap(nullptr, mapLen, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ASSERT_NE(addr, MAP_FAILED);
    static_cast<TestShmHeader *>(addr)->writeIndex = 1000000;

    unsigned char *pData = nullptr;
    size_t dataSize      = 0;
    // Must fail cleanly (after the bounded retry loop), not crash.
    EXPECT_FALSE(reader.read(&pData, &dataSize, nullptr, nullptr, nullptr, nullptr, nullptr,
                             nullptr, 0, true));

    static_cast<TestShmHeader *>(addr)->writeIndex = -12345;
    EXPECT_FALSE(reader.read(&pData, &dataSize, nullptr, nullptr, nullptr, nullptr, nullptr,
                             nullptr, 0, true));

    // Restore and confirm the reader recovers.
    static_cast<TestShmHeader *>(addr)->writeIndex = 1;
    EXPECT_TRUE(reader.read(&pData, &dataSize, nullptr, nullptr, nullptr, nullptr, nullptr,
                            nullptr, 0, true));
    munmap(addr, mapLen);
}

TEST(CameraSharedMemory, ReadClampsPoisonedDataSize)
{
    std::string name = uniqueName("clamp");
    ScopedShm guard(name);

    CameraSharedMemoryEx writer;
    int fd = writer.create(name, kData, kMeta, kExtra, kSolution, kCount);
    ASSERT_GE(fd, 0);

    unsigned char frame[64] = {};
    ASSERT_TRUE(writer.write(frame, sizeof(frame), nullptr, 0, nullptr, 0, nullptr, 0));

    // Poison buffer 0's size slot (first size_t after the header).
    size_t mapLen = sizeof(TestShmHeader) + sizeof(size_t);
    void *addr    = mmap(nullptr, mapLen, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ASSERT_NE(addr, MAP_FAILED);
    size_t *slot = reinterpret_cast<size_t *>(static_cast<unsigned char *>(addr) +
                                              sizeof(TestShmHeader));
    *slot        = SIZE_MAX;
    munmap(addr, mapLen);

    CameraSharedMemoryEx reader;
    ASSERT_TRUE(reader.open(fd));

    unsigned char *pData = nullptr;
    size_t dataSize      = 0;
    ASSERT_TRUE(reader.read(&pData, &dataSize, nullptr, nullptr, nullptr, nullptr, nullptr,
                            nullptr, 0, true));
    EXPECT_LE(dataSize, kData); // clamped to capacity, never SIZE_MAX
}

TEST(CameraSharedMemory, FirstSignalWakesWaiter)
{
    std::string name = uniqueName("signal");
    ScopedShm guard(name);

    CameraSharedMemoryEx shm;
    ASSERT_GE(shm.create(name, kData, kMeta, kExtra, kSolution, kCount), 0);

    int efd = shm.createSignal();
    ASSERT_GE(efd, 0);

    // Regression: the very first notify used to write 0 to the eventfd,
    // which never wakes the poller.
    ASSERT_TRUE(shm.notifySignal());
    EXPECT_TRUE(shm.waitForSignal(200));

    // No pending event now: the wait must time out, not hang or succeed.
    EXPECT_FALSE(shm.waitForSignal(100));
}

TEST(CameraSharedMemory, IncrementWriteIndexWraps)
{
    std::string name = uniqueName("wrap");
    ScopedShm guard(name);

    CameraSharedMemoryEx shm;
    ASSERT_GE(shm.create(name, kData, kMeta, kExtra, kSolution, kCount), 0);

    EXPECT_EQ(shm.getWriteIndex(), -1);
    for (size_t i = 0; i < kCount * 2; ++i)
    {
        ASSERT_TRUE(shm.incrementWriteIndex());
        int idx = shm.getWriteIndex();
        ASSERT_GE(idx, 0);
        ASSERT_LT(idx, (int)kCount);
    }
}

int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
