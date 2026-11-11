#include "FuxiBlockLayout.h"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

using namespace fuxi_layout;

static void testObserved1125x2436() {
    Descriptor desc{
        1125, // width
        2436, // height
        1,    // levels
        1,    // slices
        4,    // blockWidth
        4,    // blockHeight
        16    // blockBytes
    };

    Layout layout;
    const bool ok = make(desc, layout);
    assert(ok);
    assert(layout.level[0].rowBytes == 4512);
    assert(layout.totalBytes == 2747808);
    assert(layout.totalBlocks == 171738);
    assert(layout.level[0].blocksX == 282);
    assert(layout.level[0].blocksY == 609);
    assert(layout.level[0].offset == 0);
    assert(layout.level[0].firstBlock == 0);

    CopyPlan plan;
    const bool planOk = layout.plan(0, 0, {0, 0, 1125, 2436}, 0, 4512, 0, plan);
    assert(planOk);
    assert(plan.memoryOffset == 0);
    assert(plan.memoryRowBytes == 4512);
    assert(plan.copiedRowBytes == 4512);
    assert(plan.rows == 609);
    assert(plan.blocksPerRow == 282);
    assert(plan.blockStride == 282);
    assert(plan.linearOffset == 0);
    assert(plan.linearSpan == 2747808);
    assert(plan.firstBlock == 0);

    std::cout << "[PASS] observed 1125x2436 4x4/16" << std::endl;
}

static void test7x7PartialAndSpans() {
    Descriptor desc{7, 7, 1, 1, 4, 4, 16};
    Layout layout;
    assert(make(desc, layout));
    assert(layout.level[0].blocksX == 2);
    assert(layout.level[0].blocksY == 2);
    assert(layout.level[0].rowBytes == 32);
    assert(layout.totalBytes == 64);
    assert(layout.totalBlocks == 4);

    CopyPlan fullPlan;
    const size_t linearOffset = 64;
    const size_t linearPitch = 48; // padded, divisible by 16
    assert(layout.plan(0, 0, {0, 0, 7, 7}, linearOffset, linearPitch, 0, fullPlan));
    assert(fullPlan.copiedRowBytes == 32);
    assert(fullPlan.rows == 2);
    assert(fullPlan.blocksPerRow == 2);
    assert(fullPlan.linearOffset == 64);
    assert(fullPlan.linearSpan == 64 + 1 * 48 + 32); // 144
    assert(fullPlan.memoryOffset == 0);
    assert(fullPlan.firstBlock == 0);

    CopyPlan subPlanEdge;
    assert(layout.plan(0, 0, {4, 4, 3, 3}, 16, 16, 0, subPlanEdge));
    assert(subPlanEdge.firstBlock == 3); // (row 1 * 2 + col 1)
    assert(subPlanEdge.memoryOffset == 48);
    assert(subPlanEdge.copiedRowBytes == 16);
    assert(subPlanEdge.rows == 1);
    assert(subPlanEdge.linearSpan == 32);

    std::cout << "[PASS] 7x7 partial last blocks and spans" << std::endl;
}

