pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts

C.Dialog {
    id: dialog
    objectName: "baselineDialog"
    property real uiScale: 1
    modal: true; focus: true
    anchors.centerIn: parent
    scale: uiScale
    width: Math.min(870, parent.width / uiScale - 24)
    height: Math.min(565, parent.height / uiScale - 24)
    padding: 16
    property var results: []
    property string baselineRange: ""
    property string incidentRange: ""
    property string message: ""
    property int sourceIndex: 0
    readonly property bool compact: availableWidth < 610
    readonly property real metricWidth: (availableWidth - 162) / 6
    readonly property var metrics: [qsTr("Replies / sent"), qsTr("Median"), qsTr("P95"), qsTr("Loss"), qsTr("Timeout streak"), qsTr("Jitter")]
    function valueText(stats, index) {
        if (index === 0) return stats.received + "/" + stats.sent
        if (index === 1 || index === 2) {
            const ms = index === 1 ? stats.median : stats.p95
            return ms < 0 ? "—" : Number(ms).toFixed(1) + " ms"
        }
        if (index === 3) return stats.loss < 0 ? "—" : Number(stats.loss).toFixed(1) + "%"
        if (index === 5) return stats.jitter >= 0 ? Number(stats.jitter).toFixed(1) + " ms" : "—"
        return stats.sent ? String(stats.longest) : "—"
    }
    background: Rectangle { color: "#16212b"; radius: 6; border.color: "#425461" }
    function rangeText(a, b) {
        return new Date(a).toISOString().slice(0, 23).replace("T", " ") + " – " +
               new Date(b).toISOString().slice(0, 23).replace("T", " ") + " UTC"
    }
    function update() {
        results = []; baselineRange = ""; incidentRange = ""; message = qsTr("Reading recorded probes…")
        if (sourceIndex === 1) backend.compareYesterday()
        else backend.compareFocus(sourceIndex === 0)
    }
    onOpened: { sourceIndex = backend.savedBaseline.name ? backend.baselineNames.indexOf(backend.savedBaseline.name) + 2 : 0; update() }
    onClosed: backend.cancelBaseline()
    Connections {
        target: backend
        function onSelectionChanged() { if (dialog.opened) dialog.close() }
        function onComparisonReady(rows, a, b, c, d, error) {
            if (!dialog.visible) return
            dialog.message = error
            if (error) return
            dialog.results = rows
            dialog.baselineRange = dialog.rangeText(a,b)
            dialog.incidentRange = dialog.rangeText(c,d)
        }
    }
    component Label: Text {
        color: "#c9d6df"; font.pixelSize: 12; verticalAlignment: Text.AlignVCenter; elide: Text.ElideRight
    }
    component Value: Label { width: dialog.metricWidth; horizontalAlignment: Text.AlignRight }
    component Measures: Row {
        required property var stats
        id: measures
        Repeater {
            model: dialog.metrics.length
            Value { required property int index; text: dialog.valueText(measures.stats, index) }
        }
    }
    contentItem: ColumnLayout {
        spacing: 8
        Label { text: qsTr("Compare time ranges"); color: "#e8eff2"; font.pixelSize: 19; font.bold: true }
        RowLayout {
            Label { visible: !dialog.compact; text: qsTr("Baseline") }
            TraceCombo {
                objectName: "baselineSource"
                model: [qsTr("Preceding interval"), qsTr("Same time yesterday (UTC)")].concat(backend.baselineNames)
                currentIndex: dialog.sourceIndex
                implicitWidth: 150
                Layout.fillWidth: dialog.compact
                Layout.minimumWidth: 100
                onActivated: index => { dialog.sourceIndex = index; if (index >= 2) backend.selectBaseline(backend.baselineNames[index - 2]); dialog.update() }
            }
            Item { visible: !dialog.compact; Layout.fillWidth: true }
            C.TextField { id: markName; Layout.preferredWidth: 140; Layout.minimumWidth: 55; Layout.fillWidth: true
                maximumLength: 100; placeholderText: qsTr("Baseline name"); Accessible.name: placeholderText }
            TraceButton { text: qsTr("Save focus"); enabled: !!markName.text.trim()
                onClicked: { backend.markBaseline(markName.text); dialog.close() } }
        }
        Label { text: qsTr("Baseline   ") + dialog.baselineRange; visible: !dialog.compact && !!dialog.baselineRange; Layout.fillWidth: true }
        Label { text: qsTr("Incident    ") + dialog.incidentRange; visible: !dialog.compact && !!dialog.incidentRange; Layout.fillWidth: true; color: "#b3d2ef" }
        Row {
            visible: !dialog.compact
            Label { width: 148; text: qsTr("Hop / period"); font.bold: true }
            Repeater {
                model: dialog.metrics
                Value { required property string modelData; text: modelData; font.bold: true }
            }
        }
        Label { visible: !!dialog.message; text: dialog.message; Layout.fillWidth: true; wrapMode: Text.Wrap }
        ListView {
            id: periodRows
            objectName: "periodRows"
            Layout.fillWidth: true; Layout.fillHeight: true; clip: true
            model: dialog.results
            C.ScrollBar.vertical: C.ScrollBar {}
            header: Column {
                width: periodRows.width - 14
                height: visible ? implicitHeight : 0
                visible: dialog.compact && !!dialog.baselineRange
                Label { width: parent.width; wrapMode: Text.Wrap; text: qsTr("Baseline   ") + dialog.baselineRange }
                Label { width: parent.width; wrapMode: Text.Wrap; text: qsTr("Incident    ") + dialog.incidentRange; color: "#b3d2ef" }
            }
            delegate: Rectangle {
                id: entry
                required property var modelData
                required property int index
                width: ListView.view.width; height: details.implicitHeight + 10
                color: index % 2 ? "#16212b" : "#121c25"
                Column {
                    id: details
                    anchors.left: parent.left; anchors.right: parent.right; y: 4; spacing: 2
                    Label {
                        text: (entry.modelData.hop ? qsTr("Hop %1").arg(entry.modelData.hop) : qsTr("Destination")) +
                              (entry.modelData.changed ? qsTr(" · Responder changed") : "") +
                              (entry.modelData.baseline.p95 >= 0 && entry.modelData.incident.p95 >= 0
                               ? qsTr("   Δ p95 %1 ms").arg((entry.modelData.incident.p95 - entry.modelData.baseline.p95).toFixed(1)) : "")
                        rightPadding: 3
                        color: entry.modelData.changed ? "#edc47f" : "#e8eff2"; font.bold: true
                    }
                    Row { visible: !dialog.compact; Label { width: 148; text: qsTr("Baseline"); leftPadding: 8 } Measures { stats: entry.modelData.baseline } }
                    Row { visible: !dialog.compact; Label { width: 148; text: qsTr("Incident"); leftPadding: 8 } Measures { stats: entry.modelData.incident } }
                    Column {
                        id: smallTable
                        width: parent.width - 14
                        visible: dialog.compact
                        readonly property real metricColumn: Math.floor(width * 0.36)
                        readonly property real valueColumn: Math.floor((width - metricColumn) / 2)
                        Row {
                            Label { width: smallTable.metricColumn; text: qsTr("Metric") }
                            Label { width: smallTable.valueColumn; text: qsTr("Baseline"); horizontalAlignment: Text.AlignRight }
                            Label { width: smallTable.valueColumn; text: qsTr("Incident"); horizontalAlignment: Text.AlignRight }
                        }
                        Repeater {
                            model: dialog.metrics
                            Row {
                                id: metricRow
                                required property int index
                                required property string modelData
                                Label { width: smallTable.metricColumn; text: metricRow.modelData }
                                Label {
                                    width: smallTable.valueColumn; horizontalAlignment: Text.AlignRight
                                    text: dialog.valueText(entry.modelData.baseline, metricRow.index)
                                }
                                Label {
                                    objectName: "compactIncident" + entry.index + "_" + metricRow.index
                                    width: smallTable.valueColumn; horizontalAlignment: Text.AlignRight
                                    text: dialog.valueText(entry.modelData.incident, metricRow.index)
                                }
                            }
                        }
                    }
                    Label {
                        width: parent.width - 20; visible: entry.modelData.changed; wrapMode: Text.WrapAnywhere
                        text: entry.modelData.baseline.addresses.join(", ") + " → " + entry.modelData.incident.addresses.join(", ")
                    }
                }
            }
        }
        RowLayout {
            Label { Layout.fillWidth: true; wrapMode: Text.Wrap; text: qsTr("RTT uses replies. The streak counts consecutive recorded timeouts.") }
            TraceButton { text: qsTr("Done"); onClicked: dialog.close() }
        }
    }
}
