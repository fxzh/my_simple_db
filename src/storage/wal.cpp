// wal.cpp: 预写日志实现 —— 记录追加 / fsync 提交 / 顺序读取
//
// 本文件实现 wal.h 描述的记录格式与 Wal/WalReader 两个结构。设计要点:
//   - append 只做一次 ::write, 绝不 fsync(提交由 session 层的 sync 负责);
//   - 记录写入用 O_APPEND 打开的 fd, 追加位置完全交给内核, 进程内无需记账;
//   - 多字节字段一律 memcpy 原生小端序读写, 与页格式(page.cpp)保持同一约定。
#include "wal.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <string>

#include "common/err.h"
#include "log/log.h"
#include "page.h"

namespace st {

namespace {

// 记录 payload 的长度上限: 补丁记录 20B 定位头 + 一整页, 超过必是数据损坏
constexpr uint32_t WAL_PAYLOAD_MAX = WAL_PATCH_HEADER_SIZE + PAGE_SIZE;

// 记 ERROR 日志并抛 DbError, 供编译期确认调用点终止
[[noreturn]] void raise_io(const std::string& what, int err)
{
    DB_RAISE(db::ErrCode::IoError, LogModule::STORAGE, "{}: {}", what, std::strerror(err));
}

}  // namespace

// ==================== Wal(写入侧) ====================

Wal::Wal(std::string path) : path_(std::move(path))
{
    // O_APPEND: 每次 write 自动追加到文件尾, ftruncate 清空后下一次 write 从 0 开始
    fd_ = ::open(path_.c_str(), O_RDWR | O_CREAT | O_APPEND, 0644);
    if (fd_ < 0) {
        raise_io("打开 wal.log 失败", errno);
    }
}

Wal::~Wal()
{
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

uint64_t Wal::append(WalOp op, const char* payload, uint32_t len)
{
    // 记录布局: [len u32][lsn u64][op u8][crc u32][payload]
    // crc 覆盖 lsn+op+payload(不含 len 与 crc 自身), len 由整条写入的边界保证
    char buf[WAL_HEADER_SIZE + WAL_PAYLOAD_MAX];
    const uint64_t lsn = next_lsn_;
    wal_put_u32(buf, len);
    wal_put_u64(buf + 4, lsn);
    buf[12] = static_cast<char>(op);
    std::memcpy(buf + WAL_HEADER_SIZE, payload, len);

    char crc_src[sizeof(uint64_t) + 1 + WAL_PAYLOAD_MAX];
    std::memcpy(crc_src, buf + 4, sizeof(uint64_t) + 1);
    std::memcpy(crc_src + sizeof(uint64_t) + 1, payload, len);
    wal_put_u32(buf + 13, crc32(crc_src, sizeof(uint64_t) + 1 + len));

    // 整条记录一次 write: 记录边界要么完整出现在日志里, 要么整条缺失,
    // 配合长度+CRC 让恢复端能精确识别崩溃截断点
    const size_t total = WAL_HEADER_SIZE + len;
    size_t done = 0;
    while (done < total) {
        const ssize_t n = ::write(fd_, buf + done, total - done);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            raise_io("写 wal.log 失败", errno);
        }
        done += static_cast<size_t>(n);
    }
    ++next_lsn_;
    bytes_since_reset_ += static_cast<uint64_t>(total);
    return lsn;
}

void Wal::sync()
{
    if (::fsync(fd_) != 0) {
        raise_io("fsync wal.log 失败", errno);
    }
}

void Wal::reset()
{
    // 检查点: 清空日志回到文件头。LSN 计数器不回退——本进程内缓冲池帧仍持有
    // 历史 LSN 语义(新记录必须比旧记录大), 清空文件只是让"重放起点"归零;
    // 字节计数器随文件清空一并归零
    if (::ftruncate(fd_, 0) != 0) {
        raise_io("清空 wal.log 失败", errno);
    }
    if (::fsync(fd_) != 0) {
        raise_io("fsync wal.log 失败", errno);
    }
    bytes_since_reset_ = 0;
}

// ==================== WalReader(读取侧) ====================

WalReader::WalReader(std::string path)
{
    fd_ = ::open(path.c_str(), O_RDONLY);
    if (fd_ < 0) {
        // 文件不存在视为空日志(全新数据目录): eof_ 置真, next 恒返回 false
        eof_ = true;
        return;
    }
    buf_.reserve(64 * 1024);
}

WalReader::~WalReader()
{
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

// 从 fd 继续读一块进缓冲; 无新数据(EOF 或已标记)返回 false
bool WalReader::fill()
{
    if (eof_ || fd_ < 0) {
        return false;
    }
    char chunk[16 * 1024];
    for (;;) {
        const ssize_t n = ::read(fd_, chunk, sizeof(chunk));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            raise_io("读 wal.log 失败", errno);
        }
        if (n == 0) {
            eof_ = true;
            return false;
        }
        buf_.insert(buf_.end(), chunk, chunk + n);
        return true;
    }
}

bool WalReader::next(WalRecord* out)
{
    // 1) 凑齐记录头
    while (buf_.size() < WAL_HEADER_SIZE) {
        if (!fill()) {
            return false;  // 尾部截断: 连记录头都不完整
        }
    }
    const uint32_t len = wal_get_u32(buf_.data());
    // len 异常(超上限): 视为损坏尾部, 停止解析。合法日志不会出现该值
    if (len > WAL_PAYLOAD_MAX) {
        return false;
    }
    // 2) 凑齐整条记录
    while (buf_.size() < WAL_HEADER_SIZE + len) {
        if (!fill()) {
            return false;  // 尾部截断: payload 不完整
        }
    }
    const char* rec = buf_.data();
    // 3) CRC 校验: lsn + op + payload
    char crc_src[sizeof(uint64_t) + 1 + WAL_PAYLOAD_MAX];
    std::memcpy(crc_src, rec + 4, sizeof(uint64_t) + 1);
    std::memcpy(crc_src + sizeof(uint64_t) + 1, rec + WAL_HEADER_SIZE, len);
    if (crc32(crc_src, sizeof(uint64_t) + 1 + len) != wal_get_u32(rec + 13)) {
        return false;  // 记录字节损坏(掉电写坏): 丢弃其后全部
    }
    out->lsn = wal_get_u64(rec + 4);
    out->op = static_cast<WalOp>(rec[12]);
    out->payload.assign(rec + WAL_HEADER_SIZE, rec + WAL_HEADER_SIZE + len);
    // 4) 消费本条, 记账安全截断点
    buf_.erase(buf_.begin(),
               buf_.begin() + static_cast<std::ptrdiff_t>(WAL_HEADER_SIZE + len));
    valid_bytes_ += WAL_HEADER_SIZE + len;
    return true;
}

}  // namespace st
