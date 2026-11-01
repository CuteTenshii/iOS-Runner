#include <time.h>

#include <cmath>
#include <cstdint>
#include <cstring>

#include "uikit_internal.h"

namespace uikit {

namespace {

struct Point {
    double x, y;
};
struct Size {
    double width, height;
};
struct Rect {
    double x, y, width, height;
};
struct Transform {
    double a, b, c, d, tx, ty;
};

const double kEdgeInsetsZero[4] = {0, 0, 0, 0};
const Size kFittingCompressedSize = {0, 0};
const double kNoIntrinsicMetric = -1.0;
const double kAutomaticDimension = -1.0;
const uint64_t kBackgroundTaskInvalid = 0;
const double kFontWeightRegular = 0.0, kFontWeightMedium = 0.23, kFontWeightSemibold = 0.3, kFontWeightBold = 0.4;
const uint64_t kTraitButton = 1 << 0, kTraitNotEnabled = 1 << 8, kTraitUpdatesFrequently = 1 << 10, kTraitHeader = 1 << 16;
const double kFoundationVersion = 1953.1;
const double kAbsoluteTimeSince1970 = 978307200.0;
void* const kDefaultAllocator = nullptr;

Rect normalized(Rect rect) {
    if (rect.width < 0) {
        rect.x += rect.width;
        rect.width = -rect.width;
    }
    if (rect.height < 0) {
        rect.y += rect.height;
        rect.height = -rect.height;
    }
    return rect;
}

// Multiplies with `first` applied before `second`, as CGAffineTransformConcat does.
Transform concat(const Transform& first, const Transform& second) {
    return {first.a * second.a + first.b * second.c,
            first.a * second.b + first.b * second.d,
            first.c * second.a + first.d * second.c,
            first.c * second.b + first.d * second.d,
            first.tx * second.a + first.ty * second.c + second.tx,
            first.tx * second.b + first.ty * second.d + second.ty};
}

double media_time() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<double>(now.tv_sec) + static_cast<double>(now.tv_nsec) / 1e9;
}

}  // namespace

void add_constant_symbols(SymbolMap& table) {
    table.insert({
        {"UIEdgeInsetsZero", const_cast<double*>(kEdgeInsetsZero)},
        {"NSDirectionalEdgeInsetsZero", const_cast<double*>(kEdgeInsetsZero)},
        {"UILayoutFittingCompressedSize", const_cast<Size*>(&kFittingCompressedSize)},
        {"UIViewNoIntrinsicMetric", const_cast<double*>(&kNoIntrinsicMetric)},
        {"UITableViewAutomaticDimension", const_cast<double*>(&kAutomaticDimension)},
        {"UIBackgroundTaskInvalid", const_cast<uint64_t*>(&kBackgroundTaskInvalid)},
        {"UIFontWeightRegular", const_cast<double*>(&kFontWeightRegular)},
        {"UIFontWeightMedium", const_cast<double*>(&kFontWeightMedium)},
        {"UIFontWeightSemibold", const_cast<double*>(&kFontWeightSemibold)},
        {"UIFontWeightBold", const_cast<double*>(&kFontWeightBold)},
        {"UIAccessibilityTraitButton", const_cast<uint64_t*>(&kTraitButton)},
        {"UIAccessibilityTraitNotEnabled", const_cast<uint64_t*>(&kTraitNotEnabled)},
        {"UIAccessibilityTraitUpdatesFrequently", const_cast<uint64_t*>(&kTraitUpdatesFrequently)},
        {"UIAccessibilityTraitHeader", const_cast<uint64_t*>(&kTraitHeader)},
        {"NSFoundationVersionNumber", const_cast<double*>(&kFoundationVersion)},
        {"kCFAbsoluteTimeIntervalSince1970", const_cast<double*>(&kAbsoluteTimeSince1970)},
        {"kCFAllocatorDefault", const_cast<void**>(&kDefaultAllocator)},
        {"CACurrentMediaTime", reinterpret_cast<void*>(media_time)},
        {"UIAccessibilityPostNotification", reinterpret_cast<void*>(+[](uint32_t, void*) {})},
        {"CGRectGetMinX", reinterpret_cast<void*>(+[](Rect r) -> double { return normalized(r).x; })},
        {"CGRectGetMinY", reinterpret_cast<void*>(+[](Rect r) -> double { return normalized(r).y; })},
        {"CGRectGetMidX", reinterpret_cast<void*>(+[](Rect r) -> double { r = normalized(r); return r.x + r.width / 2; })},
        {"CGRectGetMidY", reinterpret_cast<void*>(+[](Rect r) -> double { r = normalized(r); return r.y + r.height / 2; })},
        {"CGRectGetMaxX", reinterpret_cast<void*>(+[](Rect r) -> double { r = normalized(r); return r.x + r.width; })},
        {"CGRectGetMaxY", reinterpret_cast<void*>(+[](Rect r) -> double { r = normalized(r); return r.y + r.height; })},
        {"CGRectGetWidth", reinterpret_cast<void*>(+[](Rect r) -> double { return normalized(r).width; })},
        {"CGRectGetHeight", reinterpret_cast<void*>(+[](Rect r) -> double { return normalized(r).height; })},
        {"CGRectOffset", reinterpret_cast<void*>(+[](Rect r, double dx, double dy) -> Rect {
             r = normalized(r);
             return {r.x + dx, r.y + dy, r.width, r.height};
         })},
        {"CGRectContainsPoint", reinterpret_cast<void*>(+[](Rect r, Point p) -> bool {
             r = normalized(r);
             return p.x >= r.x && p.x < r.x + r.width && p.y >= r.y && p.y < r.y + r.height;
         })},
        {"CGAffineTransformMakeScale", reinterpret_cast<void*>(+[](double sx, double sy) -> Transform { return {sx, 0, 0, sy, 0, 0}; })},
        {"CGAffineTransformScale", reinterpret_cast<void*>(+[](Transform t, double sx, double sy) -> Transform { return concat({sx, 0, 0, sy, 0, 0}, t); })},
        {"CGAffineTransformTranslate", reinterpret_cast<void*>(+[](Transform t, double dx, double dy) -> Transform { return concat({1, 0, 0, 1, dx, dy}, t); })},
        {"CGAffineTransformRotate", reinterpret_cast<void*>(+[](Transform t, double angle) -> Transform {
             const double c = cos(angle), s = sin(angle);
             return concat({c, s, -s, c, 0, 0}, t);
         })},
    });
}

}  // namespace uikit