static void testOdd9x5MipChainAndSlices() {
    Descriptor desc{9, 5, 4, 3, 4, 4, 16};
    Layout layout;
    assert(make(desc, layout));

    // Mip 0: 9x5 -> 3x2 blocks
    assert(layout.level[0].width == 9 && layout.level[0].height == 5);
    assert(layout.level[0].blocksX == 3 && layout.level[0].blocksY == 2);
    assert(layout.level[0].rowBytes == 48 && layout.level[0].imageBytes == 96);
    assert(layout.level[0].offset == 0 && layout.level[0].firstBlock == 0);

    // Mip 1: 4x2 -> 1x1 block
    assert(layout.level[1].width == 4 && layout.level[1].height == 2);
    assert(layout.level[1].blocksX == 1 && layout.level[1].blocksY == 1);
    assert(layout.level[1].rowBytes == 16 && layout.level[1].imageBytes == 16);
    assert(layout.level[1].offset == 96 && layout.level[1].firstBlock == 6);

    // Mip 2: 2x1 -> 1x1 block
    assert(layout.level[2].width == 2 && layout.level[2].height == 1);
    assert(layout.level[2].blocksX == 1 && layout.level[2].blocksY == 1);
    assert(layout.level[2].rowBytes == 16 && layout.level[2].imageBytes == 16);
    assert(layout.level[2].offset == 112 && layout.level[2].firstBlock == 7);

    // Mip 3: 1x1 -> 1x1 block
    assert(layout.level[3].width == 1 && layout.level[3].height == 1);
    assert(layout.level[3].blocksX == 1 && layout.level[3].blocksY == 1);
    assert(layout.level[3].rowBytes == 16 && layout.level[3].imageBytes == 16);
    assert(layout.level[3].offset == 128 && layout.level[3].firstBlock == 8);

    assert(layout.chainBytes == 144);
    assert(layout.chainBlocks == 9);
    assert(layout.totalBytes == 432);
    assert(layout.totalBlocks == 27);

    CopyPlan cp;
    // Slice 1, mip 1, region {0, 0, 4, 2}
    assert(layout.plan(1, 1, {0, 0, 4, 2}, 0, 16, 0, cp));
    assert(cp.firstBlock == 9 + 6); // slice 1 * 9 blocks + level 1 offset 6 blocks = 15
    assert(cp.memoryOffset == 15 * 16); // 240
    assert(cp.memoryRowBytes == 16);
    assert(cp.copiedRowBytes == 16);

    // Slice 2, mip 0, block at x=4, y=4 -> block col 1, row 1
    assert(layout.plan(2, 0, {4, 4, 4, 1}, 0, 16, 0, cp));
    // slice 2: 2 * 9 = 18 blocks. level 0: blockY=1 * stride 3 + blockX=1 = 4 blocks -> total 22 blocks
    assert(cp.firstBlock == 22);
    assert(cp.memoryOffset == 22 * 16); // 352

    std::cout << "[PASS] odd 9x5 mip chain and slices" << std::endl;
}

static void testTinyMipUnderBlockDimensions() {
    Descriptor desc{4, 4, 3, 1, 4, 4, 8};
    Layout layout;
    assert(make(desc, layout));
    // Mip 2 is 1x1
    assert(layout.level[2].width == 1 && layout.level[2].height == 1);
    assert(layout.level[2].blocksX == 1 && layout.level[2].blocksY == 1);

    CopyPlan cp;
    assert(layout.plan(0, 2, {0, 0, 1, 1}, 0, 8, 0, cp));
    assert(cp.blocksPerRow == 1);
    assert(cp.rows == 1);
    assert(cp.copiedRowBytes == 8);
    assert(cp.linearSpan == 8);

    std::cout << "[PASS] tiny mip under block dimensions" << std::endl;
}

static void testAlignmentAndIllegalInterior() {
    Descriptor desc{16, 16, 1, 1, 4, 4, 16};
    Layout layout;
    assert(make(desc, layout));

    CopyPlan cp;
    // Unaligned region origins
    assert(!layout.plan(0, 0, {1, 0, 4, 4}, 0, 16, 0, cp));
    assert(!layout.plan(0, 0, {0, 2, 4, 4}, 0, 16, 0, cp));
    assert(!layout.plan(0, 0, {3, 3, 4, 4}, 0, 16, 0, cp));

    // Partial region not ending at block boundary and not at mip edge
    assert(!layout.plan(0, 0, {0, 0, 6, 4}, 0, 32, 0, cp));
    assert(!layout.plan(0, 0, {0, 0, 4, 5}, 0, 16, 0, cp));
    assert(!layout.plan(0, 0, {4, 4, 2, 4}, 0, 16, 0, cp));

    // Empty regions
    assert(!layout.plan(0, 0, {0, 0, 0, 4}, 0, 16, 0, cp));
    assert(!layout.plan(0, 0, {0, 0, 4, 0}, 0, 16, 0, cp));

    // Out of bounds
    assert(!layout.plan(0, 0, {16, 0, 4, 4}, 0, 16, 0, cp));
    assert(!layout.plan(0, 0, {0, 16, 4, 4}, 0, 16, 0, cp));
    assert(!layout.plan(0, 0, {12, 0, 8, 4}, 0, 32, 0, cp));
    assert(!layout.plan(0, 0, {0, 12, 4, 8}, 0, 16, 0, cp));

    std::cout << "[PASS] alignment and illegal interior" << std::endl;
}

