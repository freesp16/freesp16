#include "services/library/FileOps.h"

#include "services/store/SampleRefs.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <vector>

namespace services::library
{
namespace
{
using core::Result;

Result<juce::File> osError(const char* what, const juce::File& f, int err)
{
    return Result<juce::File>::failure(std::string("Cannot ") + what + " " + f.getFileName().toStdString(),
                                       f.getFullPathName().toStdString() + ": " + std::strerror(err));
}

std::string path(const juce::File& f)
{
    return f.getFullPathName().toStdString();
}

bool writeAll(int fd, const unsigned char* p, std::size_t n)
{
    while (n > 0)
    {
        const ssize_t w = ::write(fd, p, n);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            return false;
        p += w;
        n -= static_cast<std::size_t>(w);
    }
    return true;
}

juce::String safeName(const juce::String& name)
{
    return juce::String(services::store::safeFileName(name.toStdString()));
}
} // namespace

bool fsyncPath(const juce::File& f)
{
    const int fd = ::open(path(f).c_str(), O_RDONLY);
    if (fd < 0)
        return false;
    const bool ok = ::fsync(fd) == 0;
    ::close(fd);
    return ok;
}

juce::File FileOps::uniqueChild(const juce::File& folder, const juce::String& fileName)
{
    juce::File f = folder.getChildFile(fileName);
    if (! f.exists())
        return f;
    const juce::String stem = f.getFileNameWithoutExtension(), ext = f.getFileExtension();
    for (int n = 2;; ++n)
    {
        f = folder.getChildFile(stem + " " + juce::String(n) + ext);
        if (! f.exists())
            return f;
    }
}

Result<juce::File> FileOps::copyTo(const juce::File& source, const juce::File& target) const
{
    if (source.isDirectory())
    {
        if (! target.createDirectory())
            return osError("create", target, errno);
        for (const auto& child :
             source.findChildFiles(juce::File::findFilesAndDirectories, false, "*", juce::File::FollowSymlinks::no))
            if (auto r = copyTo(child, target.getChildFile(child.getFileName())); ! r)
                return r;
        fsyncPath(target);
        return target;
    }
    const int in = ::open(path(source).c_str(), O_RDONLY);
    if (in < 0)
        return osError("read", source, errno);
    const juce::File tmp = target.getSiblingFile(target.getFileName() + ".tmp-" +
                                                 juce::String::toHexString(juce::Random::getSystemRandom().nextInt()));
    const int out = ::open(path(tmp).c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (out < 0)
    {
        const int err = errno;
        ::close(in);
        return osError("write", target, err);
    }
    step("write", tmp);
    std::vector<unsigned char> buffer(1 << 20);
    bool ok = true;
    int err = 0;
    for (;;)
    {
        const ssize_t r = ::read(in, buffer.data(), buffer.size());
        if (r < 0 && errno == EINTR)
            continue;
        if (r < 0)
        {
            ok = false, err = errno;
            break;
        }
        if (r == 0)
            break;
        if (! writeAll(out, buffer.data(), static_cast<std::size_t>(r)))
        {
            ok = false, err = errno;
            break;
        }
    }
    ::close(in);
    if (ok && ::fsync(out) != 0)
        ok = false, err = errno;
    else if (ok)
        step("fsync", tmp);
    ::close(out);
    if (ok && std::rename(path(tmp).c_str(), path(target).c_str()) != 0)
        ok = false, err = errno;
    if (! ok)
    {
        tmp.deleteFile();
        return osError("copy", source, err);
    }
    step("rename", target);
    if (fsyncPath(target.getParentDirectory()))
        step("fsync-dir", target.getParentDirectory());
    return target;
}

Result<juce::File> FileOps::moveTo(const juce::File& source, const juce::File& target) const
{
    if (! forceCrossVolume_)
    {
        if (std::rename(path(source).c_str(), path(target).c_str()) == 0)
        {
            step("rename", target);
            fsyncPath(target.getParentDirectory());
            fsyncPath(source.getParentDirectory());
            step("fsync-dir", target.getParentDirectory());
            return target;
        }
        if (errno != EXDEV)
            return osError("move", source, errno);
    }
    auto copied = copyTo(source, target); // durable before the source goes
    if (! copied)
        return copied;
    step("delete-source", source);
    if (! (source.isDirectory() ? source.deleteRecursively() : source.deleteFile()))
        return osError("delete", source, errno);
    fsyncPath(source.getParentDirectory());
    return target;
}

Result<juce::File> FileOps::copy(const juce::File& source, const juce::File& destFolder) const
{
    if (! source.exists())
        return osError("find", source, ENOENT);
    if (! destFolder.isDirectory())
        return osError("find", destFolder, ENOENT);
    if (source.isDirectory() && destFolder.isAChildOf(source))
        return osError("copy into itself", source, EINVAL);
    return copyTo(source, uniqueChild(destFolder, source.getFileName()));
}

Result<juce::File> FileOps::move(const juce::File& source, const juce::File& destFolder) const
{
    if (! source.exists())
        return osError("find", source, ENOENT);
    if (! destFolder.isDirectory())
        return osError("find", destFolder, ENOENT);
    if (source.getParentDirectory() == destFolder)
        return source;
    if (source.isDirectory() && (destFolder == source || destFolder.isAChildOf(source)))
        return osError("move into itself", source, EINVAL);
    return moveTo(source, uniqueChild(destFolder, source.getFileName()));
}

Result<juce::File> FileOps::rename(const juce::File& source, const juce::String& newName) const
{
    if (! source.exists())
        return osError("find", source, ENOENT);
    const juce::String name = safeName(newName);
    if (name == source.getFileName())
        return source;
    const juce::File target = source.getSiblingFile(name);
    if (target.exists())
        return Result<juce::File>::failure("A file with this name already exists", path(target));
    if (std::rename(path(source).c_str(), path(target).c_str()) != 0)
        return osError("rename", source, errno);
    fsyncPath(target.getParentDirectory());
    return target;
}

Result<juce::File> FileOps::trash(const juce::File& source) const
{
    if (! source.exists())
        return osError("find", source, ENOENT);
    if (! trash_.createDirectory())
        return osError("create", trash_, errno);
    return moveTo(source, uniqueChild(trash_, source.getFileName()));
}

Result<juce::File> FileOps::newFolder(const juce::File& parent, const juce::String& name) const
{
    if (! parent.isDirectory())
        return osError("find", parent, ENOENT);
    const juce::File f = uniqueChild(parent, safeName(name));
    if (! f.createDirectory())
        return osError("create", f, errno);
    fsyncPath(parent);
    return f;
}
} // namespace services::library
