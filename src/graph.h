#pragma once

#include <QColor>
#include <QQuickItem>
#include <QVariantList>

// Both modes consume an already bounded snapshot. The graph never accesses storage.
class GraphView : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(QVariantList samples READ samples WRITE setSamples NOTIFY samplesChanged)
    Q_PROPERTY(bool profile READ profile WRITE setProfile NOTIFY profileChanged)
    Q_PROPERTY(int pixels READ pixels WRITE setPixels NOTIFY pixelsChanged)
    Q_PROPERTY(qreal baselineP95 READ baselineP95 WRITE setBaselineP95 NOTIFY baselineP95Changed)
    Q_PROPERTY(qreal ceiling READ ceiling WRITE setCeiling NOTIFY ceilingChanged)
    Q_PROPERTY(qreal rowHeight READ rowHeight WRITE setRowHeight NOTIFY rowHeightChanged)
    Q_PROPERTY(qreal maxGap READ maxGap WRITE setMaxGap NOTIFY maxGapChanged)
    Q_PROPERTY(QColor accent READ accent WRITE setAccent NOTIFY accentChanged)
public:
    explicit GraphView(QQuickItem *parent = nullptr);
    QVariantList samples() const { return m_samples; }
    bool profile() const { return m_profile; }
    int pixels() const { return m_pixels; }
    qreal ceiling() const { return m_ceiling; }
    qreal rowHeight() const { return m_rowHeight; }
    qreal maxGap() const { return m_maxGap; }
    QColor accent() const { return m_accent; }
    void setSamples(const QVariantList &samples);
    void setProfile(bool profile);
    void setPixels(int pixels);
    qreal baselineP95() const { return m_baselineP95; }
    void setBaselineP95(qreal value);
    void setCeiling(qreal ceiling);
    void setRowHeight(qreal height);
    void setMaxGap(qreal gap);
    void setAccent(const QColor &accent);
signals:
    void samplesChanged();
    void profileChanged();
    void pixelsChanged();
    void ceilingChanged();
    void baselineP95Changed();
    void rowHeightChanged();
    void maxGapChanged();
    void accentChanged();
protected:
    QSGNode *updatePaintNode(QSGNode *old, UpdatePaintNodeData *) override;
    void geometryChange(const QRectF &geometry, const QRectF &oldGeometry) override;
private:
    QVariantList m_samples;
    bool m_profile = false;
    int m_pixels = 1;
    qreal m_baselineP95 = -1;
    qreal m_ceiling = 100;
    qreal m_rowHeight = 30;
    qreal m_maxGap = 2;
    QColor m_accent = QColor("#58d5af");
};
