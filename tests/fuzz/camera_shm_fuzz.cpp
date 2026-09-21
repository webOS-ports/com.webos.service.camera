// Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
//
// SPDX-License-Identifier: Apache-2.0
//
// Hardening harness for the shared-memory consumer path: feeds thousands of
// randomized/hostile ShmHeader layouts (and post-open header corruption)
// through CameraSharedMemoryEx and expects graceful failure, never a crash.
// Runs on target as a plain binary:
//
//   camera-shm-fuzz [iterations] [seed]
//
// Exit code 0 means every iteration was survived. A crash (signal) is the
// failure signal — run it under a watchdog or just check the exit code.

#include "camera_shared_memory_ex.h"

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <random>
#include <string>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

namespace
{

#pragma pack(push, 4)
struct RawHeader
{
    int writeIndex;
    size_t bufferCount;
    size_t dataSize;
    size_t metaSize;
    size_t extraSize;
    size_t solutionSize;
};
#pragma pack(pop)

// Values that historically break size arithmetic and index math.
const size_t kEdge[] = {0,
                        1,
                        7,
                        8,
                        9,
                        63,
                        64,
                        65,
                        4095,
                        4096,
                        66355200,
                        66355201,
                        (size_t)INT32_MAX,
                        (size_t)INT32_MAX + 1,
                        SIZE_MAX / 8,
                        SIZE_MAX / 2,
                        SIZE_MAX - 1,
                        SIZE_MAX};

size_t pickSize(std::mt19937_64 &rng)
{
    if (rng() % 2)
        return kEdge[rng() % (sizeof(kEdge) / sizeof(kEdge[0]))];
    return rng() % (1 << 20);
}

int makeSegment(const std::string &name, const RawHeader &header, size_t payload)
{
    shm_unlink(name.c_str());
    int fd = shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0)
        return -1;
    size_t total = sizeof(header) + payload;
    if (ftruncate(fd, total) != 0)
    {
        close(fd);
        shm_unlink(name.c_str());
        return -1;
    }
    void *addr = mmap(nullptr, sizeof(header), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED)
    {
        close(fd);
        shm_unlink(name.c_str());
        return -1;
    }
    memcpy(addr, &header, sizeof(header));
    munmap(addr, sizeof(header));
    return fd;
}

} // namespace

int main(int argc, char **argv)
{
    unsigned long iterations = (argc > 1) ? strtoul(argv[1], nullptr, 10) : 2000;
    uint64_t seed            = (argc > 2) ? strtoull(argv[2], nullptr, 10) : 0xC0FFEE;
    std::mt19937_64 rng(seed);

    std::string name = "/camfuzz-" + std::to_string(getpid());
    unsigned long rejected = 0, accepted = 0;

    for (unsigned long i = 0; i < iterations; ++i)
    {
        RawHeader h;
        h.writeIndex   = (int)(rng() % 2 ? rng() : rng() % 16) - 4;
        h.bufferCount  = pickSize(rng);
        h.dataSize     = pickSize(rng);
        h.metaSize     = pickSize(rng);
        h.extraSize    = pickSize(rng);
        h.solutionSize = pickSize(rng);
        size_t payload = rng() % (1 << 22); // up to 4MB of real backing

        int fd = makeSegment(name, h, payload);
        if (fd < 0)
            continue;

        {
            CameraSharedMemoryEx shm;
            if (shm.open(fd))
            {
                ++accepted;
                // Exercise the consumer paths against whatever was accepted.
                unsigned char *p = nullptr;
                size_t sz        = 0;
                shm.read(&p, &sz, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, 0, true);
                shm.getWriteIndex();
                shm.incrementWriteIndex();
                shm.writeHeader((int)(rng() % 32) - 8, pickSize(rng));
                std::vector<unsigned char> junk(64, 0x5A);
                shm.write(junk.data(), junk.size(), junk.data(), junk.size(), nullptr, 0, nullptr,
                          0);

                // Corrupt the live header mid-flight like a hostile peer.
                void *addr =
                    mmap(nullptr, sizeof(RawHeader), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
                if (addr != MAP_FAILED)
                {
                    RawHeader *live   = static_cast<RawHeader *>(addr);
                    live->writeIndex  = (int)rng();
                    live->bufferCount = pickSize(rng);
                    live->dataSize    = pickSize(rng);
                    munmap(addr, sizeof(RawHeader));
                }
                shm.read(&p, &sz, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, 0, true);
                shm.write(junk.data(), junk.size(), nullptr, 0, nullptr, 0, nullptr, 0);
                shm.incrementWriteIndex();
            }
            else
            {
                ++rejected;
            }
        }
        close(fd);
        shm_unlink(name.c_str());

        if ((i + 1) % 500 == 0)
            printf("progress %lu/%lu (accepted %lu, rejected %lu)\n", i + 1, iterations, accepted,
                   rejected);
    }

    printf("done: %lu iterations, %lu accepted, %lu rejected, seed %" PRIu64 "\n", iterations,
           accepted, rejected, seed);
    return 0;
}
