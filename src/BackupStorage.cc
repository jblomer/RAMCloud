/* Copyright (c) 2010 Stanford University
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR(S) DISCLAIM ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL AUTHORS BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include "Common.h"
#include "BackupStorage.h"

namespace RAMCloud {

// --- BackupStorage::Handle ---

int32_t BackupStorage::Handle::allocatedHandlesCount = 0;

// --- SingleFileStorage ---

// - public -

/**
 * Create a SingleFileStorage.
 *
 * \param segmentSize
 *      The size in bytes of the segments this storage will deal with.
 * \param segmentFrames
 *      The number of segments this storage can store simultaneously.
 * \param filePath
 *      A filesystem path to the device or file where segments will be stored.
 * \param openFlags
 *      Extra flags for use while opening filePath (default to 0, O_DIRECT may
 *      be used to disable the OS buffer cache.
 */
SingleFileStorage::SingleFileStorage(uint32_t segmentSize,
                                     uint32_t segmentFrames,
                                     const char* filePath,
                                     int openFlags)
    : BackupStorage(segmentSize, Type::DISK)
    , freeMap(segmentFrames)
    , fd(-1)
    , lastAllocatedFrame(FreeMap::npos)
    , segmentFrames(segmentFrames)
{
    freeMap.set();

    fd = open(filePath,
              O_CREAT | O_RDWR | openFlags,
              0666);
    if (fd == -1)
        throw BackupStorageException(HERE,
              format("Failed to open backup storage file %s", filePath), errno);

    // If its a regular file reserve space, otherwise
    // assume its a device and we don't need to bother.
    struct stat st;
    int r = stat(filePath, &st);
    if (r == -1)
        return;
    if (st.st_mode & S_IFREG)
        reserveSpace();
}

/// Close the file.
SingleFileStorage::~SingleFileStorage()
{
    int r = close(fd);
    if (r == -1)
        LOG(ERROR, "Couldn't close backup log");
}

// See BackupStorage::allocate().
BackupStorage::Handle*
SingleFileStorage::allocate(uint64_t masterId,
                            uint64_t segmentId)
{
    FreeMap::size_type next = freeMap.find_next(lastAllocatedFrame);
    if (next == FreeMap::npos) {
        next = freeMap.find_first();
        if (next == FreeMap::npos)
            throw BackupStorageException(HERE, "Out of free segment frames.");
    }
    lastAllocatedFrame = next;
    uint32_t targetSegmentFrame = static_cast<uint32_t>(next);
    LOG(DEBUG, "Writing <%lu,%lu> to frame %u", masterId, segmentId,
                                                targetSegmentFrame);
    freeMap[targetSegmentFrame] = 0;
    return new Handle(targetSegmentFrame);
}

// See BackupStorage::free().
void
SingleFileStorage::free(BackupStorage::Handle* handle)
{
    uint32_t segmentFrame =
        static_cast<const Handle*>(handle)->getSegmentFrame();

    off_t offset = lseek(fd,
                         offsetOfSegmentFrame(segmentFrame),
                         SEEK_SET);
    if (offset == -1)
        throw BackupStorageException(HERE,
                "Failed to seek to segment frame to free storage", errno);
    const char* killMessage = "FREE";
    ssize_t killMessageLen = strlen(killMessage);
    ssize_t r = write(fd, killMessage, killMessageLen);
    if (r != killMessageLen)
        throw BackupStorageException(HERE,
                "Couldn't overwrite stored segment header to free", errno);

    freeMap[segmentFrame] = 1;
    delete handle;
}

// See BackupStorage::getSegment().
// NOTE: This must remain thread-safe, so be careful about adding
// access to other resources.
void
SingleFileStorage::getSegment(const BackupStorage::Handle* handle,
                              char* segment) const
{
    uint32_t sourceSegmentFrame =
        static_cast<const Handle*>(handle)->getSegmentFrame();
    off_t offset = offsetOfSegmentFrame(sourceSegmentFrame);
    ssize_t r = pread(fd, segment, segmentSize, offset);
    if (r != static_cast<ssize_t>(segmentSize))
        throw BackupStorageException(HERE, errno);
}

// See BackupStorage::putSegment().
// NOTE: This must remain thread-safe, so be careful about adding
// access to other resources.
void
SingleFileStorage::putSegment(const BackupStorage::Handle* handle,
                              const char* segment) const
{
    uint32_t targetSegmentFrame =
        static_cast<const Handle*>(handle)->getSegmentFrame();
    off_t offset = offsetOfSegmentFrame(targetSegmentFrame);
    ssize_t r = pwrite(fd, segment, segmentSize, offset);
    if (r != static_cast<ssize_t>(segmentSize))
        throw BackupStorageException(HERE, errno);
}

// - private -

/**
 * Pure function.
 *
 * \param segmentFrame
 *      The segmentFrame to find the offset of in the file.
 * \return
 *      The offset into the file a particular segmentFrame starts at.
 */
uint64_t
SingleFileStorage::offsetOfSegmentFrame(uint32_t segmentFrame) const
{
    return segmentFrame * segmentSize;
}

/**
 * Fix the size of the logfile to ensure that the OS doesn't tell us
 * the filesystem is out of space later.
 *
 * \throw BackupStorageException
 *      If space for segmentFrames segments of segmentSize cannot be reserved.
 */
void
SingleFileStorage::reserveSpace()
{
    uint64_t logSpace = segmentSize;
    logSpace *= segmentFrames;

    LOG(DEBUG, "Reserving %lu bytes of log space", logSpace);
    int r = ftruncate(fd, logSpace);
    if (r == -1)
        throw BackupStorageException(HERE,
                "Couldn't reserve storage space for backup", errno);
}

// --- InMemoryStorage ---

// - public -

/**
 * Create an in-memory storage area.
 *
 * \param segmentSize
 *      The size of segments this storage will house.
 * \param segmentFrames
 *      The number of segments this storage can house.
 */
InMemoryStorage::InMemoryStorage(uint32_t segmentSize,
                                 uint32_t segmentFrames)
    : BackupStorage(segmentSize, Type::MEMORY)
    , pool(segmentSize)
    , segmentFrames(segmentFrames)
{
}

/// Free up all storage.
InMemoryStorage::~InMemoryStorage()
{
}

// See BackupStorage::allocate().
BackupStorage::Handle*
InMemoryStorage::allocate(uint64_t masterId,
                          uint64_t segmentId)
{
    if (!segmentFrames)
        throw BackupStorageException(HERE, "Out of free segment frames.");
    segmentFrames--;
    char* address = static_cast<char *>(pool.malloc());
    return new Handle(address);
}

// See BackupStorage::free().
void
InMemoryStorage::free(BackupStorage::Handle* handle)
{
    TEST_LOG("called");
    char* address =
        static_cast<const Handle*>(handle)->getAddress();
    memcpy(address, "FREE", 4);
    pool.free(address);
    delete handle;
}

// See BackupStorage::getSegment().
// NOTE: This must remain thread-safe, so be careful about adding
// access to other resources.
void
InMemoryStorage::getSegment(const BackupStorage::Handle* handle,
                            char* segment) const
{
    char* address = static_cast<const Handle*>(handle)->getAddress();
    memcpy(segment, address, segmentSize);
}

// See BackupStorage::putSegment().
// NOTE: This must remain thread-safe, so be careful about adding
// access to other resources.
void
InMemoryStorage::putSegment(const BackupStorage::Handle* handle,
                            const char* segment) const
{
    char* address = static_cast<const Handle*>(handle)->getAddress();
    memcpy(address, segment, segmentSize);
}

} // namespace RAMCloud
