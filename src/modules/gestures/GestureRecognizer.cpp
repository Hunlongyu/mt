#include "GestureRecognizer.h"
#include "GestureRules.h"
#include <QSet>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>

namespace mt {
namespace {
constexpr double radians = std::numbers::pi / 180.0;
constexpr qsizetype maximumSamples = 4096;
constexpr qsizetype maximumCorners = 64;
using Legs = QVector<QPointF>;

double length(QPointF p) { return std::hypot(p.x(), p.y()); }
double dot(QPointF a, QPointF b) { return a.x() * b.x() + a.y() * b.y(); }
double heading(QPointF p) { return std::atan2(p.y(), p.x()) / radians; }
double angleDifference(double a, double b) { return std::abs(std::remainder(a - b, 360.0)); }
double ramp(double x, double low, double high) { return std::clamp((x - low) / (high - low), 0.0, 1.0); }
double turnDistance(double previousLength, double threshold) {
    return std::clamp(previousLength * 0.2, threshold, threshold * 6.0);
}
QChar digit(QPointF vector) {
    constexpr char directions[] = {'6', '3', '2', '1', '4', '7', '8', '9'};
    const auto sector = (int(std::lround(heading(vector) / 45.0)) + 8) % 8;
    return QLatin1Char(directions[sector]);
}
QPointF unit(QChar direction) {
    constexpr double diagonal = std::numbers::sqrt2 / 2.0;
    switch (direction.unicode()) {
    case '1': return {-diagonal, diagonal};
    case '2': return {0, 1};
    case '3': return {diagonal, diagonal};
    case '4': return {-1, 0};
    case '6': return {1, 0};
    case '7': return {-diagonal, -diagonal};
    case '8': return {0, -1};
    case '9': return {diagonal, -diagonal};
    default: return {};
    }
}

struct Path {
    Legs legs;
    QPointF pending;
};

// Fit whole straight sections before estimating headings. This avoids a sector
// flip at every noisy sample and the spurious horizontal step across a V's tip.
Path fitPath(const QVector<QPointF>& points, double threshold) {
    if (points.size() < 2) return {};
    QVector<qsizetype> corners{0, points.size() - 1};
    QVector<QPair<qsizetype, qsizetype>> ranges{{0, points.size() - 1}};
    QPointF minimum = points.front(), maximum = minimum;
    for (const auto point : points) {
        minimum.setX(std::min(minimum.x(), point.x()));
        minimum.setY(std::min(minimum.y(), point.y()));
        maximum.setX(std::max(maximum.x(), point.x()));
        maximum.setY(std::max(maximum.y(), point.y()));
    }
    const double tolerance = std::clamp(length(maximum - minimum) * 0.025, threshold * 0.3, threshold * 0.9);
    const double toleranceSquared = tolerance * tolerance;
    while (!ranges.isEmpty()) {
        const auto [first, last] = ranges.takeLast();
        const auto start = points[first];
        const auto line = points[last] - start;
        const auto lineSquared = dot(line, line);
        double farthest = toleranceSquared;
        qsizetype split = -1;
        for (qsizetype i = first + 1; i < last; ++i) {
            const auto offset = points[i] - start;
            const auto t = lineSquared > 0 ? std::clamp(dot(offset, line) / lineSquared, 0.0, 1.0) : 0.0;
            const auto residual = offset - line * t;
            const auto distance = dot(residual, residual);
            if (distance > farthest) { farthest = distance; split = i; }
        }
        if (split < 0) continue;
        // Bounded work even for malicious/degenerate zigzags. Never recognize a
        // truncated prefix of an over-complex path.
        if (corners.size() == maximumCorners) return {};
        corners.append(split);
        ranges.append({first, split});
        ranges.append({split, last});
    }
    std::ranges::sort(corners);
    Path path;
    for (qsizetype i = 1; i < corners.size(); ++i) {
        const auto leg = points[corners[i]] - points[corners[i - 1]];
        if (length(leg) > 0) path.legs.append(leg);
    }
    // Absorb small startup hooks, rounded corner bridges and shallow kinks.
    // A final unconfirmed turn is retained separately for candidate-aware refusal.
    for (qsizetype i = 0; i + 1 < path.legs.size();) {
        const auto required = i == 0 ? threshold : turnDistance(length(path.legs[i - 1]), threshold);
        if (length(path.legs[i]) < required ||
            angleDifference(heading(path.legs[i]), heading(path.legs[i + 1])) < 25.0) {
            path.legs[i + 1] += path.legs[i];
            path.legs.removeAt(i);
            if (i > 0) --i;
        } else ++i;
    }
    const bool discardedTail = !path.legs.isEmpty() && length(path.legs.last()) < threshold;
    if (discardedTail)
        path.legs.removeLast();
    if (path.legs.size() > 1 && length(path.legs.last()) < turnDistance(length(path.legs[path.legs.size() - 2]), threshold))
        path.pending = path.legs.takeLast();
    // Simplification can hide a short 45-degree turn after a very long arm.
    // Inspect the last minimum-distance span too, so scaling noise tolerance up
    // cannot accidentally execute a prefix of a configured longer gesture.
    if (!discardedTail && length(path.pending) == 0 && !path.legs.isEmpty()) {
        qsizetype start = points.size() - 2;
        while (start > 0 && length(points.last() - points[start]) < threshold) --start;
        const auto tail = points.last() - points[start];
        const auto prefix = path.legs.last() - tail;
        const double tailLength = length(tail), prefixLength = length(prefix);
        if (prefixLength >= threshold && tailLength >= threshold &&
            tailLength < turnDistance(prefixLength, threshold) &&
            angleDifference(heading(prefix), heading(tail)) >= 30.0) {
            bool straight = true;
            for (qsizetype i = start + 1; i + 1 < points.size(); ++i) {
                const auto delta = points[i] - points[start];
                const auto t = std::clamp(dot(delta, tail) / dot(tail, tail), 0.0, 1.0);
                if (length(delta - tail * t) > threshold * 0.2) { straight = false; break; }
            }
            if (straight) { path.legs.last() = prefix; path.pending = tail; }
        }
    }
    return path;
}

Legs roundedCorners(Legs legs) {
    const auto cross = [](QPointF a, QPointF b) { return a.x() * b.y() - a.y() * b.x(); };
    for (qsizetype i = 1; i + 1 < legs.size();) {
        const auto a = legs[i - 1], bridge = legs[i], b = legs[i + 1];
        const double bridgeLength = length(bridge), denominator = cross(a, b);
        if (bridgeLength > 0.3 * std::min(length(a), length(b)) ||
            std::abs(denominator) < 0.01 * length(a) * length(b) ||
            cross(a, bridge) * cross(bridge, b) <= 0) { ++i; continue; }
        const double first = cross(a + bridge, b) / denominator;
        const double second = cross(a + bridge, a) / denominator;
        // Intersect the supporting lines only when both extensions lie near the
        // small curved bridge. Preserve the original three-leg hypothesis too.
        if (first < 1 || second > 0 || (first - 1) * length(a) > bridgeLength * 1.5 ||
            -second * length(b) > bridgeLength * 1.5) { ++i; continue; }
        legs[i - 1] = a * first;
        legs[i + 1] = b * (1 - second);
        legs.removeAt(i);
    }
    return legs;
}

struct Chevron {
    QString key;
    double confidence = 0;
};
// A V may be stretched horizontally or vertically. Measure evidence for two
// advancing arms and one reversal, continuously, rather than snapping both arms
// to 45 degrees or turning a near-vertical retrace into a V at a hard cutoff.
Chevron chevron(const Legs& legs, double threshold) {
    if (legs.size() != 2) return {};
    const auto a = legs[0], b = legs[1];
    const bool sameX = a.x() * b.x() > 0, sameY = a.y() * b.y() > 0;
    const bool reverseX = a.x() * b.x() < 0, reverseY = a.y() * b.y() < 0;
    if (!(sameX && reverseY) && !(sameY && reverseX)) return {};
    const double advanceA = std::abs(sameX ? a.x() : a.y());
    const double advanceB = std::abs(sameX ? b.x() : b.y());
    const double reverseA = std::abs(sameX ? a.y() : a.x());
    const double reverseB = std::abs(sameX ? b.y() : b.x());
    const double depth = std::max(reverseA, reverseB);
    const double confidence = std::min({
        ramp(std::min(advanceA, advanceB) / threshold, 0.4, 1.5),
        ramp(std::min(reverseA, reverseB) / threshold, 0.5, 2.0),
        ramp((advanceA + advanceB) / depth, 0.12, 0.34),
        ramp(std::min(reverseA, reverseB) / depth, 0.15, 0.6)});
    const auto quadrant = [](QPointF p) {
        return p.x() > 0 ? (p.y() > 0 ? QLatin1Char('3') : QLatin1Char('9'))
                         : (p.y() > 0 ? QLatin1Char('1') : QLatin1Char('7'));
    };
    QString key;
    key += quadrant(a);
    key += quadrant(b);
    return {key, confidence};
}
QString encode(const Legs& legs, double threshold) {
    const auto v = chevron(legs, threshold);
    if (v.confidence >= 0.5) return v.key;
    QString text;
    for (const auto leg : legs) {
        const auto next = digit(leg);
        if (text.isEmpty() || text.back() != next) text += next;
    }
    return text;
}

// Arc-length sampling removes the input device's sampling density. Relative leg
// lengths are fitted to each template; translation/size may vary, rotation may not.
constexpr size_t shapeSamples = 32;
std::array<QPointF, shapeSamples> shape(const Legs& legs) {
    std::array<QPointF, shapeSamples> samples{};
    double perimeter = 0;
    for (const auto leg : legs) perimeter += length(leg);
    if (perimeter == 0) return samples;
    qsizetype index = 0;
    double traveled = 0;
    QPointF start, center;
    for (size_t i = 0; i < samples.size(); ++i) {
        const double distance = perimeter * double(i) / double(samples.size() - 1);
        while (index + 1 < legs.size() && traveled + length(legs[index]) < distance) {
            traveled += length(legs[index]);
            start += legs[index++];
        }
        samples[i] = (start + legs[index] * ((distance - traveled) / length(legs[index]))) / perimeter;
        center += samples[i];
    }
    center /= double(samples.size());
    for (auto& sample : samples) sample -= center;
    return samples;
}
double similarity(const Legs& legs, const QString& key, double threshold) {
    if (legs.isEmpty() || legs.size() != key.size()) return 0;
    Legs expected;
    double headings = 0, turns = 0;
    for (qsizetype i = 0; i < legs.size(); ++i) {
        expected.append(unit(key[i]) * length(legs[i]));
        headings += angleDifference(heading(legs[i]), heading(expected[i]));
        if (i > 0)
            turns += angleDifference(heading(legs[i]) - heading(legs[i - 1]),
                                     heading(expected[i]) - heading(expected[i - 1]));
    }
    headings /= double(legs.size());
    if (legs.size() > 1) turns /= double(legs.size() - 1);
    const auto observedShape = shape(legs), expectedShape = shape(expected);
    double displacement = 0;
    for (size_t i = 0; i < shapeSamples; ++i)
        displacement += length(observedShape[i] - expectedShape[i]);
    double score = 100.0 - 0.42 * headings - 0.08 * turns - 18.0 * displacement / double(shapeSamples);
    const auto v = chevron(legs, threshold);
    if (v.key == key) score += (100.0 - score) * 0.96 * v.confidence;
    else score -= 20.0 * v.confidence;
    return std::clamp(score, 0.0, 100.0);
}
}

GestureRecognizer::GestureRecognizer(double threshold)
    : threshold_(std::isfinite(threshold) ? std::clamp(threshold, 5.0, 200.0) : 20.0) {}
void GestureRecognizer::reset() {
    points_.clear();
    invalid_ = false;
}
void GestureRecognizer::begin(QPointF point) {
    reset();
    append(point);
}
bool GestureRecognizer::append(QPointF point) {
    // Windows supplies signed 32-bit physical coordinates. Reject non-finite
    // or out-of-domain input before any distance/score calculation.
    constexpr double coordinateLimit = double(std::numeric_limits<int>::max()) + 1.0;
    if (!std::isfinite(point.x()) || !std::isfinite(point.y()) ||
        std::abs(point.x()) > coordinateLimit || std::abs(point.y()) > coordinateLimit) invalid_ = true;
    if (invalid_) return false;
    if (!points_.isEmpty() && length(point - points_.last()) < 1.0) return false;
    if (points_.size() >= maximumSamples) { invalid_ = true; return false; }
    points_.append(point);
    return points_.size() > 1;
}
QString GestureRecognizer::directions() const {
    return invalid_ ? QString{} : encode(fitPath(points_, threshold_).legs, threshold_);
}
GestureMatch GestureRecognizer::match(const QStringList& bindings, double minimumScore, double ambiguityMargin) const {
    GestureMatch result;
    if (invalid_ || bindings.size() > 256 || !std::isfinite(minimumScore) || !std::isfinite(ambiguityMargin)) return result;
    const auto path = fitPath(points_, threshold_);
    const auto rounded = roundedCorners(path.legs);
    result.directions = encode(path.legs, threshold_);
    if (path.legs.isEmpty()) return result;
    minimumScore = std::clamp(minimumScore, 0.0, 100.0);
    ambiguityMargin = std::clamp(ambiguityMargin, 0.0, 100.0);
    QString best;
    QSet<QString> seen;
    QStringList candidates;
    for (const auto& binding : bindings) {
        auto key = normalizeGesture(binding);
        if (!key || seen.contains(*key)) continue;
        seen.insert(*key);
        candidates.append(*key);
        const auto directScore = similarity(path.legs, *key, threshold_);
        const auto roundedScore = rounded.size() != path.legs.size()
            ? std::max(0.0, similarity(rounded, *key, threshold_) - 4.0) : 0.0;
        const auto score = std::max(directScore, roundedScore);
        if (score > result.score) {
            result.runnerUpScore = result.score;
            result.score = score;
            best = *key;
        } else result.runnerUpScore = std::max(result.runnerUpScore, score);
    }
    if (length(path.pending) > 0) {
        auto extended = path.legs;
        extended.append(path.pending);
        auto extendedRounded = rounded;
        extendedRounded.append(path.pending);
        for (const auto& key : candidates) {
            // A real but unconfirmed tail pointing toward a configured longer
            // gesture must not fire its short prefix. An unrelated hook may be ignored.
            const bool directContinuation = key.size() >= extended.size() &&
                similarity(extended, key.first(extended.size()), threshold_) >= minimumScore - 8.0;
            const bool roundedContinuation = key.size() >= extendedRounded.size() &&
                similarity(extendedRounded, key.first(extendedRounded.size()), threshold_) >= minimumScore - 8.0;
            if (directContinuation || roundedContinuation) {
                result.awaitingTurn = true;
                return result;
            }
        }
    }
    if (result.score >= minimumScore && result.score - result.runnerUpScore >= ambiguityMargin)
        result.key = best;
    return result;
}
} // namespace mt
