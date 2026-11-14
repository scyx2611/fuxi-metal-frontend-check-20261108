#ifndef FUXI_BLOCK_LAYOUT_H
#define FUXI_BLOCK_LAYOUT_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>

namespace fuxi_layout {

constexpr size_t kMaxLevels = 15;
constexpr size_t kMaxBytes = 256 * 1024 * 1024;
constexpr size_t kMaxDimension = 16384;
constexpr size_t kMaxSlices = 2048;

struct Descriptor {
    size_t width;
    size_t height;
    size_t levels;
    size_t slices;
    size_t blockWidth;
    size_t blockHeight;
    size_t blockBytes;
};

struct Level {
    size_t width = 0;
    size_t height = 0;
    size_t blocksX = 0;
    size_t blocksY = 0;
    size_t rowBytes = 0;
    size_t imageBytes = 0;
    size_t offset = 0;
    size_t firstBlock = 0;
};

struct Region {
    size_t x;
    size_t y;
    size_t width;
    size_t height;
};

struct CopyPlan {
    size_t memoryOffset = 0;
    size_t memoryRowBytes = 0;
    size_t copiedRowBytes = 0;
    size_t rows = 0;
    size_t linearOffset = 0;
    size_t linearSpan = 0;
    size_t firstBlock = 0;
    size_t blockStride = 0;
    size_t blocksPerRow = 0;
};

namespace detail {

inline bool safeAdd(size_t a, size_t b, size_t &out) {
    if (std::numeric_limits<size_t>::max() - a < b) {
        return false;
    }
    out = a + b;
    return true;
}

inline bool safeMul(size_t a, size_t b, size_t &out) {
    if (a != 0 && b > std::numeric_limits<size_t>::max() / a) {
        return false;
    }
    out = a * b;
    return true;
}

inline size_t maxLevelsForDimension(size_t maxDim) {
    size_t count = 1;
    while ((maxDim >>= 1) > 0) {
        ++count;
    }
    return std::min<size_t>(kMaxLevels, count);
}

} // namespace detail

struct Layout {
    Descriptor descriptor{};
    std::array<Level, kMaxLevels> level{};
    size_t chainBytes = 0;
    size_t totalBytes = 0;
    size_t chainBlocks = 0;
    size_t totalBlocks = 0;

