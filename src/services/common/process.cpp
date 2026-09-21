// Copyright (c) 2023 LG Electronics, Inc.
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

#define LOG_TAG "Process"
#include "process.h"

#include <dirent.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <vector>
#include "camera_log.h"
#include <cerrno>
#include <csignal>
#include <iterator>
#include <sstream>
#include <sys/wait.h>
#include <vector>

Process::Process(const std::string &cmd)
{
    PLOGI("");

    if (!start(cmd))
    {
        PLOGE("failed to start process");
    }
}

Process::~Process()
{
    PLOGI("");

    stop();
}

namespace
{
/* Close every descriptor from 3 upwards. close_range() does it in one call on
 * Linux 5.9+; the /proc walk is the fallback, and reads the directory before
 * closing so it is not iterating something it is mutating. */
void closeInheritedDescriptors()
{
#if defined(__linux__) && defined(SYS_close_range)
    if (syscall(SYS_close_range, 3, ~0U, 0) == 0)
        return;
#endif
    DIR *d = opendir("/proc/self/fd");
    if (!d)
        return;

    std::vector<int> fds;
    const int dirFd = dirfd(d);
    while (struct dirent *e = readdir(d))
    {
        int fd = atoi(e->d_name);
        if (fd >= 3 && fd != dirFd)
            fds.push_back(fd);
    }
    closedir(d);

    for (int fd : fds)
        close(fd);
}
} // namespace

bool Process::start(const std::string &cmd)
{
    PLOGI("%s", cmd.c_str());

    /* Tokenize and build argv before fork(). The parent is multithreaded, so
     * only async-signal-safe calls are allowed between fork() and exec() -
     * no heap allocation in the child. fork() copies the address space, so
     * the child can keep using these buffers as-is. */
    std::istringstream iss(cmd);
    std::vector<std::string> tokens{std::istream_iterator<std::string>{iss},
                                    std::istream_iterator<std::string>{}};
    if (tokens.empty())
    {
        PLOGE("empty command");
        return false;
    }

    std::vector<char *> argv;
    argv.reserve(tokens.size() + 1);
    for (auto &token : tokens)
    {
        argv.push_back(token.data());
    }
    argv.push_back(nullptr);

    _pid = fork();
    if (_pid < 0)
    {
        PLOGE("fork failed : %d", errno);
        return false;
    }
    if (_pid == 0)
    {
        /* Close everything the parent had open before handing over. fork()
         * duplicates every descriptor that is not O_CLOEXEC, and this service
         * has plenty by the time it spawns a HAL: luna-service2 sockets, the
         * shared-memory fds startPreview created moments earlier, and whatever
         * the loaded plugins hold. The child re-opens what it needs, so the
         * copies are pure inheritance, and passing them into a process that
         * then talks to the Android side through droidmedia is asking for
         * trouble. Keep stdin/stdout/stderr so its logging still reaches
         * journald. */
        closeInheritedDescriptors();

        execv(argv[0], argv.data());
        _exit(127);
    }

    return true;
}
void Process::stop()
{
    PLOGI("pid %d", _pid);

    if (_pid <= 0)
    {
        PLOGI("no process to stop");
        return;
    }

    /* Ask nicely first : after the luna 'release' call the solution process
     * quits its own main loop, so SIGTERM is normally a no-op and the WNOHANG
     * poll just reaps the exit. Escalate to SIGKILL only if it is still
     * around after ~3s. */
    if (kill(_pid, SIGTERM) == -1)
    {
        PLOGE("kill(SIGTERM) error : %d", errno);
    }

    int status    = 0;
    pid_t waitPid = -1;
    for (int i = 0; i < 300; i++) // 10ms * 300 = 3s
    {
        waitPid = waitpid(_pid, &status, WNOHANG);
        if (waitPid != 0)
            break;
        usleep(10000);
    }

    if (waitPid == 0)
    {
        PLOGE("pid %d did not exit in time; sending SIGKILL", _pid);
        if (kill(_pid, SIGKILL) == -1)
        {
            PLOGE("kill(SIGKILL) error : %d", errno);
        }
        waitPid = waitpid(_pid, &status, 0);
    }

    if (waitPid == -1)
    {
        PLOGE("error : %d", errno);
    }
    else
    {
        if (WIFEXITED(status))
        {
            PLOGI("normal exit status %d", WEXITSTATUS(status));
        }
        else if (WIFSIGNALED(status))
        {
            PLOGI("abnormal exit status %d", WTERMSIG(status));
        }
    }

    PLOGI("end pid %d", waitPid);
    _pid = -1;
}