static void testShortRowAndImage() {
    Descriptor desc{8, 8, 1, 1, 4, 4, 16};
    Layout layout;
    assert(make(desc, layout));

    CopyPlan cp;
    // copiedRowBytes = 32, rows = 2, accessedBytes = 1 * pitch + 32
    // Pitch too small (< copiedRowBytes)
    assert(!layout.plan(0, 0, {0, 0, 8, 8}, 0, 16, 0, cp));

    // Pitch not divisible by blockBytes
    assert(!layout.plan(0, 0, {0, 0, 8, 8}, 0, 40, 0, cp));

    // Pitch 32, rows 2 -> accessedBytes = 64
    // linearBytesPerImage non-zero but < accessedBytes
    assert(!layout.plan(0, 0, {0, 0, 8, 8}, 0, 32, 63, cp));
    assert(layout.plan(0, 0, {0, 0, 8, 8}, 0, 32, 64, cp));
    assert(layout.plan(0, 0, {0, 0, 8, 8}, 0, 32, 128, cp));

    std::cout << "[PASS] short row and image limits" << std::endl;
}

static void testOverflowAndLimits() {
    Layout layout;
    CopyPlan cp;

    // Initial default layout must fail plan
    assert(!layout.plan(0, 0, {0, 0, 4, 4}, 0, 16, 0, cp));

    // Invalid descriptor values
    assert(!make({0, 16, 1, 1, 4, 4, 16}, layout));
    assert(!make({16, 0, 1, 1, 4, 4, 16}, layout));
    assert(!make({16385, 16, 1, 1, 4, 4, 16}, layout));
    assert(!make({16, 16385, 1, 1, 4, 4, 16}, layout));
    assert(!make({16, 16, 0, 1, 4, 4, 16}, layout));
    assert(!make({16, 16, 16, 1, 4, 4, 16}, layout)); // max level for 16 is 5
    assert(!make({16, 16, 1, 0, 4, 4, 16}, layout));
    assert(!make({16, 16, 1, 2049, 4, 4, 16}, layout));
    assert(!make({16, 16, 1, 1, 0, 4, 16}, layout));
    assert(!make({16, 16, 1, 1, 13, 4, 16}, layout));
    assert(!make({16, 16, 1, 1, 4, 0, 16}, layout));
    assert(!make({16, 16, 1, 1, 4, 13, 16}, layout));
    assert(!make({16, 16, 1, 1, 4, 4, 4}, layout));
    assert(!make({16, 16, 1, 1, 4, 4, 32}, layout));

    // Exceeding kMaxBytes: 16384 x 16384 with 1x1 block and 16 bytes = 4 GB > 256 MB
    assert(!make({16384, 16384, 1, 1, 1, 1, 16}, layout));

    // Overflow in linearOffset in plan
    assert(make({8, 8, 1, 1, 4, 4, 16}, layout));
    assert(!layout.plan(0, 0, {0, 0, 8, 8}, SIZE_MAX, 32, 0, cp));
    assert(!layout.plan(0, 0, {0, 0, 8, 8}, kMaxBytes, 32, 0, cp));
    assert(!layout.plan(1, 0, {0, 0, 8, 8}, 0, 32, 0, cp)); // slice out of range
    assert(!layout.plan(0, 1, {0, 0, 8, 8}, 0, 32, 0, cp)); // mip out of range

    std::cout << "[PASS] overflow and boundary limits" << std::endl;
}

static void testFailedOutputUnchanged() {
    Layout canaryLayout;
    canaryLayout.chainBytes = 777777;
    canaryLayout.totalBytes = 888888;
    assert(!make({0, 0, 0, 0, 0, 0, 0}, canaryLayout));
    assert(canaryLayout.chainBytes == 777777);
    assert(canaryLayout.totalBytes == 888888);

    Layout validLayout;
    assert(make({16, 16, 1, 1, 4, 4, 16}, validLayout));
    CopyPlan canaryPlan;
    canaryPlan.memoryOffset = 999999;
    canaryPlan.rows = 5555;
    assert(!validLayout.plan(0, 0, {1, 0, 4, 4}, 0, 16, 0, canaryPlan));
    assert(canaryPlan.memoryOffset == 999999);
    assert(canaryPlan.rows == 5555);

    std::cout << "[PASS] failed outputs remain unchanged" << std::endl;
}

