/*
 * Copyright 2014 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "BufferQueueCore"
#define ATRACE_TAG ATRACE_TAG_GRAPHICS
//#define LOG_NDEBUG 0

#define EGL_EGLEXT_PROTOTYPES

#include <inttypes.h>

#include <gui/BufferItem.h>
#include <gui/BufferQueueCore.h>
#include <gui/IConsumerListener.h>
#include <gui/IGraphicBufferAlloc.h>
#include <gui/IProducerListener.h>
#include <gui/ISurfaceComposer.h>
#include <private/gui/ComposerService.h>

namespace android {

static String8 getUniqueName() {
    static volatile int32_t counter = 0;
    return String8::format("unnamed-%d-%d", getpid(),
            android_atomic_inc(&counter));
}

BufferQueueCore::BufferQueueCore(const sp<IGraphicBufferAlloc>& allocator) :
    mAllocator(allocator),
    mMutex(),
    mIsAbandoned(false),
    mConsumerControlledByApp(false),
    mConsumerName(getUniqueName()),
    mConsumerListener(),
    mConsumerUsageBits(0),
    mConnectedApi(NO_CONNECTED_API),
    mConnectedProducerListener(),
    mSlots(),
    mQueue(),
    mFreeSlots(),
    mFreeBuffers(),
    mUnusedSlots(),
    mActiveBuffers(),
    mDequeueCondition(),
    mDequeueBufferCannotBlock(false),
    mDefaultBufferFormat(PIXEL_FORMAT_RGBA_8888),
    mDefaultWidth(1),
    mDefaultHeight(1),
    mDefaultBufferDataSpace(HAL_DATASPACE_UNKNOWN),
    mMaxBufferCount(BufferQueueDefs::NUM_BUFFER_SLOTS),
    mMaxAcquiredBufferCount(1),
    mMaxDequeuedBufferCount(1),
    mBufferHasBeenQueued(false),
    mFrameCounter(0),
    mTransformHint(0),
    mIsAllocating(false),
    mIsAllocatingCondition(),
    mAllowAllocation(true),
    mBufferAge(0),
    mGenerationNumber(0),
    mAsyncMode(false),
    mSingleBufferMode(false),
    mSingleBufferSlot(INVALID_BUFFER_SLOT),
    mSingleBufferCache(Rect::INVALID_RECT, 0, NATIVE_WINDOW_SCALING_MODE_FREEZE,
            HAL_DATASPACE_UNKNOWN)
{
    if (allocator == NULL) {
        sp<ISurfaceComposer> composer(ComposerService::getComposerService());
        mAllocator = composer->createGraphicBufferAlloc();
        if (mAllocator == NULL) {
            BQ_LOGE("createGraphicBufferAlloc failed");
        }
    }

    int numStartingBuffers = getMaxBufferCountLocked();
    for (int s = 0; s < numStartingBuffers; s++) {
        mFreeSlots.insert(s);
    }
    for (int s = numStartingBuffers; s < BufferQueueDefs::NUM_BUFFER_SLOTS;
            s++) {
        mUnusedSlots.push_front(s);
    }
}

BufferQueueCore::~BufferQueueCore() {}

void BufferQueueCore::dump(String8& result, const char* prefix) const {
    Mutex::Autolock lock(mMutex);

    String8 fifo;
    Fifo::const_iterator current(mQueue.begin());
    while (current != mQueue.end()) {
        fifo.appendFormat("%02d:%p crop=[%d,%d,%d,%d], "
                "xform=0x%02x, time=%#" PRIx64 ", scale=%s\n",
                current->mSlot, current->mGraphicBuffer.get(),
                current->mCrop.left, current->mCrop.top, current->mCrop.right,
                current->mCrop.bottom, current->mTransform, current->mTimestamp,
                BufferItem::scalingModeName(current->mScalingMode));
        ++current;
    }

    result.appendFormat("%s-BufferQueue mMaxAcquiredBufferCount=%d, "
            "mMaxDequeuedBufferCount=%d, mDequeueBufferCannotBlock=%d "
            "mAsyncMode=%d, default-size=[%dx%d], default-format=%d, "
            "transform-hint=%02x, FIFO(%zu)={%s}\n", prefix,
            mMaxAcquiredBufferCount, mMaxDequeuedBufferCount,
            mDequeueBufferCannotBlock, mAsyncMode, mDefaultWidth,
            mDefaultHeight, mDefaultBufferFormat, mTransformHint, mQueue.size(),
            fifo.string());

    for (int s : mActiveBuffers) {
        const sp<GraphicBuffer>& buffer(mSlots[s].mGraphicBuffer);
        result.appendFormat("%s%s[%02d:%p] state=%-8s, %p [%4ux%4u:%4u,%3X]\n",
                prefix, (mSlots[s].mBufferState.isAcquired()) ? ">" : " ", s,
                buffer.get(), mSlots[s].mBufferState.string(), buffer->handle,
                buffer->width, buffer->height, buffer->stride, buffer->format);

    }
    for (int s : mFreeBuffers) {
        const sp<GraphicBuffer>& buffer(mSlots[s].mGraphicBuffer);
        result.appendFormat("%s [%02d:%p] state=%-8s, %p [%4ux%4u:%4u,%3X]\n",
                prefix, s, buffer.get(), mSlots[s].mBufferState.string(),
                buffer->handle, buffer->width, buffer->height, buffer->stride,
                buffer->format);
    }

    for (int s : mFreeSlots) {
        const sp<GraphicBuffer>& buffer(mSlots[s].mGraphicBuffer);
        result.appendFormat("%s [%02d:%p] state=%-8s\n", prefix, s,
                buffer.get(), mSlots[s].mBufferState.string());
    }
}

int BufferQueueCore::getMinUndequeuedBufferCountLocked() const {
    // If dequeueBuffer is allowed to error out, we don't have to add an
    // extra buffer.
    if (mAsyncMode || mDequeueBufferCannotBlock) {
        return mMaxAcquiredBufferCount + 1;
    }

    return mMaxAcquiredBufferCount;
}

int BufferQueueCore::getMinMaxBufferCountLocked() const {
    return getMinUndequeuedBufferCountLocked() + 1;
}

int BufferQueueCore::getMaxBufferCountLocked(bool asyncMode,
        bool dequeueBufferCannotBlock, int maxBufferCount) const {
    int maxCount = mMaxAcquiredBufferCount + mMaxDequeuedBufferCount +
            ((asyncMode || dequeueBufferCannotBlock) ? 1 : 0);
    maxCount = std::min(maxBufferCount, maxCount);
    return maxCount;
}

int BufferQueueCore::getMaxBufferCountLocked() const {
    int maxBufferCount = mMaxAcquiredBufferCount + mMaxDequeuedBufferCount +
            ((mAsyncMode || mDequeueBufferCannotBlock) ? 1 : 0);

    // limit maxBufferCount by mMaxBufferCount always
    maxBufferCount = std::min(mMaxBufferCount, maxBufferCount);

    return maxBufferCount;
}

void BufferQueueCore::clearBufferSlotLocked(int slot) {
    BQ_LOGV("clearBufferSlotLocked: slot %d", slot);

    mSlots[slot].mGraphicBuffer.clear();
    mSlots[slot].mBufferState.reset();
    mSlots[slot].mRequestBufferCalled = false;
    mSlots[slot].mFrameNumber = 0;
    mSlots[slot].mAcquireCalled = false;
    mSlots[slot].mNeedsReallocation = true;

    // Destroy fence as BufferQueue now takes ownership
    if (mSlots[slot].mEglFence != EGL_NO_SYNC_KHR) {
        eglDestroySyncKHR(mSlots[slot].mEglDisplay, mSlots[slot].mEglFence);
        mSlots[slot].mEglFence = EGL_NO_SYNC_KHR;
    }
    mSlots[slot].mFence = Fence::NO_FENCE;
    mSlots[slot].mEglDisplay = EGL_NO_DISPLAY;
}

void BufferQueueCore::freeAllBuffersLocked() {
    for (int s : mFreeSlots) {
        clearBufferSlotLocked(s);
    }

    for (int s : mFreeBuffers) {
        mFreeSlots.insert(s);
        clearBufferSlotLocked(s);
    }
    mFreeBuffers.clear();

    for (int s : mActiveBuffers) {
        mFreeSlots.insert(s);
        clearBufferSlotLocked(s);
    }
    mActiveBuffers.clear();

    for (auto& b : mQueue) {
        b.mIsStale = true;
    }

    validateConsistencyLocked();
}

bool BufferQueueCore::adjustAvailableSlotsLocked(int delta) {
    if (delta >= 0) {
        while (delta > 0) {
            if (mUnusedSlots.empty()) {
                return false;
            }
            int slot = mUnusedSlots.back();
            mUnusedSlots.pop_back();
            mFreeSlots.insert(slot);
            delta--;
        }
    } else {
        while (delta < 0) {
            if (!mFreeSlots.empty()) {
                auto slot = mFreeSlots.begin();
                clearBufferSlotLocked(*slot);
                mUnusedSlots.push_back(*slot);
                mFreeSlots.erase(slot);
            } else if (!mFreeBuffers.empty()) {
                int slot = mFreeBuffers.back();
                clearBufferSlotLocked(slot);
                mUnusedSlots.push_back(slot);
                mFreeBuffers.pop_back();
            } else {
                return false;
            }
            delta++;
        }
    }
    return true;
}

void BufferQueueCore::waitWhileAllocatingLocked() const {
    ATRACE_CALL();
    while (mIsAllocating) {
        mIsAllocatingCondition.wait(mMutex);
    }
}

void BufferQueueCore::validateConsistencyLocked() const {
    static const useconds_t PAUSE_TIME = 0;
    int allocatedSlots = 0;
    for (int slot = 0; slot < BufferQueueDefs::NUM_BUFFER_SLOTS; ++slot) {
        bool isInFreeSlots = mFreeSlots.count(slot) != 0;
        bool isInFreeBuffers =
                std::find(mFreeBuffers.cbegin(), mFreeBuffers.cend(), slot) !=
                mFreeBuffers.cend();
        bool isInActiveBuffers = mActiveBuffers.count(slot) != 0;
        bool isInUnusedSlots =
                std::find(mUnusedSlots.cbegin(), mUnusedSlots.cend(), slot) !=
                mUnusedSlots.cend();

        if (isInFreeSlots || isInFreeBuffers || isInActiveBuffers) {
            allocatedSlots++;
        }

        if (isInUnusedSlots) {
            if (isInFreeSlots) {
                BQ_LOGE("Slot %d is in mUnusedSlots and in mFreeSlots", slot);
                usleep(PAUSE_TIME);
            }
            if (isInFreeBuffers) {
                BQ_LOGE("Slot %d is in mUnusedSlots and in mFreeBuffers", slot);
                usleep(PAUSE_TIME);
            }
            if (isInActiveBuffers) {
                BQ_LOGE("Slot %d is in mUnusedSlots and in mActiveBuffers",
                        slot);
                usleep(PAUSE_TIME);
            }
            if (!mSlots[slot].mBufferState.isFree()) {
                BQ_LOGE("Slot %d is in mUnusedSlots but is not FREE", slot);
                usleep(PAUSE_TIME);
            }
            if (mSlots[slot].mGraphicBuffer != NULL) {
                BQ_LOGE("Slot %d is in mUnusedSluts but has an active buffer",
                        slot);
                usleep(PAUSE_TIME);
            }
        } else if (isInFreeSlots) {
            if (isInUnusedSlots) {
                BQ_LOGE("Slot %d is in mFreeSlots and in mUnusedSlots", slot);
                usleep(PAUSE_TIME);
            }
            if (isInFreeBuffers) {
                BQ_LOGE("Slot %d is in mFreeSlots and in mFreeBuffers", slot);
                usleep(PAUSE_TIME);
            }
            if (isInActiveBuffers) {
                BQ_LOGE("Slot %d is in mFreeSlots and in mActiveBuffers", slot);
                usleep(PAUSE_TIME);
            }
            if (!mSlots[slot].mBufferState.isFree()) {
                BQ_LOGE("Slot %d is in mFreeSlots but is not FREE", slot);
                usleep(PAUSE_TIME);
            }
            if (mSlots[slot].mGraphicBuffer != NULL) {
                BQ_LOGE("Slot %d is in mFreeSlots but has a buffer",
                        slot);
                usleep(PAUSE_TIME);
            }
        } else if (isInFreeBuffers) {
            if (isInUnusedSlots) {
                BQ_LOGE("Slot %d is in mFreeBuffers and in mUnusedSlots", slot);
                usleep(PAUSE_TIME);
            }
            if (isInFreeSlots) {
                BQ_LOGE("Slot %d is in mFreeBuffers and in mFreeSlots", slot);
                usleep(PAUSE_TIME);
            }
            if (isInActiveBuffers) {
                BQ_LOGE("Slot %d is in mFreeBuffers and in mActiveBuffers",
                        slot);
                usleep(PAUSE_TIME);
            }
            if (!mSlots[slot].mBufferState.isFree()) {
                BQ_LOGE("Slot %d is in mFreeBuffers but is not FREE", slot);
                usleep(PAUSE_TIME);
            }
            if (mSlots[slot].mGraphicBuffer == NULL) {
                BQ_LOGE("Slot %d is in mFreeBuffers but has no buffer", slot);
                usleep(PAUSE_TIME);
            }
        } else if (isInActiveBuffers) {
            if (isInUnusedSlots) {
                BQ_LOGE("Slot %d is in mActiveBuffers and in mUnusedSlots",
                        slot);
                usleep(PAUSE_TIME);
            }
            if (isInFreeSlots) {
                BQ_LOGE("Slot %d is in mActiveBuffers and in mFreeSlots", slot);
                usleep(PAUSE_TIME);
            }
            if (isInFreeBuffers) {
                BQ_LOGE("Slot %d is in mActiveBuffers and in mFreeBuffers",
                        slot);
                usleep(PAUSE_TIME);
            }
            if (mSlots[slot].mBufferState.isFree() &&
                    !mSlots[slot].mBufferState.isShared()) {
                BQ_LOGE("Slot %d is in mActiveBuffers but is FREE", slot);
                usleep(PAUSE_TIME);
            }
            if (mSlots[slot].mGraphicBuffer == NULL && !mIsAllocating) {
                BQ_LOGE("Slot %d is in mActiveBuffers but has no buffer", slot);
                usleep(PAUSE_TIME);
            }
        } else {
            BQ_LOGE("Slot %d isn't in any of mUnusedSlots, mFreeSlots, "
                    "mFreeBuffers, or mActiveBuffers", slot);
            usleep(PAUSE_TIME);
        }
    }

    if (allocatedSlots != getMaxBufferCountLocked()) {
        BQ_LOGE("Number of allocated slots is incorrect. Allocated = %d, "
                "Should be %d (%zu free slots, %zu free buffers, "
                "%zu activeBuffers, %zu unusedSlots)", allocatedSlots,
                getMaxBufferCountLocked(), mFreeSlots.size(),
                mFreeBuffers.size(), mActiveBuffers.size(),
                mUnusedSlots.size());
    }
}

} // namespace android
