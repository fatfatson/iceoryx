// Copyright (c) 2021 - 2022 by Apex.AI Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// SPDX-License-Identifier: Apache-2.0

#include "iceoryx_hoofs/posix_wrapper/file_lock.hpp"
#include "iceoryx_hoofs/posix_wrapper/posix_call.hpp"
#include "iceoryx_hoofs/posix_wrapper/types.hpp"
#include "iceoryx_platform/errno.hpp"
#include "iceoryx_platform/fcntl.hpp"
#include "iceoryx_platform/stat.hpp"
#include "iceoryx_platform/unistd.hpp"
#include "iox/logging.hpp"

#include "iceoryx_platform/platform_correction.hpp"

namespace iox
{
namespace posix
{
/// NOLINTJUSTIFICATION see declaration
/// NOLINTNEXTLINE(hicpp-avoid-c-arrays,cppcoreguidelines-avoid-c-arrays)
constexpr const char FileLock::LOCK_FILE_SUFFIX[];

expected<FileLock, FileLockError> FileLockBuilder::create() noexcept
{
    if (!isValidFileName(m_name))
    {
        IOX_LOG(ERROR) << "Unable to create FileLock since the name \"" << m_name << "\" is not a valid file name.";
        return error<FileLockError>(FileLockError::INVALID_FILE_NAME);
    }

    if (!isValidPathToDirectory(m_path))
    {
        IOX_LOG(ERROR) << "Unable to create FileLock since the path \"" << m_path << "\" is not a valid path.";
        return error<FileLockError>(FileLockError::INVALID_PATH);
    }

    FileLock::FilePath_t fileLockPath = m_path;

    if (!doesEndWithPathSeparator(fileLockPath))
    {
        fileLockPath.unsafe_append(iox::platform::IOX_PATH_SEPARATORS[0]);
    }

    fileLockPath.unsafe_append(m_name);
    fileLockPath.unsafe_append(FileLock::LOCK_FILE_SUFFIX);

    auto openCall = posixCall(iox_open)(fileLockPath.c_str(),
                                        convertToOflags(AccessMode::READ_ONLY, OpenMode::OPEN_OR_CREATE),
                                        m_permission.value())
                        .failureReturnValue(-1)
                        .evaluate();

    if (openCall.has_error())
    {
        return error<FileLockError>(FileLock::convertErrnoToFileLockError(openCall.get_error().errnum, fileLockPath));
    }

    auto fileDescriptor = openCall.value().value;
    auto lockCall = posixCall(iox_flock)(fileDescriptor, static_cast<int>(FileLock::LockOperation::LOCK))
                        .failureReturnValue(-1)
                        .suppressErrorMessagesForErrnos(EWOULDBLOCK)
                        .evaluate();

    if (lockCall.has_error())
    {
        posixCall(iox_close)(fileDescriptor).failureReturnValue(-1).evaluate().or_else([&](auto& result) {
            IOX_DISCARD_RESULT(FileLock::convertErrnoToFileLockError(result.errnum, fileLockPath));
            IOX_LOG(ERROR) << "Unable to close file lock \"" << fileLockPath
                           << "\" in error related cleanup during initialization.";
        });

        //  possible errors in iox_close() are masked and we inform the user about the actual error
        return error<FileLockError>(FileLock::convertErrnoToFileLockError(lockCall.get_error().errnum, fileLockPath));
    }

    return success<FileLock>(FileLock(fileDescriptor, fileLockPath));
}

FileLock::FileLock(const int32_t fileDescriptor, const FilePath_t& path) noexcept
    : m_fd{fileDescriptor}
    , m_fileLockPath{path}
{
}

FileLock::FileLock(FileLock&& rhs) noexcept
{
    *this = std::move(rhs);
}

FileLock& FileLock::operator=(FileLock&& rhs) noexcept
{
#pragma clang diagnostic ignored "-Wtautological-undefined-compare"
    /*
    检查&rhs!=nullptr是为了避免在前置资源初始化失败的情况下还要继续跑，会崩掉，
    而我们希望只是报个错不要崩。只能说这个库容错性太差了！
    而且检查的方式要注意不能直接与nullptr比较，因为在release版会被编译优化掉（认为引用不可能为空）
    但事实上这个库就是能搞出这样的事。。
    */
    auto* rhsAsPtr = (char*)&rhs + 1;
    if (this != &rhs && rhsAsPtr != (char*)1)
    {
        if (closeFileDescriptor().has_error())
        {
            IOX_LOG(ERROR) << "Unable to cleanup file lock \"" << m_fileLockPath
                           << "\" in the move constructor/move assingment operator";
        }

        m_fileLockPath = std::move(rhs.m_fileLockPath);
        m_fd = rhs.m_fd;

        rhs.invalidate();
    }

    return *this;
}

FileLock::~FileLock() noexcept
{
    if (closeFileDescriptor().has_error())
    {
        IOX_LOG(ERROR) << "unable to cleanup file lock \"" << m_fileLockPath << "\" in the destructor";
    }
}

expected<FileLockError> FileLock::closeFileDescriptor() noexcept
{
    if (m_fd != INVALID_FD)
    {
        bool cleanupFailed = false;
        posixCall(iox_flock)(m_fd, static_cast<int>(LockOperation::UNLOCK))
            .failureReturnValue(-1)
            .suppressErrorMessagesForErrnos(EWOULDBLOCK)
            .evaluate()
            .or_else([&](auto& result) {
                cleanupFailed = true;
                IOX_DISCARD_RESULT(FileLock::convertErrnoToFileLockError(result.errnum, m_fileLockPath));
                IOX_LOG(ERROR) << "Unable to unlock the file lock \"" << m_fileLockPath << "\"";
            });

        posixCall(iox_close)(m_fd).failureReturnValue(-1).evaluate().or_else([&](auto& result) {
            cleanupFailed = true;
            IOX_DISCARD_RESULT(FileLock::convertErrnoToFileLockError(result.errnum, m_fileLockPath));
            IOX_LOG(ERROR) << "Unable to close the file handle to the file lock \"" << m_fileLockPath << "\"";
        });

        posixCall(remove)(m_fileLockPath.c_str()).failureReturnValue(-1).evaluate().or_else([&](auto& result) {
            cleanupFailed = true;
            IOX_DISCARD_RESULT(FileLock::convertErrnoToFileLockError(result.errnum, m_fileLockPath));
            IOX_LOG(ERROR) << "Unable to remove the file lock \"" << m_fileLockPath << "\"";
        });

        if (cleanupFailed)
        {
            return error<FileLockError>(FileLockError::INTERNAL_LOGIC_ERROR);
        }
    }
    return success<>();
}

void FileLock::invalidate() noexcept
{
    m_fd = INVALID_FD;
    m_fileLockPath.assign("");
}

// NOLINTJUSTIFICATION the function size is unavoidable due to the errno translation and the cognitive complexity
// results from the expanded log macro
// NOLINTNEXTLINE(readability-function-size,readability-function-cognitive-complexity)
FileLockError FileLock::convertErrnoToFileLockError(const int32_t errnum, const FilePath_t& fileLockPath) noexcept
{
    switch (errnum)
    {
    case EACCES:
    {
        IOX_LOG(ERROR) << "permission denied for file lock \"" << fileLockPath << "\"";
        return FileLockError::ACCESS_DENIED;
    }
    case EDQUOT:
    {
        IOX_LOG(ERROR) << "user disk quota exhausted for file lock \"" << fileLockPath << "\"";
        return FileLockError::QUOTA_EXHAUSTED;
    }
    case EFAULT:
    {
        IOX_LOG(ERROR) << "outside address space error for file lock \"" << fileLockPath << "\"";
        return FileLockError::ACCESS_DENIED;
    }
    case EFBIG:
    case EOVERFLOW:
    {
        IOX_LOG(ERROR) << "file lock \"" << fileLockPath << "\""
                       << " is too large to be openend";
        return FileLockError::FILE_TOO_LARGE;
    }
    case ELOOP:
    {
        IOX_LOG(ERROR) << "too many symbolic links for file lock \"" << fileLockPath << "\"";
        return FileLockError::INVALID_FILE_NAME;
    }
    case EMFILE:
    {
        IOX_LOG(ERROR) << "process limit reached for file lock \"" << fileLockPath << "\"";
        return FileLockError::PROCESS_LIMIT;
    }
    case ENFILE:
    {
        IOX_LOG(ERROR) << "system limit reached for file lock \"" << fileLockPath << "\"";
        return FileLockError::SYSTEM_LIMIT;
    }
    case ENODEV:
    {
        IOX_LOG(ERROR) << "permission to access file lock denied \"" << fileLockPath << "\"";
        return FileLockError::ACCESS_DENIED;
    }
    case ENOENT:
    {
        IOX_LOG(ERROR) << "directory \"" << &platform::IOX_LOCK_FILE_PATH_PREFIX[0] << "\""
                       << " does not exist.";
        return FileLockError::NO_SUCH_DIRECTORY;
    }
    case ENOMEM:
    {
        IOX_LOG(ERROR) << "out of memory for file lock \"" << fileLockPath << "\"";
        return FileLockError::OUT_OF_MEMORY;
    }
    case ENOSPC:
    {
        IOX_LOG(ERROR) << "Device has no space for file lock \"" << fileLockPath << "\"";
        return FileLockError::QUOTA_EXHAUSTED;
    }
    case ENOSYS:
    {
        IOX_LOG(ERROR) << "open() not implemented for filesystem to \"" << fileLockPath << "\"";
        return FileLockError::SYS_CALL_NOT_IMPLEMENTED;
    }
    case ENXIO:
    {
        IOX_LOG(ERROR) << "\"" << fileLockPath << "\""
                       << " is a special file and no corresponding device exists";
        return FileLockError::SPECIAL_FILE;
    }
    case EPERM:
    {
        IOX_LOG(ERROR) << "permission denied to file lock \"" << fileLockPath << "\"";
        return FileLockError::ACCESS_DENIED;
    }
    case EROFS:
    {
        IOX_LOG(ERROR) << "read only error for file lock \"" << fileLockPath << "\"";
        return FileLockError::INVALID_FILE_NAME;
    }
    case ETXTBSY:
    {
        IOX_LOG(ERROR) << "write access requested for file lock \"" << fileLockPath << "\""
                       << " in use";
        return FileLockError::FILE_IN_USE;
    }
    case EWOULDBLOCK:
    {
        // no error message needed since this is a normal use case
        return FileLockError::LOCKED_BY_OTHER_PROCESS;
    }
    case ENOLCK:
    {
        IOX_LOG(ERROR) << "system limit for locks reached for file lock \"" << fileLockPath << "\"";
        return FileLockError::SYSTEM_LIMIT;
    }
    case EIO:
    {
        IOX_LOG(ERROR) << "I/O for file lock \"" << fileLockPath << "\"";
        return FileLockError::I_O_ERROR;
    }
    default:
    {
        IOX_LOG(ERROR) << "internal logic error in file lock \"" << fileLockPath << "\" occurred";
        return FileLockError::INTERNAL_LOGIC_ERROR;
    }
    }
}

} // namespace posix
} // namespace iox
