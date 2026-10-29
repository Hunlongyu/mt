#pragma once
#include <QPointF>
#include <QString>
#include <QStringList>
#include <QVector>

namespace mt {
struct GestureMatch {
    QString key; // Accepted, normalized binding; empty means no action.
    QString directions; // Diagnostic encoding, never the matching input.
    double score = 0.0;
    double runnerUpScore = 0.0;
    bool awaitingTurn = false;
};

// Pure input-independent recognizer. Native hooks only enqueue samples.
class GestureRecognizer {
  public:
    explicit GestureRecognizer(double threshold = 20.0);
    void begin(QPointF point);
    bool append(QPointF point);
    QString directions() const;
    GestureMatch match(const QStringList& bindings, double minimumScore = 82.0,
                       double ambiguityMargin = 6.0) const;
    bool hasStroke() const {
        return !directions().isEmpty();
    }
    const QVector<QPointF>& points() const {
        return points_;
    }
    void reset();

  private:
    double threshold_;
    QVector<QPointF> points_;
    bool invalid_ = false;
};
} // namespace mt
