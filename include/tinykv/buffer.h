#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace tinykv {

// 可增长字节缓冲区（非环形）。
//
// 内存布局：
//   [0 ... read_index)            已消费数据
//   [read_index ... write_index)  可读数据
//   [write_index ... capacity)    可写空间
//
// 空间不足时先压缩（把未读数据搬到头部），仍不足再扩容（2 倍增长）。
// 为防止慢/恶意客户端造成无限扩容，设有最大容量上限。
class Buffer {
public:
    static constexpr std::size_t kDefaultSize = 4096;
    static constexpr std::size_t kMaxCapacity = 64u * 1024u * 1024u;  // 64MB

    explicit Buffer(std::size_t initial = kDefaultSize);

    std::size_t readable_bytes() const { return write_index_ - read_index_; }
    std::size_t writable_bytes() const { return data_.size() - write_index_; }

    // 当前可读区起始指针（只读视图）。可读为空时仍返回合法指针。
    const char* peek() const { return data_.data() + read_index_; }

    // 确保至少有 len 字节可写空间。返回 false 表示超过上限或溢出。
    bool ensure_writable(std::size_t len);

    // 追加数据。返回 false 表示失败（超过上限等），失败时不改变内容。
    bool append(const void* data, std::size_t len);
    bool append(const std::string& s) { return append(s.data(), s.size()); }

    // 消费前 len 字节；len 超过可读则等价于全部消费。
    void retrieve(std::size_t len);
    void retrieve_all();

    // 从非阻塞 fd 读取并追加，循环读到 EAGAIN。
    // 返回本次累计读取字节数：
    //   >0  读到的字节数
    //    0  EAGAIN（暂无数据）或 EOF；通过 *eof 区分（true 表示对端关闭）
    //   -1  出错，errno 写入 *saved_errno
    ssize_t read_fd(int fd, int* saved_errno, bool* eof);

private:
    // std::vector 的分配经全局 operator new 重载路由到选定后端。
    std::vector<char> data_;
    std::size_t read_index_ = 0;
    std::size_t write_index_ = 0;

    char* begin_write() { return data_.data() + write_index_; }
};

}  // namespace tinykv
