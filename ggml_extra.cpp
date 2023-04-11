#include "ggml_extra.h"

#include <vector>
#include <utility>
#include <algorithm>
#include <cassert>
#include <thread>
#include <atomic>
#include <cstring>

namespace {

inline int toNearestInt(float fval) {
    assert(fval <= 4194303.f);
    constexpr float kSnapper=3<<22;
    auto val = fval + kSnapper;
    int i; std::memcpy(&i, &val, sizeof(int));
    return (i & 0x007fffff) - 0x00400000;
}

float kQuantize0(int n, const float* X, int8_t* L, std::vector<std::pair<float,int>>& work, int nmin, int nmax) {
    work.clear();
    work.reserve(n*(nmax+2));
    float max = 0; int imax = -1;
    for (int i=0; i<n; ++i) {
        float x = std::abs(X[i]);
        if (x > max) { max = x; imax = i; }
    }
    if (imax < 0) {  // all X are zero
        for (int i=0; i<n; ++i) L[i] = 0;
        return 1.f;
    }
    float maxi = 1/max;
    int kmin, kmax;
    {
        float scale0 = nmax*maxi;
        double sumlx0 = 0; int suml20 = 0;
        for (int i=0; i<n; ++i) {
            int l = std::max(nmin, std::min(nmax, toNearestInt(scale0*X[i])));
            sumlx0 += X[i]*l; suml20 += l*l;
        }
        auto df0 = suml20/scale0 - sumlx0;
        if (df0 > 0) {
            kmin = nmax-2; kmax = nmax + 1;
        } else {
            kmin = nmax/2; kmax = nmax+1;
        }
    }
    for (int k=kmin; k<=kmax; ++k) work.push_back({(k + 0.501f)*maxi, imax});
    float minScale = work.front().first;
    float maxScale = work.back().first;
    for (int i=0; i<n; ++i) {
        L[i] = 0;
        auto x = std::abs(X[i]);
        if (i == imax || !x) continue;
        int kkmin = std::max(0, int(minScale*x-0.501f));
        int kkmax = std::min(kmax, int(maxScale*x));
        auto xi = 1/x;
        for (int k=kkmin; k<=kkmax; ++k) {
            auto s = (k + 0.501f)*xi;
            if (s > maxScale) break;
            if (s > minScale) work.push_back({s,i});
        }
    }
    std::sort(work.begin(), work.end());
    float sumlx = 0; int suml2 = 0;
    float s = work.front().first;
    for (int i=0; i<n; ++i) {
        int l = std::max(nmin, std::min(nmax, toNearestInt(s*X[i])));
        sumlx += X[i]*l; suml2 += l*l;
        L[i] = l;
    }
    float bestSumlx = sumlx, bestSumlx2 = sumlx*sumlx; int bestSuml2 = suml2; float bests = s;
    float lasts = s;
    for (int k=1; k<int(work.size()); ++k) {
        s = work[k].first; int i = work[k].second;
        int l = std::max(nmin, std::min(nmax, toNearestInt(s*X[i])));
        if (l == L[i]) { lasts = s; continue; }
        if (l > L[i]) {
            sumlx += X[i];
            suml2 += 1 + 2*L[i];
        }
        else {
            sumlx -= X[i];
            suml2 += 1 - 2*L[i];
        }
        L[i] = l;
        float sumlx2 = sumlx*sumlx;
        if ((s != lasts || k == int(work.size())-1) && suml2 > 0 && sumlx2*bestSuml2 > bestSumlx2*suml2) {
            bestSumlx = sumlx; bestSumlx2 = sumlx2; bestSuml2 = suml2; bests = s;
        }
        lasts = s;
    }
    for (int i=0; i<n; ++i) L[i] = std::max(nmin, std::min(nmax, toNearestInt(bests*X[i])));
    return bestSumlx/bestSuml2;
}

std::pair<float, float> kQuantize1(int n, const float* X, int8_t* L, std::vector<float>& tmpX,
        std::vector<std::pair<float,int>>& work, int nmax) {
    float min = X[0], max = X[1];
    for (int i=1; i<n; ++i) {
        min = std::min(min, X[i]); max = std::max(max, X[i]);
    }
    if (max == min) {
        for (int i=0; i<n; ++i) L[i] = 0;
        return {min, 1.f};
    }
    if (int(tmpX.size()) < n) tmpX.resize(n);
    double a = min, b;
    for (int itry=0; itry<3; ++itry) {
        for (int i=0; i<n; ++i) tmpX[i] = X[i] - a;
        kQuantize0(n, tmpX.data(), L, work, 0, 2*nmax+1);
        double sumlx = 0, sumx = 0;
        int suml2 = 0, suml = 0;
        for (int i=0; i<n; ++i) {
            auto l = L[i];
            sumlx += X[i]*l;
            suml2 += l*l;
            suml  += l;
            sumx  += X[i];
        }
        int64_t D = suml2*n - suml*suml;
        a = (sumx*suml2 - sumlx*suml)/D;
        b = (sumlx*n - sumx*suml)/D;
    }
    return {a, b};
}

void kQuantizeQ4(const float* GGML_RESTRICT x, void* GGML_RESTRICT buffer, int k, int type) {
    constexpr int kChunkSize = 32*32*8;
    constexpr int QK = 32;
    constexpr int kBucketSize0 = QK/2 + sizeof(float);
    constexpr int kBucketSize1 = QK/2 + 2*sizeof(float);
    assert(k % QK == 0);

    auto processOne = [type] (const float* X, int8_t* L, char* y, std::vector<std::pair<float, int>>& work, std::vector<float>& tmpX) {
        if (type == 0) {
            float scale = kQuantize0(QK, X, L, work, -7, 7);
            std::memcpy(y, &scale, sizeof(scale)); y += sizeof(scale);
            uint8_t* q = (uint8_t*)y;
            for (int k=0; k<QK/2; ++k) q[k] = (L[2*k] + 8) | ((L[2*k+1] + 8) << 4);
        } else {
            auto result = kQuantize1(QK, X, L, tmpX, work, 7);
            std::memcpy(y, &result.second, sizeof(result.second)); y += sizeof(result.second);
            std::memcpy(y, &result.first,  sizeof(result.first));  y += sizeof(result.first);
            uint8_t* q = (uint8_t*)y;
            for (int k=0; k<QK/2; ++k) q[k] = L[2*k] | (L[2*k+1] << 4);
        }
    };

    auto bucketSize = type == 0 ? kBucketSize0 : kBucketSize1;
    auto y = (char*)buffer;
    int nchunk = (k + kChunkSize-1)/kChunkSize;
    if (nchunk < 2) {
        std::vector<int8_t> L(QK);
        std::vector<std::pair<float,int>> work;
        std::vector<float> tmpX;
        int nb = k / QK;
        for (int i=0; i<nb; ++i) {
            processOne(x + QK*i, L.data(), y, work, tmpX);
            y += bucketSize; x += QK;
        }
        return;
    }

    std::atomic<int> counter(0);
    auto compute = [&counter, x, y, k, bucketSize, &processOne] () {
        std::vector<int8_t> L(QK);
        std::vector<std::pair<float,int>> work;
        std::vector<float> tmpX;
        while (true) {
            int first = counter.fetch_add(kChunkSize);
            if (first >= k) break;
            int last = first + kChunkSize;
            if (last > k) last = k;
            auto xi = x + first;
            auto yi = y + (first/QK)*bucketSize;
            int n = (last - first)/QK;
            for (int i=0; i<n; ++i) {
                processOne(xi, L.data(), yi, work, tmpX);
                yi += bucketSize; xi += QK;
            }
        }
    };
    int nthread = std::min(nchunk, int(std::thread::hardware_concurrency()));
    std::vector<std::thread> workers(nthread-1);
    for (auto& w : workers) w = std::thread(compute);
    compute();
    for (auto& w : workers) w.join();
}

}

extern "C" {

void kQuantizeQ4_0(const float* x, void* buffer, int k) {
    kQuantizeQ4(x, buffer, k, 0);
}

void kQuantizeQ4_1(const float* x, void* buffer, int k) {
    kQuantizeQ4(x, buffer, k, 1);
}

}