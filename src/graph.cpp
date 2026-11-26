#include "graph.h"

#include <QPainter>
#include <QQuickWindow>
#include <QSGFlatColorMaterial>
#include <QSGGeometryNode>
#include <QSGSimpleTextureNode>

#include <algorithm>
#include <array>
#include <cmath>
#include <bit>
#include <vector>

namespace {
using Vertices = std::vector<QPointF>;
constexpr qreal warningMs = 100;
constexpr qreal criticalMs = 200;
enum Layer { Warning, Critical, Grid, Band, Loss, Range, Line, Current, Count };
using Layers = std::array<Vertices, Count>;

// The render thread reuses each layer's node, material and vertex buffer
// while the viewport moves or new samples arrive.
class GraphNode : public QSGNode {
public:
    GraphNode()
    {
        for (auto &node : layers) {
            auto *geometry = new QSGGeometry(QSGGeometry::defaultAttributes_Point2D(), 0);
            geometry->setDrawingMode(QSGGeometry::DrawTriangles);
            geometry->setVertexDataPattern(QSGGeometry::DynamicPattern);
            node = new QSGGeometryNode;
            node->setGeometry(geometry);
            node->setMaterial(new QSGFlatColorMaterial);
            node->setFlag(QSGNode::OwnsGeometry);
            node->setFlag(QSGNode::OwnsMaterial);
            appendChildNode(node);
        }
    }

