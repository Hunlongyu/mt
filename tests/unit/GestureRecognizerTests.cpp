#include "modules/gestures/GestureRecognizer.h"
#include <QtTest>
#include <cmath>
#include <limits>
#include <numbers>

using namespace mt;

namespace {
QVector<QPointF> sampledV(double angle, double noise = 0.0) {
    const double width = 200.0 / std::tan(angle * std::numbers::pi / 180.0);
    QVector<QPointF> points{{0, 0}};
    for (int arm = 0; arm < 2; ++arm)
        for (int i = 1; i <= 83; ++i) {
            const double t = i / 83.0;
            points.append({width * (arm + t) + noise * std::sin(i * 2.3),
                           200.0 * (arm == 0 ? t : 1.0 - t)});
        }
    return points;
}
GestureRecognizer recognize(const QVector<QPointF>& points) {
    GestureRecognizer recognizer;
    for (const auto point : points) recognizer.append(point);
    return recognizer;
}
const QStringList bindings{QStringLiteral("1"), QStringLiteral("2"), QStringLiteral("3"),
    QStringLiteral("4"), QStringLiteral("6"), QStringLiteral("7"), QStringLiteral("8"),
    QStringLiteral("9"), QStringLiteral("26"), QStringLiteral("39"), QStringLiteral("86"),
    QStringLiteral("28"), QStringLiteral("29"), QStringLiteral("62"), QStringLiteral("626")};

double angleFor(QChar key) {
    return QStringLiteral("63214789").indexOf(key) * 45.0;
}
QVector<QPointF> variedStroke(const QString& key, int variation) {
    QVector<QPointF> points{{0, 0}};
    QPointF origin;
    for (qsizetype arm = 0; arm < key.size(); ++arm) {
        const double angle = (angleFor(key[arm]) + (variation * 7 + arm * 11) % 25 - 12) * std::numbers::pi / 180.0;
        const double distance = 120.0 + (variation * 13 + arm * 47) % 125;
        const QPointF delta{distance * std::cos(angle), distance * std::sin(angle)};
        const QPointF normal{-std::sin(angle), std::cos(angle)};
        const int count = 13 + (variation * 3 + int(arm) * 5) % 27;
        for (int i = 1; i <= count; ++i) {
            const double t = std::pow(double(i) / count, 0.7 + (variation % 4) * 0.4);
            const double jitter = i == count ? 0.0 : 3.0 * std::sin(i * 3.7 + variation);
            points.append(origin + delta * t + normal * jitter);
        }
        origin += delta;
    }
    return points;
}
}

