// store.h — 剪贴板条目库
//
// 防丢铁律：
//   1. 自动淘汰 / 过期只碰未置顶条目
//   2. 条数上限只统计未置顶条目，置顶项不占额度
//   3. store.dat 校验失败时改名保留，绝不覆盖清空
//
// 设计要点：
//   - 所有内容（文本/图片/HTML/RTF/文件列表）统一存为二进制 blob，无外部文件
//   - 快捷键不跟条目走，由 config 按位置管理
//   - 置顶区按手动顺序排，不因使用而重排
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

enum class ItemKind : uint32_t {
    Text = 0,      // CF_UNICODETEXT 纯文本（data = UTF-16LE 字节）
    Image = 1,     // 图片（data = PNG 字节）
    Html = 2,      // "HTML Format"（data = 剪贴板原始字节，含 Version: 头）
    Rtf = 3,       // "Rich Text Format"（data = RTF 原始字节）
    FileDrop = 4,  // CF_HDROP（data = UTF-16LE 文本，每行一个路径）
};

struct Item {
    uint64_t id = 0;
    ItemKind kind = ItemKind::Text;
    bool pinned = false;
    uint64_t createdAt = 0;
    uint64_t usedAt = 0;
    std::vector<uint8_t> data;  // 统一二进制内容
    uint32_t imgW = 0;          // 仅 Image 有效
    uint32_t imgH = 0;
    std::wstring preview;       // 列表显示用，运行时重算，不落盘
    uint64_t hash = 0;          // 去重用，运行时算，不落盘
};

class Store {
public:
    enum class LoadResult {
        Ok,       // 正常读到（含空库）
        Corrupt,  // 文件坏了，已改名保留，当前以空库运行
    };

    LoadResult Load();
    bool Save();

    const std::vector<Item>& Items() const { return items_; }
    const Item* Find(uint64_t id) const;

    // 添加条目，返回 id；命中去重时刷新 usedAt 并返回已有 id
    uint64_t Add(ItemKind kind, std::vector<uint8_t> data, uint32_t imgW = 0, uint32_t imgH = 0);

    bool SetPinned(uint64_t id, bool pinned);
    bool Remove(uint64_t id);
    bool Touch(uint64_t id);  // 更新 usedAt 并重排非置顶区

    // 置顶排序
    bool MovePinned(uint64_t id, int delta);         // 上移/下移 delta 位
    bool MovePinnedTo(uint64_t id, int targetIndex); // 拖拽到指定位置

    void SetLimits(int maxHistory, int expiryDays);
    void ExpireCheck();  // 清除过期的非置顶条目

    // 序列化到内存（主线程调用，很快），然后交给 AsyncWriter 写盘
    std::vector<uint8_t> Serialize();

    // 所有条目 data 的总字节数
    uint64_t TotalDataSize() const;

    int PinnedCount() const;
    int HistoryCount() const;
    const std::wstring& CorruptBackupPath() const { return corruptBackup_; }

    // 便捷取值
    static std::wstring TextOf(const Item& item);  // 从 data 解出文本（Text/Html/Rtf/FileDrop）

private:
    Item* FindMutable(uint64_t id);
    void Reorder();
    void Evict();
    LoadResult PreserveCorrupt();

    std::vector<Item> items_;  // 置顶区在前（手动顺序），非置顶在后（usedAt 降序）
    uint64_t nextId_ = 1;
    int maxHistory_ = 50;
    int expiryDays_ = 0;
    std::wstring corruptBackup_;
};

// 根据条目内容生成一行摘要
std::wstring MakeItemPreview(const Item& item);
