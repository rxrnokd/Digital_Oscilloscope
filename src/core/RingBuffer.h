// SIGNAL LAB PRO - ring buffers.
//  * SampleRing : absolute-indexed acquisition memory (single owner thread = engine).
//                 Holds the newest `capacity` samples; used for pre-trigger history,
//                 roll mode and for re-reading the record while stopped.
//  * SpscRing   : lock-free single-producer/single-consumer FIFO used to hand samples
//                 from real-time producers (audio callback) to the acquisition thread.
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace slp {

inline size_t nextPow2(size_t v) {
    size_t p = 1;
    while (p < v) p <<= 1;
    return p;
}

class SampleRing {
public:
    explicit SampleRing(size_t capacity = 16) { reset(capacity); }

    void reset(size_t capacity) {
        cap_ = nextPow2(std::max<size_t>(capacity, 16));
        mask_ = cap_ - 1;
        buf_.assign(cap_, 0.0f);
        write_ = valid_ = 0;
    }

    // Start a new contiguous run at absolute index `index`; older data is invalidated.
    void restart(int64_t index) { write_ = valid_ = index; }

    void append(const float* x, size_t n) {
        if (n >= cap_) { // only the newest cap_ samples survive
            x += n - cap_;
            write_ += static_cast<int64_t>(n - cap_);
            n = cap_;
        }
        const size_t pos = static_cast<size_t>(write_) & mask_;
        const size_t first = std::min(n, cap_ - pos);
        std::memcpy(&buf_[pos], x, first * sizeof(float));
        if (n > first) std::memcpy(&buf_[0], x + first, (n - first) * sizeof(float));
        write_ += static_cast<int64_t>(n);
    }

    int64_t writeIndex() const { return write_; }
    int64_t validFrom() const { return std::max(valid_, write_ - static_cast<int64_t>(cap_)); }
    bool contains(int64_t i) const { return i >= validFrom() && i < write_; }
    size_t capacity() const { return cap_; }
    float at(int64_t i) const { return buf_[static_cast<size_t>(i) & mask_]; }

    // Copies samples start, start+stride, ... (n values). Indices that are not held
    // in memory are returned as NaN so that callers can see gaps explicitly.
    void copy(int64_t start, size_t n, float* out, int64_t stride = 1) const {
        const int64_t lo = validFrom();
        const float nan = std::numeric_limits<float>::quiet_NaN();
        if (stride == 1) {
            size_t k = 0;
            // leading invalid part
            while (k < n && start + static_cast<int64_t>(k) < lo) out[k++] = nan;
            // valid middle part (contiguous copies, max two because of wrap-around)
            int64_t avail = write_ - (start + static_cast<int64_t>(k));
            size_t m = avail > 0 ? std::min<size_t>(n - k, static_cast<size_t>(avail)) : 0;
            while (m > 0) {
                const size_t pos = static_cast<size_t>(start + static_cast<int64_t>(k)) & mask_;
                const size_t chunk = std::min(m, cap_ - pos);
                std::memcpy(out + k, &buf_[pos], chunk * sizeof(float));
                k += chunk;
                m -= chunk;
            }
            while (k < n) out[k++] = nan;
            return;
        }
        for (size_t k = 0; k < n; ++k) {
            const int64_t i = start + static_cast<int64_t>(k) * stride;
            out[k] = (i >= lo && i < write_) ? at(i) : nan;
        }
    }

private:
    std::vector<float> buf_;
    size_t cap_ = 0, mask_ = 0;
    int64_t write_ = 0, valid_ = 0;
};

template <class T>
class SpscRing {
public:
    explicit SpscRing(size_t capacity) : buf_(nextPow2(std::max<size_t>(capacity, 2))), mask_(buf_.size() - 1) {}

    size_t capacity() const { return buf_.size(); }

    // Producer side. Returns the number of elements actually written.
    size_t push(const T* data, size_t n) {
        const size_t h = head_.load(std::memory_order_relaxed);
        const size_t t = tail_.load(std::memory_order_acquire);
        const size_t freeSpace = buf_.size() - (h - t);
        n = std::min(n, freeSpace);
        for (size_t i = 0; i < n; ++i) buf_[(h + i) & mask_] = data[i];
        head_.store(h + n, std::memory_order_release);
        return n;
    }

    // Consumer side. Returns the number of elements read.
    size_t pop(T* out, size_t n) {
        const size_t t = tail_.load(std::memory_order_relaxed);
        const size_t h = head_.load(std::memory_order_acquire);
        n = std::min(n, h - t);
        for (size_t i = 0; i < n; ++i) out[i] = buf_[(t + i) & mask_];
        tail_.store(t + n, std::memory_order_release);
        return n;
    }

    size_t available() const {
        return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire);
    }

    // Consumer side: drop everything currently queued.
    void discard() { tail_.store(head_.load(std::memory_order_acquire), std::memory_order_release); }

private:
    std::vector<T> buf_;
    size_t mask_;
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
};

} // namespace slp