    void setLayer(size_t index, const Vertices &points, const QColor &color)
    {
        auto *node = layers[index];
        auto *geometry = node->geometry();
        auto *material = static_cast<QSGFlatColorMaterial *>(node->material());
        if (material->color() != color) {
            material->setColor(color);
            node->markDirty(QSGNode::DirtyMaterial);
        }

        const int size = int(points.size());
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
        if (size > capacity[index]) {
            capacity[index] = static_cast<int>(std::bit_ceil(static_cast<unsigned>(size)));
            geometry->allocate(capacity[index]);
        }
        geometry->setVertexCount(size);
#else
        if (size != geometry->vertexCount())
            geometry->allocate(size);
#endif
        auto *vertices = geometry->vertexDataAsPoint2D();
        for (int j = 0; j < size; ++j)
            vertices[j].set(points[j].x(), points[j].y());
        geometry->markVertexDataDirty();
        node->markDirty(QSGNode::DirtyGeometry);
    }
private:
    std::array<QSGGeometryNode *, Count> layers{};
    std::array<int, Count> capacity{};
};

void rectangle(Vertices &points, qreal x1, qreal y1, qreal x2, qreal y2)
{
    points.insert(points.end(), {{x1,y1}, {x2,y1}, {x1,y2}, {x1,y2}, {x2,y1}, {x2,y2}});
}

void line(Vertices &points, QPointF a, QPointF b, qreal thickness)
{
    const QPointF d = b - a;
    const qreal length = std::hypot(d.x(), d.y());
    if (length < 0.01)
        return;
    const QPointF offset(-d.y() * thickness / (2 * length), d.x() * thickness / (2 * length));
    points.insert(points.end(), {a + offset, a - offset, b + offset, b + offset, a - offset, b - offset});
}

void dot(Vertices &points, QPointF center, qreal radius)
{
    constexpr int sides = 12;
    static const std::array<QPointF, sides + 1> unit = [] {
        std::array<QPointF, sides + 1> table{};
        for (int i = 0; i <= sides; ++i) {
            const qreal angle = i * (2 * 3.141592653589793 / sides);
            table[i] = {std::cos(angle), std::sin(angle)};
        }
        return table;
    }();
    for (int i = 0; i < sides; ++i) {
        points.insert(points.end(), {center,
            center + unit[i] * radius,
            center + unit[i + 1] * radius});
    }
}

qreal metric(const QVariantMap &sample, const QString &key)
{
    const auto it = sample.constFind(key);
    if (it == sample.cend())
        return -1;
    bool valid = false;
    const qreal value = it->toDouble(&valid);
    return valid && std::isfinite(value) ? value : -1;
}

void profileLayers(Layers &layers, const QVariantList &samples, qreal width,
                   qreal height, qreal rowHeight, qreal ceiling)
{
    static const QString kAvg = QStringLiteral("avg");
    static const QString kMin = QStringLiteral("min");
    static const QString kMax = QStringLiteral("max");
    static const QString kCur = QStringLiteral("cur");
    const size_t n = static_cast<size_t>(samples.size());
    layers[Range].reserve(n * 18);
    layers[Line].reserve(n * 42);
    layers[Current].reserve(n * 12);
    auto x = [=](qreal latency) {
        return 5 + std::clamp(latency / ceiling, 0.0, 1.0) * std::max(1.0, width - 10);
    };
    if (ceiling > warningMs)
        rectangle(layers[Warning], x(warningMs), 0, x(std::min(criticalMs, ceiling)), height);
    if (ceiling > criticalMs)
        rectangle(layers[Critical], x(criticalMs), 0, x(ceiling), height);
    for (int i = 0; i <= 4; ++i) {
        const qreal gx = x(ceiling * i / 4);
        rectangle(layers[Grid], gx, 0, gx + 1, height);
    }
    QPointF last;
    bool haveLast = false;
    for (qsizetype i = 0; i < samples.size(); ++i) {
        const auto sample = samples[i].toMap();
        const qreal avg = metric(sample, kAvg);
        if (avg < 0)
            continue;
        const qreal y = (i + 0.5) * rowHeight;
        const qreal lo = metric(sample, kMin), hi = metric(sample, kMax), cur = metric(sample, kCur);
        if (lo >= 0 && hi >= 0) {
            rectangle(layers[Range], x(lo), y - 1, x(hi) + 1, y + 1);
            rectangle(layers[Range], x(lo), y - 4, x(lo) + 1.4, y + 4);
            rectangle(layers[Range], x(hi), y - 4, x(hi) + 1.4, y + 4);
        }
        const QPointF current(x(avg), y);
        if (haveLast)
            line(layers[Line], last, current, 1.7);
        dot(layers[Line], current, 3.4);
        if (cur >= 0) {
            const QPointF p(x(cur), y);
            line(layers[Current], p - QPointF(2.6,2.6), p + QPointF(2.6,2.6), 1.3);
            line(layers[Current], p - QPointF(2.6,-2.6), p + QPointF(2.6,-2.6), 1.3);
        }
        haveLast = true;
        last = current;
    }
}

void timelineLayers(Layers &layers, const QVariantList &samples, qreal width,
                    qreal height, qreal ceiling, int pixels, qreal maxGap, qreal baselineP95)
{
    static const QString kX = QStringLiteral("x");
    static const QString kLoss = QStringLiteral("loss");
    static const QString kAvg = QStringLiteral("avg");
    static const QString kMin = QStringLiteral("min");
    static const QString kMax = QStringLiteral("max");
    const size_t n = static_cast<size_t>(samples.size());
    layers[Band].reserve(n * 6);
    layers[Range].reserve(n * 6);
    layers[Line].reserve(n * 6);
    auto y = [=](qreal rtt) { return height - 2 - std::clamp(rtt / ceiling, 0.0, 1.0) * (height - 5); };
    if (baselineP95 >= 0) {
        if (ceiling > baselineP95)
            rectangle(layers[Warning], 0, 0, width, y(baselineP95));
    } else if (ceiling > warningMs)
        rectangle(layers[Warning], 0, ceiling > criticalMs ? y(criticalMs) : 0, width, y(warningMs));
    if (baselineP95 < 0 && ceiling > criticalMs)
        rectangle(layers[Critical], 0, 0, width, y(criticalMs));
    for (int i = 0; i <= 4; ++i) {
        const qreal gx = (width - 1) * i / 4;
        rectangle(layers[Grid], gx, 0, gx + 1, height);
    }
    rectangle(layers[Grid], 0, height / 2, width, height / 2 + 1);
    rectangle(layers[Grid], 0, height - 1, width, height);
    const qreal step = width / std::max(1, pixels);
    QPointF last;
    bool haveLast = false;
    for (const auto &item : samples) {
        const auto sample = item.toMap();
        const qreal x = std::min(width - 1, (metric(sample, kX) + 0.5) * step);
        const qreal loss = metric(sample, kLoss);
        if (loss > 0) {
            const qreal amount = std::clamp(loss / 100, 0.0, 1.0);
            rectangle(layers[Loss], x - 0.5, height * (1 - amount), x + std::max(1.1, step - 0.5), height);
        }
        const qreal avg = metric(sample, kAvg);
        if (avg < 0) {
            haveLast = false;
            continue;
        }
        const qreal lo = metric(sample, kMin), hi = metric(sample, kMax);
        if (lo >= 0 && hi >= 0)
            rectangle(layers[Range], x - 0.55, y(hi), x + 0.55, y(lo) + 1);
        const QPointF current(x, y(avg));
        if (haveLast && x - last.x() <= maxGap) {
            layers[Band].insert(layers[Band].end(), {last, current, {last.x(), height},
                current, {x,height}, {last.x(),height}});
            line(layers[Line], last, current, 1.5);
        } else {
            rectangle(layers[Line], x - 0.8, current.y() - 0.8, x + 0.8, current.y() + 0.8);
        }
        last = current;
        haveLast = true;
    }
}

void diagnosticLayers(Layers &layers, const QVariantList &samples, qreal width,
                      qreal height, qreal ceiling, bool service, double startMs, double spanMs)
{
    static const QString kT = QStringLiteral("t");
    static const QString kPeriod = QStringLiteral("periodMs");
    static const QString kMarker = QStringLiteral("marker");
    static const std::array<QString, 4> serviceKeys{
        QStringLiteral("dns"), QStringLiteral("connect"), QStringLiteral("tls"), QStringLiteral("firstByte")};
    static const std::array<QString, 2> interfaceKeys{
        QStringLiteral("rx"), QStringLiteral("tx")};

    const double span = std::max(1.0, spanMs);
    for (int i = 0; i <= 4; ++i) {
        const qreal gx = (width - 1) * i / 4;
        rectangle(layers[Grid], gx, 0, gx + 1, height);
    }
    rectangle(layers[Grid], 0, height / 2, width, height / 2 + 1);

    const int seriesCount = service ? 4 : 2;
    const size_t n = static_cast<size_t>(samples.size());
    for (int s = 0; s < seriesCount; ++s)
        layers[Band + s].reserve(n * 12);

    std::array<bool, 4> haveLast{};
    std::array<double, 4> lastT{};
    std::array<QPointF, 4> lastPt{};
    const double minGap = 3.0 * span / std::max(1.0, width);

    for (const auto &item : samples) {
        const auto p = item.toMap();
        const double t = metric(p, kT);
        if (t < 0)
            continue;
        const double periodMs = std::max(0.0, metric(p, kPeriod));
        const qreal x = (t - startMs) / span * (width - 1);
        for (int s = 0; s < seriesCount; ++s) {
            const qreal v = metric(p, service ? serviceKeys[s] : interfaceKeys[s]);
            if (v < 0) {
                haveLast[s] = false;
                continue;
            }
            const qreal y = (1.0 - std::clamp(v / ceiling, 0.0, 1.0)) * (height - 3) + 1;
            const QPointF pt(x, y);
            auto &dst = layers[Band + s];
            const bool connected = haveLast[s] && (t - lastT[s]) <= std::max(periodMs * 3.0, minGap);
            if (connected)
                line(dst, lastPt[s], pt, 1.25);
            if (!connected || (pt.x() - lastPt[s].x()) >= 2.5)
                rectangle(dst, x - 1, y - 1, x + 1, y + 1);
            lastPt[s] = pt;
            lastT[s] = t;
            haveLast[s] = true;
        }
        if (const auto mit = p.constFind(kMarker); mit != p.cend() && mit->isValid()) {
            const auto marker = mit->toMap();
            const double mt = metric(marker, kT);
            if (mt >= 0) {
                const qreal mx = (mt - startMs) / span * (width - 1);
                rectangle(layers[Current], mx - 1, 0, mx + 2, 7);
            }
        }
    }
}
} // namespace

