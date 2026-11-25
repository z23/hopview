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
    for (int i = 0; i < sides; ++i) {
        const qreal a = i * (2 * 3.141592653589793 / sides);
        const qreal b = (i + 1) * (2 * 3.141592653589793 / sides);
        points.insert(points.end(), {center,
            center + QPointF(radius * std::cos(a), radius * std::sin(a)),
            center + QPointF(radius * std::cos(b), radius * std::sin(b))});
    }
}

qreal metric(const QVariantMap &sample, const QString &key)
{
    bool valid = false;
    const qreal value = sample.value(key).toDouble(&valid);
    return valid && std::isfinite(value) ? value : -1;
}

void profileLayers(Layers &layers, const QVariantList &samples, qreal width,
                   qreal height, qreal rowHeight, qreal ceiling)
{
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
        const qreal avg = metric(sample, "avg");
        if (avg < 0)
            continue;
        const qreal y = (i + 0.5) * rowHeight;
        const qreal lo = metric(sample, "min"), hi = metric(sample, "max"), cur = metric(sample, "cur");
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
        const qreal x = std::min(width - 1, (metric(sample, "x") + 0.5) * step);
        const qreal loss = metric(sample, "loss");
        if (loss > 0) {
            const qreal amount = std::clamp(loss / 100, 0.0, 1.0);
            rectangle(layers[Loss], x - 0.5, height * (1 - amount), x + std::max(1.1, step - 0.5), height);
        }
        const qreal avg = metric(sample, "avg");
        if (avg < 0) {
            haveLast = false;
            continue;
        }
        const qreal lo = metric(sample, "min"), hi = metric(sample, "max");
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
} // namespace

GraphView::GraphView(QQuickItem *parent) : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
    setClip(true);
}

void GraphView::setSamples(const QVariantList &v) { m_samples = v; emit samplesChanged(); update(); }
void GraphView::setProfile(bool v) { if (m_profile != v) { m_profile = v; emit profileChanged(); update(); } }
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
    if (m_profile)
        profileLayers(layers, m_samples, width(), height(), m_rowHeight, m_ceiling);
    else
        timelineLayers(layers, m_samples, width(), height(), m_ceiling, m_pixels, m_maxGap, m_baselineP95);
    QColor fill = m_accent;
    fill.setAlpha(23);
    const std::array<QColor, Count> colors = {
        QColor(215,170,85,22), QColor(203,96,104,32), QColor("#293643"), fill, QColor("#dd686e"), QColor("#8296a5"), m_accent, QColor("#f4c778")
    };
    if (window()->rendererInterface()->graphicsApi() == QSGRendererInterface::Software) {
        // The software renderer needs a raster image for custom geometry.
        const qreal ratio = window()->effectiveDevicePixelRatio();
        QImage image(QSize(std::ceil(width() * ratio), std::ceil(height() * ratio)), QImage::Format_ARGB32_Premultiplied);
        image.setDevicePixelRatio(ratio);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        painter.setPen(Qt::NoPen);
        for (size_t i = 0; i < layers.size(); ++i) {
            painter.setBrush(colors[i]);
            const auto &layer = layers[i];
            for (size_t j = 0; j < layer.size(); j += 3)
                painter.drawPolygon(&layer[j], 3);
        }
        painter.end();
        delete old;
        auto *texture = new QSGSimpleTextureNode;
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
