#include "tb_cache.h"
#include <cstring>
#include <android/log.h>

#define TAG "EmuxTbCache"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

namespace emux {

TranslationBlockCache::TranslationBlockCache() {
    fastTable_ = new BasicBlock*[kFastTableSize]();
}

TranslationBlockCache::~TranslationBlockCache() {
    clear();
    delete[] fastTable_;
    fastTable_ = nullptr;
}

void TranslationBlockCache::clear() {
    std::lock_guard<std::mutex> lock(cacheMutex_);
    if (fastTable_) {
        std::memset(fastTable_, 0, sizeof(BasicBlock*) * kFastTableSize);
    }
    blockMap_.clear();
}

BasicBlock* TranslationBlockCache::findOrInsert(uint32_t pc, bool isThumb) {
    std::lock_guard<std::mutex> lock(cacheMutex_);
    auto it = blockMap_.find(pc);
    if (it != blockMap_.end()) {
        uint32_t idx = (pc >> 1) & (kFastTableSize - 1);
        fastTable_[idx] = &it->second;
        return &it->second;
    }

    BasicBlock bb;
    bb.guestPc = pc;
    bb.isThumb = isThumb;
    bb.insnCount = 0;
    bb.sizeBytes = 0;
    bb.hasBranch = false;

    auto res = blockMap_.emplace(pc, bb);
    BasicBlock* inserted = &res.first->second;

    uint32_t idx = (pc >> 1) & (kFastTableSize - 1);
    fastTable_[idx] = inserted;
    return inserted;
}

void TranslationBlockCache::invalidateRange(uint32_t startAddr, uint32_t size) {
    if (size == 0) return;
    std::lock_guard<std::mutex> lock(cacheMutex_);
    uint32_t endAddr = startAddr + size;

    for (auto it = blockMap_.begin(); it != blockMap_.end(); ) {
        uint32_t bStart = it->second.guestPc & ~1U;
        uint32_t bEnd = bStart + it->second.sizeBytes;
        if (!(bEnd <= startAddr || bStart >= endAddr)) {
            uint32_t idx = (it->second.guestPc >> 1) & (kFastTableSize - 1);
            if (fastTable_[idx] == &it->second) {
                fastTable_[idx] = nullptr;
            }
            it = blockMap_.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace emux