GraphView::GraphView(QQuickItem *parent) : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
    setClip(true);
}

void GraphView::setSamples(const QVariantList &v) { if (m_samples != v) { m_samples = v; emit samplesChanged(); update(); } }
void GraphView::setProfile(bool v) { if (m_profile != v) { m_profile = v; emit profileChanged(); update(); } }
void GraphView::setDiagnostic(bool v) { if (m_diagnostic != v) { m_diagnostic = v; emit diagnosticChanged(); update(); } }
void GraphView::setService(bool v) { if (m_service != v) { m_service = v; emit serviceChanged(); update(); } }
void GraphView::setStartMs(double v) { if (m_startMs != v) { m_startMs = v; emit startMsChanged(); update(); } }
void GraphView::setSpanMs(double v) { if (m_spanMs != v && v > 0) { m_spanMs = v; emit spanMsChanged(); update(); } }
void GraphView::setPixels(int v) { if (m_pixels != v) { m_pixels = v; emit pixelsChanged(); update(); } }
void GraphView::setBaselineP95(qreal value) {
    if (m_baselineP95 != value) { m_baselineP95 = value; emit baselineP95Changed(); update(); }
}
void GraphView::setCeiling(qreal v) { if (m_ceiling != v && v > 0) { m_ceiling = v; emit ceilingChanged(); update(); } }
void GraphView::setRowHeight(qreal v) { if (m_rowHeight != v) { m_rowHeight = v; emit rowHeightChanged(); update(); } }
void GraphView::setMaxGap(qreal v) { if (m_maxGap != v) { m_maxGap = v; emit maxGapChanged(); update(); } }
void GraphView::setAccent(const QColor &v) { if (m_accent != v) { m_accent = v; emit accentChanged(); update(); } }

