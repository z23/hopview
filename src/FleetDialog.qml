pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts

C.Dialog {
    id: dialog
    objectName: "fleetDialog"
    property real uiScale: 1
    property var cells: []
    property var picked: []
    property double fromMs: 0
    property double throughMs: 0
    property bool busy: false
    property string problem: ""
    readonly property int targetWidth: 230
    readonly property int siteWidth: 196
    readonly property var matrix: {
        const sites = new Set(), targets = new Set(), values = new Map()
        const needle = filter.text.trim().toLowerCase()
        for (const cell of cells) {
            if (!(cell.siteLabel + " " + cell.targetLabel).toLowerCase().includes(needle)) continue
            sites.add(cell.siteLabel)
            targets.add(cell.targetLabel)
            values.set(JSON.stringify([cell.siteLabel, cell.targetLabel]), cell)
        }
        return {sites: Array.from(sites).sort(), targets: Array.from(targets).sort(), values: values}
    }
    function refresh() {
        const useFocus = scope.currentIndex === 1 && backend.sessionId > 0
        const end = useFocus ? backend.focusEnd : Date.now()
        const start = Math.max(0, useFocus ? backend.focusStart : end - 300000)
        busy = true
        problem = ""
        backend.requestFleet(start, end)
    }
    function label(cell) {
        if (!cell) return qsTr("No recording")
        const now = cell.incident || {}, before = cell.baseline || {}
        const seconds = Math.max(0, (throughMs - cell.lastSampleMs) / 1000)
        const age = !cell.lastSampleMs ? qsTr("Never sampled") : seconds < 60 ? qsTr("%1 s ago").arg(Math.floor(seconds))
                       : seconds < 86400 ? qsTr("%1 min ago").arg(Math.floor(seconds / 60))
                       : qsTr("%1 d ago").arg(Math.floor(seconds / 86400))
        if (!now.sent) return (now.resolution ? qsTr("No retained samples") : qsTr("No samples")) + "\n" + age
        const p95 = now.p95 >= 0 ? now.p95.toFixed(1) + " ms" : "—"
        const delta = now.p95 >= 0 && before.p95 >= 0 ? (now.p95 >= before.p95 ? "+" : "") + (now.p95 - before.p95).toFixed(1) : "—"
        return qsTr("%1% loss · %2/%3").arg(now.loss.toFixed(1)).arg(now.received).arg(now.sent)
                + (now.resolution ? qsTr("\nRetained summaries") : "\np95 " + p95 + " · Δ " + delta) + "\n" + age
    }
    function toggle(id) {
        picked = picked.includes(id) ? picked.filter(v => v !== id) : picked.concat([id])
    }
    Connections {
        target: backend
        function onFleetReady(data, from, through, error) {
            if (!dialog.visible) return
            dialog.fromMs = from
            dialog.throughMs = through
            dialog.cells = data
            dialog.problem = error
            dialog.busy = false
            const available = new Set(data.map(c => c.id))
            dialog.picked = dialog.picked.filter(id => available.has(id))
        }
    }
    onOpened: {
        picked = []
        scope.currentIndex = backend.sessionId && (backend.replayMode || !backend.live) ? 1 : 0
        refresh()
    }
    onClosed: backend.cancelFleet()
    Timer { running: dialog.visible && scope.currentIndex === 0 && !dialog.busy; interval: 10000; repeat: true; onTriggered: dialog.refresh() }
    modal: true
    focus: true
    anchors.centerIn: parent
    scale: uiScale
    width: Math.min(1080, parent.width / uiScale - 22)
    height: Math.min(650, parent.height / uiScale - 22)
    padding: 16
    background: Rectangle { color: "#16212b"; border.color: "#425461"; radius: 6 }
    component Label: Text { color: "#dce6ec"; font.pixelSize: 13; elide: Text.ElideRight; textFormat: Text.PlainText }
    contentItem: ColumnLayout {
        spacing: 8
        Label { text: qsTr("Site health"); font.pixelSize: 19; font.bold: true }
        RowLayout {
            C.TextField {
                id: filter
                objectName: "fleetFilter"
                placeholderText: qsTr("Filter sites or targets")
                Accessible.name: placeholderText
                Layout.fillWidth: true
            }
            TraceCombo {
                id: scope
                model: backend.sessionId ? [qsTr("Last 5 minutes"), qsTr("Current focus")] : [qsTr("Last 5 minutes")]
                onActivated: dialog.refresh()
            }
            TraceButton { text: qsTr("Refresh"); enabled: !dialog.busy; onClicked: dialog.refresh() }
        }
        Label {
            Layout.fillWidth: true
            text: dialog.problem || (dialog.busy ? qsTr("Reading measurements…") :
                !dialog.matrix.targets.length ? qsTr("No matching recordings.") :
                qsTr("%1 – %2 UTC · p95 change from the preceding equal interval").arg(new Date(dialog.fromMs).toISOString().slice(0,19).replace("T"," ")).arg(new Date(dialog.throughMs).toISOString().slice(11,19)))
        }
        Flickable {
            id: horizontal
            Layout.fillWidth: true; Layout.fillHeight: true
            clip: true
            contentWidth: Math.max(width, dialog.targetWidth + dialog.siteWidth * dialog.matrix.sites.length)
            contentHeight: height
            flickableDirection: Flickable.HorizontalFlick
            C.ScrollBar.horizontal: C.ScrollBar {}
            Column {
                width: horizontal.contentWidth; height: horizontal.height - 12
                Row {
                    height: 30
                    Label { width: dialog.targetWidth; height: 30; font.bold: true; text: qsTr("Target and probe"); verticalAlignment: Text.AlignVCenter }
                    Repeater {
                        model: dialog.matrix.sites
                        Label { required property string modelData; width: dialog.siteWidth; height: 30; text: modelData; font.bold: true; verticalAlignment: Text.AlignVCenter }
                    }
                }
                ListView {
                    id: rows
                    height: parent.height - 30; width: parent.width; clip: true
                    model: dialog.matrix.targets
                    C.ScrollBar.vertical: C.ScrollBar {}
                    delegate: Row {
                        id: targetRow
                        required property string modelData
                        height: 70
                        Label { width: dialog.targetWidth - 4; rightPadding: 8; height: 70; text: targetRow.modelData; wrapMode: Text.WrapAnywhere; verticalAlignment: Text.AlignVCenter }
                        spacing: 0
                        Repeater {
                            model: dialog.matrix.sites
                            C.AbstractButton {
                                id: cellButton
                                required property string modelData
                                readonly property var cell: dialog.matrix.values.get(JSON.stringify([modelData, targetRow.modelData]))
                                readonly property bool chosen: !!cell && dialog.picked.includes(cell.id)
                                width: dialog.siteWidth; height: targetRow.height
                                enabled: !!cell
                                Accessible.name: modelData + " " + targetRow.modelData + " " + dialog.label(cell)
                                Accessible.checkable: true
                                Accessible.checked: chosen
                                onClicked: dialog.toggle(cell.id)
                                background: Rectangle {
                                    anchors.margins: 2; radius: 3
                                    color: !cellButton.cell || !cellButton.cell.incident?.sent ? "#202830" :
                                        cellButton.cell.incident.loss > 0 ? "#493136" : "#19342f"
                                    border.color: cellButton.chosen || cellButton.visualFocus ? "#9cc4ff" : "#384650"
                                    border.width: cellButton.chosen || cellButton.visualFocus ? 2 : 1
                                }
                                contentItem: Label {
                                    leftPadding: 9; text: (cellButton.chosen ? "✓ " : "") + dialog.label(cellButton.cell)
                                    verticalAlignment: Text.AlignVCenter; font.pixelSize: 12
                                }
                            }
                        }
                    }
                }
            }
        }
        RowLayout {
            Label { Layout.fillWidth: true; text: dialog.picked.length ? qsTr("%1 selected").arg(dialog.picked.length) : qsTr("Select a cell to inspect, or several cells to compare.") }
            TraceButton { text: qsTr("Close"); onClicked: dialog.close() }
            TraceButton {
                text: dialog.picked.length > 1 ? qsTr("Compare selection") : qsTr("Open trace")
                enabled: !dialog.busy && dialog.picked.length > 0
                primary: true
                onClicked: { backend.compareSessions(dialog.picked, dialog.fromMs, dialog.throughMs); dialog.close() }
            }
        }
    }
}