    bool plan(size_t slice,
              size_t mip,
              Region region,
              size_t linearOffset,
              size_t linearBytesPerRow,
              size_t linearBytesPerImage,
              CopyPlan &out) const {
        if (descriptor.levels == 0 || descriptor.levels > kMaxLevels || descriptor.slices == 0 ||
            descriptor.blockWidth == 0 || descriptor.blockHeight == 0 ||
            descriptor.blockBytes == 0 || totalBytes == 0) {
            return false;
        }

        if (slice >= descriptor.slices || mip >= descriptor.levels) {
            return false;
        }

        const Level &lvl = level[mip];

        if (region.width == 0 || region.height == 0) {
            return false;
        }

        size_t endX = 0;
        if (!detail::safeAdd(region.x, region.width, endX) || endX > lvl.width) {
            return false;
        }

        size_t endY = 0;
        if (!detail::safeAdd(region.y, region.height, endY) || endY > lvl.height) {
            return false;
        }

        if ((region.x % descriptor.blockWidth != 0) || (region.y % descriptor.blockHeight != 0)) {
            return false;
        }

        const bool endXAtBlock = (region.width % descriptor.blockWidth == 0);
        const bool endXAtEdge = (endX == lvl.width);
        if (!endXAtBlock && !endXAtEdge) {
            return false;
        }

        const bool endYAtBlock = (region.height % descriptor.blockHeight == 0);
        const bool endYAtEdge = (endY == lvl.height);
        if (!endYAtBlock && !endYAtEdge) {
            return false;
        }

        const size_t blocksPerRow = (region.width + descriptor.blockWidth - 1) / descriptor.blockWidth;
        const size_t rows = (region.height + descriptor.blockHeight - 1) / descriptor.blockHeight;

        size_t copiedRowBytes = 0;
        if (!detail::safeMul(blocksPerRow, descriptor.blockBytes, copiedRowBytes)) {
            return false;
        }

        if (linearBytesPerRow < copiedRowBytes || (linearBytesPerRow % descriptor.blockBytes != 0)) {
            return false;
        }

        size_t rowSpan = 0;
        if (rows > 1) {
            if (!detail::safeMul(linearBytesPerRow, rows - 1, rowSpan)) {
                return false;
            }
        }

        size_t accessedBytes = 0;
        if (!detail::safeAdd(rowSpan, copiedRowBytes, accessedBytes)) {
            return false;
        }

        if (linearBytesPerImage != 0 && linearBytesPerImage < accessedBytes) {
            return false;
        }

        size_t linearSpan = 0;
        if (!detail::safeAdd(linearOffset, accessedBytes, linearSpan)) {
            return false;
        }
        if (linearSpan > kMaxBytes) {
            return false;
        }

        const size_t blockX = region.x / descriptor.blockWidth;
        const size_t blockY = region.y / descriptor.blockHeight;

        size_t blockYTimesStride = 0;
        if (!detail::safeMul(blockY, lvl.blocksX, blockYTimesStride)) {
            return false;
        }

        size_t blockOffsetInLevel = 0;
        if (!detail::safeAdd(blockYTimesStride, blockX, blockOffsetInLevel)) {
            return false;
        }

        size_t sliceBlocks = 0;
        if (!detail::safeMul(slice, chainBlocks, sliceBlocks)) {
            return false;
        }

        size_t firstBlock = 0;
        if (!detail::safeAdd(sliceBlocks, lvl.firstBlock, firstBlock) ||
            !detail::safeAdd(firstBlock, blockOffsetInLevel, firstBlock)) {
            return false;
        }

        size_t memoryOffset = 0;
        if (!detail::safeMul(firstBlock, descriptor.blockBytes, memoryOffset)) {
            return false;
        }

        size_t finalRows = 0, finalByte = 0;
        if (!detail::safeMul(rows - 1, lvl.rowBytes, finalRows) ||
            !detail::safeAdd(memoryOffset, finalRows, finalByte) ||
            !detail::safeAdd(finalByte, copiedRowBytes, finalByte) || finalByte > totalBytes) {
            return false;
        }

        CopyPlan temp{};
        temp.memoryOffset = memoryOffset;
        temp.memoryRowBytes = lvl.rowBytes;
        temp.copiedRowBytes = copiedRowBytes;
        temp.rows = rows;
        temp.linearOffset = linearOffset;
        temp.linearSpan = linearSpan;
        temp.firstBlock = firstBlock;
        temp.blockStride = lvl.blocksX;
        temp.blocksPerRow = blocksPerRow;

        out = temp;
        return true;
    }
};

inline bool make(Descriptor descriptor, Layout &out) {
    if (descriptor.width < 1 || descriptor.width > kMaxDimension) {
        return false;
    }
    if (descriptor.height < 1 || descriptor.height > kMaxDimension) {
        return false;
    }
    if (descriptor.slices < 1 || descriptor.slices > kMaxSlices) {
        return false;
    }
    if (descriptor.blockWidth < 1 || descriptor.blockWidth > 12) {
        return false;
    }
    if (descriptor.blockHeight < 1 || descriptor.blockHeight > 12) {
        return false;
    }
    if (descriptor.blockBytes != 8 && descriptor.blockBytes != 16) {
        return false;
    }

    const size_t maxDim = std::max(descriptor.width, descriptor.height);
    const size_t allowedLevels = detail::maxLevelsForDimension(maxDim);
    if (descriptor.levels < 1 || descriptor.levels > allowedLevels) {
        return false;
    }

    Layout temp{};
    temp.descriptor = descriptor;

    size_t currentOffset = 0;
    for (size_t mip = 0; mip < descriptor.levels; ++mip) {
        const size_t w = std::max<size_t>(1, descriptor.width >> mip);
        const size_t h = std::max<size_t>(1, descriptor.height >> mip);

        const size_t blocksX = (w + descriptor.blockWidth - 1) / descriptor.blockWidth;
        const size_t blocksY = (h + descriptor.blockHeight - 1) / descriptor.blockHeight;

        size_t rowBytes = 0;
        if (!detail::safeMul(blocksX, descriptor.blockBytes, rowBytes)) {
            return false;
        }

        size_t imageBytes = 0;
        if (!detail::safeMul(rowBytes, blocksY, imageBytes)) {
            return false;
        }

        temp.level[mip].width = w;
        temp.level[mip].height = h;
        temp.level[mip].blocksX = blocksX;
        temp.level[mip].blocksY = blocksY;
        temp.level[mip].rowBytes = rowBytes;
        temp.level[mip].imageBytes = imageBytes;
        temp.level[mip].offset = currentOffset;
        temp.level[mip].firstBlock = currentOffset / descriptor.blockBytes;

        if (!detail::safeAdd(currentOffset, imageBytes, currentOffset)) {
            return false;
        }
    }

    temp.chainBytes = currentOffset;
    temp.chainBlocks = currentOffset / descriptor.blockBytes;

    if (!detail::safeMul(temp.chainBytes, descriptor.slices, temp.totalBytes)) {
        return false;
    }
    if (temp.totalBytes > kMaxBytes) {
        return false;
    }

    temp.totalBlocks = temp.totalBytes / descriptor.blockBytes;

    out = temp;
    return true;
}

} // namespace fuxi_layout

#endif // FUXI_BLOCK_LAYOUT_H