void GraphView::geometryChange(const QRectF &now, const QRectF &before)
{
    QQuickItem::geometryChange(now, before);
    if (now.size() != before.size())
        update();
}

QSGNode *GraphView::updatePaintNode(QSGNode *old, UpdatePaintNodeData *)
{
    if (width() < 2 || height() < 3) {
        delete old;
        return nullptr;
    }

    Layers layers;
    std::array<QColor, Count> colors;
    if (m_diagnostic) {
        diagnosticLayers(layers, m_samples, width(), height(), m_ceiling, m_service, m_startMs, m_spanMs);
        colors = {Qt::transparent, Qt::transparent, QColor("#2b3946"), QColor("#82aaff"),
                  QColor("#8ad6b4"), QColor("#e6b779"), QColor("#cb9be8"), QColor("#ee9294")};
    } else {
        if (m_profile)
            profileLayers(layers, m_samples, width(), height(), m_rowHeight, m_ceiling);
        else
            timelineLayers(layers, m_samples, width(), height(), m_ceiling, m_pixels, m_maxGap, m_baselineP95);
        QColor fill = m_accent;
        fill.setAlpha(23);
        colors = {QColor(215,170,85,22), QColor(203,96,104,32), QColor("#293643"), fill,
                  QColor("#dd686e"), QColor("#8296a5"), m_accent, QColor("#f4c778")};
    }
    if (window()->rendererInterface()->graphicsApi() == QSGRendererInterface::Software) {
        // The software renderer needs a raster image for custom geometry.
        const qreal ratio = window()->effectiveDevicePixelRatio();
        QImage image(QSize(std::ceil(width() * ratio), std::ceil(height() * ratio)), QImage::Format_ARGB32_Premultiplied);
        image.setDevicePixelRatio(ratio);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        painter.setPen(Qt::NoPen);
        for (size_t i = 0; i < layers.size(); ++i) {
            const auto &layer = layers[i];
            if (layer.empty())
                continue;
            painter.setBrush(colors[i]);
            for (size_t j = 0; j < layer.size(); j += 3)
                painter.drawPolygon(&layer[j], 3);
        }
        painter.end();
        auto *texture = old ? static_cast<QSGSimpleTextureNode *>(old) : new QSGSimpleTextureNode;
        texture->setTexture(window()->createTextureFromImage(image));
        texture->setOwnsTexture(true);
        texture->setRect(boundingRect());
        return texture;
    }
    auto *root = old ? static_cast<GraphNode *>(old) : new GraphNode;
    for (size_t i = 0; i < layers.size(); ++i)
        root->setLayer(i, layers[i], colors[i]);
    return root;
}