static void testContentCopyRoundtrip() {
    // Odd size 7x5 texture, 4x4 blocks, 16 blockBytes
    // blocksX = 2, blocksY = 2, rowBytes = 32, imageBytes = 64
    Descriptor desc{7, 5, 1, 1, 4, 4, 16};
    Layout layout;
    assert(make(desc, layout));
    assert(layout.totalBytes == 64);

    std::vector<uint8_t> textureMemory(64, 0x00);

    CopyPlan plan;
    const size_t linearOffset = 16;
    const size_t linearPitch = 48; // padded
    assert(layout.plan(0, 0, {0, 0, 7, 5}, linearOffset, linearPitch, 0, plan));
    assert(plan.memoryOffset == 0);
    assert(plan.memoryRowBytes == 32);
    assert(plan.copiedRowBytes == 32);
    assert(plan.rows == 2);
    assert(plan.linearOffset == 16);
    assert(plan.linearSpan == 96);

    constexpr uint8_t kSentinel = 0xDE;
    std::vector<uint8_t> srcLinear(128, kSentinel);
    std::vector<uint8_t> dstLinear(128, kSentinel);

    // Fill source linear buffer at planned offsets:
    // Row 0: block 0 (16 bytes) = 0x11, block 1 (16 bytes) = 0x22
    std::memset(srcLinear.data() + linearOffset, 0x11, 16);
    std::memset(srcLinear.data() + linearOffset + 16, 0x22, 16);
    // Row 1: block 0 (16 bytes) = 0x33, block 1 (16 bytes) = 0x44
    std::memset(srcLinear.data() + linearOffset + linearPitch, 0x33, 16);
    std::memset(srcLinear.data() + linearOffset + linearPitch + 16, 0x44, 16);

    // Copy from linear buffer to texture memory using plan
    for (size_t r = 0; r < plan.rows; ++r) {
        const uint8_t *src = srcLinear.data() + plan.linearOffset + r * linearPitch;
        uint8_t *dst = textureMemory.data() + plan.memoryOffset + r * plan.memoryRowBytes;
        std::memcpy(dst, src, plan.copiedRowBytes);
    }

    // Validate texture memory at exact known byte offsets
    for (size_t i = 0; i < 16; ++i) {
        assert(textureMemory[i] == 0x11);
        assert(textureMemory[16 + i] == 0x22);
        assert(textureMemory[32 + i] == 0x33);
        assert(textureMemory[48 + i] == 0x44);
    }

    // Copy back from texture memory to destination linear buffer using plan
    for (size_t r = 0; r < plan.rows; ++r) {
        const uint8_t *src = textureMemory.data() + plan.memoryOffset + r * plan.memoryRowBytes;
        uint8_t *dst = dstLinear.data() + plan.linearOffset + r * linearPitch;
        std::memcpy(dst, src, plan.copiedRowBytes);
    }

    // Verify destination linear buffer content and intact sentinel padding
    for (size_t i = 0; i < linearOffset; ++i) {
        assert(dstLinear[i] == kSentinel);
    }
    for (size_t i = 0; i < 16; ++i) {
        assert(dstLinear[linearOffset + i] == 0x11);
        assert(dstLinear[linearOffset + 16 + i] == 0x22);
    }
    for (size_t i = linearOffset + 32; i < linearOffset + linearPitch; ++i) {
        assert(dstLinear[i] == kSentinel);
    }
    for (size_t i = 0; i < 16; ++i) {
        assert(dstLinear[linearOffset + linearPitch + i] == 0x33);
        assert(dstLinear[linearOffset + linearPitch + 16 + i] == 0x44);
    }
    for (size_t i = plan.linearSpan; i < dstLinear.size(); ++i) {
        assert(dstLinear[i] == kSentinel);
    }

    std::cout << "[PASS] content-copy roundtrip with sentinel padding" << std::endl;
}

int main() {
    testObserved1125x2436();
    test7x7PartialAndSpans();
    testOdd9x5MipChainAndSlices();
    testTinyMipUnderBlockDimensions();
    testAlignmentAndIllegalInterior();
    testShortRowAndImage();
    testOverflowAndLimits();
    testFailedOutputUnchanged();
    testContentCopyRoundtrip();

    std::cout << "All tests passed successfully." << std::endl;
    return 0;
}