class GestureRecognizerTests final : public QObject {
    Q_OBJECT
  private Q_SLOTS:
    void naturalV_data() {
        QTest::addColumn<double>("angle");
        QTest::addColumn<double>("noise");
        for (int angle : {30, 45, 60, 65, 75, 80})
            for (int noise : {0, 2})
                QTest::newRow(qPrintable(QStringLiteral("%1-degrees-noise-%2").arg(angle).arg(noise)))
                    << double(angle) << double(noise);
    }
    void naturalV() {
        QFETCH(double, angle);
        QFETCH(double, noise);
        const auto recognizer = recognize(sampledV(angle, noise));
        QCOMPARE(recognizer.directions(), QStringLiteral("39"));
        const auto result = recognizer.match(bindings);
        QCOMPARE(result.key, QStringLiteral("39"));
        QVERIFY(result.score >= 82.0);
        QVERIFY(result.score - result.runnerUpScore >= 6.0);
    }
    void mirroredAndSidewaysV() {
        const QStringList keys{QStringLiteral("39"), QStringLiteral("17"), QStringLiteral("93"),
            QStringLiteral("71"), QStringLiteral("31"), QStringLiteral("97"), QStringLiteral("13"),
            QStringLiteral("79"), QStringLiteral("28"), QStringLiteral("82"), QStringLiteral("46"), QStringLiteral("64")};
        for (int orientation = 0; orientation < 8; ++orientation)
            for (int angle = 30; angle <= 80; angle += 5) {
                auto points = sampledV(angle, 2);
                for (auto& point : points) {
                    if (orientation & 1) point.setX(-point.x());
                    if (orientation & 2) point.setY(-point.y());
                    if (orientation & 4) point = {point.y(), point.x()};
                }
                QCOMPARE(recognize(points).match(keys).key, keys[orientation]);
            }
    }
    void historicalIncidentTrace() {
        // Input coordinates only, from hua's documented V regression fixture.
        // The fitting/scoring implementation is independently written in mt.
        const QVector<QPointF> points{{2546,357},{2551,375},{2562,399},{2569,419},
            {2575,430},{2579,442},{2585,453},{2593,472},{2607,498},{2617,514},
            {2623,525},{2629,536},{2637,551},{2645,562},{2654,574},{2667,591},
            {2676,601},{2684,612},{2692,621},{2694,605},{2697,590},{2703,558},
            {2709,529},{2712,517},{2717,495},{2721,479},{2727,460},{2734,436},
            {2738,422},{2743,404},{2745,392},{2750,372},{2754,360},{2758,345},
            {2762,329},{2765,317}};
        QCOMPARE(recognize(points).match(bindings).key, QStringLiteral("39"));
    }
    void retracesAndLShapesAreNotV() {
        QCOMPARE(recognize({{0,0},{10,100},{20,0}}).match(bindings).key, QStringLiteral("28"));
        QCOMPARE(recognize({{0,0},{0,200},{140,60}}).match(bindings).key, QStringLiteral("29"));
        QCOMPARE(recognize({{0,0},{10,200},{150,60}}).match(bindings).key, QStringLiteral("29"));
        QCOMPARE(recognize({{0,0},{0,120},{160,113}}).match(bindings).key, QStringLiteral("26"));
        QCOMPARE(recognize({{0,0},{0,50},{60,50}}).match(bindings).key, QStringLiteral("26"));
        QCOMPARE(recognize({{0,0},{0,200},{120,200},{120,218}}).match(bindings).key, QStringLiteral("26"));
    }
    void roundedCornerStillMatches() {
        // A rounded bottom and unequal arms, without a sample at the ideal vertex.
        const QVector<QPointF> points{{0,0},{35,55},{70,110},{100,150},{111,159},
            {120,162},{130,160},{140,152},{165,100},{195,30},{210,-10}};
        QCOMPARE(recognize(points).match(bindings).key, QStringLiteral("39"));
        // A deliberately long horizontal leg is a separate gesture, not a rounded V.
        const auto separate = recognize({{0,0},{80,140},{180,140},{260,0}});
        QVERIFY(separate.match(bindings).key.isEmpty());
        QCOMPARE(separate.match({QStringLiteral("39"),QStringLiteral("369")}).key, QStringLiteral("369"));
    }
    void unevenSamplingAnglesAndLengths() {
        const QStringList keys{QStringLiteral("1"),QStringLiteral("2"),QStringLiteral("3"),
            QStringLiteral("4"),QStringLiteral("6"),QStringLiteral("7"),QStringLiteral("8"),QStringLiteral("9"),
            QStringLiteral("26"),QStringLiteral("39"),QStringLiteral("86"),QStringLiteral("626")};
        for (const auto& key : keys)
            for (int variant = 0; variant < 24; ++variant) {
                const auto result = recognize(variedStroke(key, variant)).match(bindings);
                QVERIFY2(result.key == key, qPrintable(QStringLiteral("%1 variant %2: %3 score %4 runner-up %5")
                    .arg(key).arg(variant).arg(result.key).arg(result.score).arg(result.runnerUpScore)));
            }
    }
    void translationAndScalePreserveMeaning() {
        const auto original = variedStroke(QStringLiteral("39"), 3);
        const auto baseline = recognize(original).match(bindings);
        for (double scale : {0.7, 1.0, 1.5, 3.0}) {
            QVector<QPointF> transformed;
            for (const auto point : original) transformed.append(point * scale + QPointF(-10000, 7000));
            const auto result = recognize(transformed).match(bindings);
            QVERIFY2(result.key == baseline.key, qPrintable(QStringLiteral("scale %1: %2 (%3), score %4")
                .arg(scale).arg(result.key, result.directions).arg(result.score)));
            QVERIFY(std::abs(result.score - baseline.score) < 1.0);
        }
        // Mouse coordinates may span signed integer extrema; distance arithmetic is double.
        const double low = std::numeric_limits<int>::min(), high = std::numeric_limits<int>::max();
        QCOMPARE(recognize({{low,low},{0,0},{high,high}}).match(bindings).key, QStringLiteral("3"));
    }
    void candidateAwareTail_data() {
        QTest::addColumn<double>("firstLength");
        QTest::addColumn<double>("tail");
        QTest::addColumn<QStringList>("keys");
        QTest::addColumn<QString>("expected");
        for (int tail : {0, 19, 20, 25, 39, 40, 60}) {
            QTest::newRow(qPrintable(QStringLiteral("turn-%1").arg(tail)))
                << 200.0 << double(tail) << QStringList{QStringLiteral("2"),QStringLiteral("26")}
                << (tail < 20 ? QStringLiteral("2") : tail < 40 ? QString{} : QStringLiteral("26"));
        }
        QTest::newRow("long-prefix-pending") << 1000.0 << 60.0
            << QStringList{QStringLiteral("2"),QStringLiteral("26")} << QString{};
        QTest::newRow("unconfigured-tail") << 1000.0 << 60.0 << QStringList{QStringLiteral("2")} << QStringLiteral("2");
        QTest::newRow("opposite-tail") << 1000.0 << -60.0
            << QStringList{QStringLiteral("2"),QStringLiteral("26")} << QStringLiteral("2");
        QTest::newRow("confirmation-cap") << 1000.0 << 120.0
            << QStringList{QStringLiteral("2"),QStringLiteral("26")} << QStringLiteral("26");
    }
    void candidateAwareTail() {
        QFETCH(double, firstLength);
        QFETCH(double, tail);
        QFETCH(QStringList, keys);
        QFETCH(QString, expected);
        QCOMPARE(recognize({{0,0},{0,firstLength},{tail,firstLength}}).match(keys).key, expected);
    }
    void unfinishedVCannotExecuteDelete() {
        const auto path = recognize({{0,0},{140,140},{158,122}});
        const auto result = path.match(bindings);
        QVERIFY(result.key.isEmpty());
        QVERIFY(result.awaitingTurn);
        QCOMPARE(path.match({QStringLiteral("3")}).key, QStringLiteral("3"));
        const auto shallowTurn = recognize({{0,0},{0,1000},{15,1015}})
            .match({QStringLiteral("2"),QStringLiteral("23")});
        QVERIFY(shallowTurn.key.isEmpty());
        QVERIFY(shallowTurn.awaitingTurn);
    }
    void ambiguousLowScoreAndInvalidTemplates() {
        const auto line = [](double angle) {
            return recognize({{0,0},{200 * std::cos(angle * std::numbers::pi / 180.0),
                                   200 * std::sin(angle * std::numbers::pi / 180.0)}});
        };
        const QStringList neighbors{QStringLiteral("6"),QStringLiteral("3")};
        const auto boundary = line(22.5).match(neighbors);
        QVERIFY(boundary.key.isEmpty());
        QVERIFY(boundary.score >= 82.0);
        QVERIFY(boundary.score - boundary.runnerUpScore < 6.0);
        QCOMPARE(line(15).match(neighbors).key, QStringLiteral("6"));
        QVERIFY(line(180).match({QStringLiteral("6")}).key.isEmpty());
        QCOMPARE(line(0).match({QStringLiteral("6"),QStringLiteral("66"),QStringLiteral("5"),QString{},QStringLiteral("hello")}).key,
                 QStringLiteral("6"));
        QVERIFY(recognize({{0,0},{9,0}}).match({QStringLiteral("6")},1,0).key.isEmpty());
    }
    void narrowVConfidenceIsContinuous() {
        double previous = -1;
        for (int width = 24; width <= 56; ++width) {
            const auto result = recognize({{0,0},{width / 2.0,100},{double(width),0}})
                .match({QStringLiteral("39")}, 0, 0);
            if (previous >= 0) QVERIFY2(std::abs(result.score - previous) <= 3.0, qPrintable(QString::number(width)));
            previous = result.score;
        }
    }
    void overflowNonFiniteAndComplexityRejectTheWholePath() {
        GestureRecognizer recognizer;
        for (int i = 0; i <= 4096; ++i) recognizer.append({double(i * 2),0});
        QVERIFY(recognizer.match(bindings).key.isEmpty());
        QVERIFY(recognizer.directions().isEmpty());
        QCOMPARE(recognizer.points().size(), qsizetype(4096));
        recognizer.begin({0,0}); recognizer.append({100,0});
        QCOMPARE(recognizer.match(bindings).key, QStringLiteral("6"));
        recognizer.append({std::numeric_limits<double>::quiet_NaN(),0});
        QVERIFY(recognizer.match(bindings).key.isEmpty());
        recognizer.begin({0,0});
        for (int i = 1; i < 100; ++i) recognizer.append({double(i * 50), (i % 2) * 100.0});
        QVERIFY(recognizer.match(bindings).key.isEmpty());
        QVERIFY(recognizer.directions().isEmpty());
    }
    void boundedLargeInputAndCatalog() {
        QStringList catalog = bindings;
        for (int value = 0; catalog.size() < 192; ++value) {
            int number = value;
            QString key;
            for (int j = 0; j < 7; ++j) { key += QStringLiteral("63214789")[number % 8]; number /= 8; }
            catalog.append(key);
        }
        GestureRecognizer recognizer;
        for (int i = 0; i < 4096; ++i) recognizer.append({double(i * 2),2.0 * std::sin(i * 1.7)});
        QElapsedTimer timer;
        timer.start();
        for (int i = 0; i < 100; ++i) QCOMPARE(recognizer.match(catalog).key, QStringLiteral("6"));
        qInfo() << "4096 samples / 192 bindings / 100 matches:" << timer.elapsed() << "ms";
        QVERIFY(timer.elapsed() < 2000);
    }
};

QTEST_GUILESS_MAIN(GestureRecognizerTests)
#include "GestureRecognizerTests.moc"
