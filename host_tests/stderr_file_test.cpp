#include "proc/stderr_file.h"
#include <cassert>
#include <string>
#include <sys/wait.h>
#include <vector>

static std::string Read(int fd) {
    char block[4096]; std::string text;
    assert(lseek(fd, 0, SEEK_SET) >= 0);
    for (ssize_t n; (n = read(fd, block, sizeof(block))) > 0;) text.append(block, n);
    return text;
}

int main() {
    char path[] = "/tmp/vpp-stderr-files-XXXXXX";
    char* directory = mkdtemp(path); assert(directory);
    const time_t now = time(nullptr);
    int fd = winehua::OpenStderrFile(directory, now); assert(fd >= 0);
    assert(winehua::AppendStderrFile(fd, "first error\n", 12, 1024));
    const std::string many(900, 'x');
    assert(winehua::AppendStderrFile(fd, many.data(), many.size(), 1024));
    assert(winehua::AppendStderrFile(fd, many.data(), many.size(), 1024));
    struct stat st{}; assert(!fstat(fd, &st) && st.st_size <= 1024);
    // A concurrent writer has a distinct open file description/lock.
    std::vector<pid_t> children;
    for (int i = 0; i < 4; ++i) {
        const pid_t child = fork(); assert(child >= 0);
        if (!child) {
            const int other = winehua::OpenStderrFile(directory, now); assert(other >= 0);
            for (int j = 0; j < 100; ++j) assert(winehua::AppendStderrFile(other, many.data(), many.size(), 1024));
            close(other); _exit(0);
        }
        children.push_back(child);
    }
    for (const pid_t child : children) { int status; assert(waitpid(child, &status, 0) == child && status == 0); }
    assert(winehua::AppendStderrFile(fd, "late fatal error\n", 17, 1024));
    assert(!fstat(fd, &st) && st.st_size <= 1024); close(fd);
    struct tm local{}; localtime_r(&now, &local); char name[64];
    strftime(name, sizeof(name), "/wine_stderr_%Y%m%d.log", &local);
    const std::string today = std::string(directory) + name;
    fd = open(today.c_str(), O_RDONLY); assert(fd >= 0);
    const std::string text = Read(fd); close(fd);
    assert(text.find("older Wine stderr discarded") != std::string::npos);
    assert(text.find("late fatal error") != std::string::npos);
    const std::string old = std::string(directory) + "/wine_stderr_20000101.log";
    int oldfd = open(old.c_str(), O_CREAT | O_WRONLY, 0600); assert(oldfd >= 0); close(oldfd);
    const std::string unrelated = std::string(directory) + "/user.txt";
    oldfd = open(unrelated.c_str(), O_CREAT | O_WRONLY, 0600); assert(oldfd >= 0); close(oldfd);
    const std::string link = std::string(directory) + "/wine_stderr_20000102.log";
    assert(!symlink(unrelated.c_str(), link.c_str()));
    fd = winehua::OpenStderrFile(directory, now); assert(fd >= 0); close(fd);
    assert(access(old.c_str(), F_OK) < 0);
    assert(!lstat(link.c_str(), &st) && S_ISLNK(st.st_mode));
    assert(!access(unrelated.c_str(), F_OK));
    assert(!unlink(today.c_str()) && !symlink(unrelated.c_str(), today.c_str()));
    assert(winehua::OpenStderrFile(directory, now) < 0);
    unlink(today.c_str()); unlink(link.c_str()); unlink(unrelated.c_str()); rmdir(directory);
    puts("PASS bounded Wine stderr: concurrent processes, recent error, rollover marker, retention and symlinks");
}
