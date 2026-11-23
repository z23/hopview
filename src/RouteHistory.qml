pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts

C.Dialog {
    id: dialog
    objectName: "routeHistoryDialog"
    modal: true
    focus: true
    anchors.centerIn: parent
    property real uiScale: 1
    scale: uiScale
    width: Math.min(810, parent.width / uiScale - 24)
    height: Math.min(565, parent.height / uiScale - 24)
    padding: 14
    property bool scanning: false
    property string failure: ""
    property bool followNewest: true
    readonly property var selected: picker.currentIndex >= 0 && picker.currentIndex < events.count
                                    ? events.get(picker.currentIndex) : null
    readonly property int eventCount: events.count
    readonly property color ink: "#e8eff2"
    readonly property color muted: "#a3b3bf"
    readonly property color accent: "#84d9bd"
    background: Rectangle { color: "#16212b"; radius: 6; border.color: "#425461" }

    function reload() {
        scanning = true
        failure = ""
        followNewest = true
        events.clear()
        backend.requestRouteHistory()
    }
    function timestamp(ms) {
        return Qt.formatDateTime(new Date(ms), "yyyy-MM-dd HH:mm:ss.zzz")
    }
    function rtt(mean, count) {
        return count > 0 ? qsTr("%1 ms (%2/3)").arg(Number(mean).toFixed(1)).arg(count) : qsTr("No replies")
    }
    function responder(address, exists, target) {
        if (!exists) return "—"
        return (address || qsTr("Unknown")) + (target ? qsTr(" (target)") : "")
    }
    onOpened: reload()
    onClosed: backend.cancelRouteHistory()
    ListModel { id: events }

    Connections {
        target: backend
        function onSelectionChanged() { if (dialog.opened) dialog.reload() }
        function onRouteHistoryPage(batch, finished, error) {
            if (!dialog.visible) return
            for (let e of batch) {
                events.append({firstMs: Number(e.firstMs), confirmedMs: Number(e.confirmedMs),
                               startMs: Number(e.startMs), beforeRtt: e.beforeRtt, afterRtt: e.afterRtt,
                               beforeN: e.beforeN, afterN: e.afterN, path: e.path,
                               label: dialog.timestamp(e.firstMs) + qsTr("   ·   Changed hops %1").arg(e.changedHops)})
            }
            if (dialog.followNewest && batch.length > 0) picker.currentIndex = events.count - 1
            dialog.scanning = !finished
            dialog.failure = error
        }
    }

    component Caption: Text {
        color: dialog.muted
        font.pixelSize: 12
        wrapMode: Text.Wrap
    }
    contentItem: ColumnLayout {
        Accessible.role: Accessible.Dialog
        Accessible.name: qsTr("Route history")
        spacing: 8
        Text {
            text: qsTr("Route history")
            color: dialog.ink; font.pixelSize: 18; font.bold: true
        }
        Caption {
            Layout.fillWidth: true
            text: qsTr("A new responder or target hop needs three consecutive matching rounds. Load balancing can also produce this pattern.")
        }
        C.ComboBox {
            id: picker
            objectName: "routeEventPicker"
            Layout.fillWidth: true
            visible: events.count > 0
            model: events
            textRole: "label"
            Accessible.name: qsTr("Route change")
            onActivated: dialog.followNewest = false
        }
        Caption {
            Layout.fillWidth: true
            visible: dialog.selected !== null
            text: dialog.selected ? qsTr("Confirmed %1    ·    Destination RTT before %2, after %3")
                      .arg(dialog.timestamp(dialog.selected.confirmedMs))
                      .arg(dialog.rtt(dialog.selected.beforeRtt, dialog.selected.beforeN))
                      .arg(dialog.rtt(dialog.selected.afterRtt, dialog.selected.afterN)) : ""
        }
        GridLayout {
            visible: dialog.selected !== null
            Layout.fillWidth: true
            columns: 3; columnSpacing: 8
            Caption { Layout.preferredWidth: 40; text: qsTr("Hop") }
            Caption { Layout.fillWidth: true; Layout.preferredWidth: 1; text: qsTr("Before") }
            Caption { Layout.fillWidth: true; Layout.preferredWidth: 1; text: qsTr("After") }
        }
        ListView {
            id: routePath
            objectName: "routePath"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: dialog.selected ? dialog.selected.path : null
            C.ScrollBar.vertical: C.ScrollBar {}
            delegate: Rectangle {
                id: row
                required property int hop
                required property string before
                required property string after
                required property bool inBefore
                required property bool inAfter
                required property bool wasTarget
                required property bool isTarget
                required property bool changed
                readonly property string oldAddress: dialog.responder(before, inBefore, wasTarget)
                readonly property string newAddress: dialog.responder(after, inAfter, isTarget)
                width: routePath.width
                height: cells.implicitHeight + 8
                color: changed ? "#263747" : hop % 2 ? "#121c25" : "#16212b"
                Accessible.role: Accessible.ListItem
                Accessible.name: qsTr("Hop %1%2. Before %3. After %4").arg(hop)
                                     .arg(changed ? qsTr(", changed") : "").arg(oldAddress).arg(newAddress)
                RowLayout {
                    id: cells
                    width: parent.width - 8
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 8
                    Caption { Layout.preferredWidth: 40; text: row.changed ? "▸ " + row.hop : "   " + row.hop; color: row.changed ? dialog.accent : dialog.muted }
                    Caption { Layout.fillWidth: true; Layout.preferredWidth: 1; font.family: "monospace"; text: row.oldAddress; wrapMode: Text.WrapAnywhere }
                    Caption { Layout.fillWidth: true; Layout.preferredWidth: 1; font.family: "monospace"; text: row.newAddress; color: row.changed ? dialog.accent : dialog.ink; wrapMode: Text.WrapAnywhere }
                }
            }
            Caption {
                anchors.centerIn: parent
                width: parent.width - 16
                horizontalAlignment: Text.AlignHCenter
                visible: events.count === 0
                text: dialog.failure ? dialog.failure : dialog.scanning ? qsTr("Reading recording…") : qsTr("No sustained route changes in this recording.")
            }
        }
        Caption {
            Layout.fillWidth: true
            visible: events.count > 0
            text: dialog.failure ? dialog.failure : dialog.scanning ? qsTr("Reading recording…")
                 : events.count === 1 ? qsTr("1 confirmed change") : qsTr("%1 confirmed changes").arg(events.count)
        }
        RowLayout {
            Layout.fillWidth: true
            TraceButton { text: qsTr("Refresh"); enabled: !dialog.scanning; onClicked: dialog.reload() }
            Item { Layout.fillWidth: true }
            TraceButton { text: qsTr("Close"); onClicked: dialog.close() }
            TraceButton {
                objectName: "showRouteEvent"
                text: qsTr("Show on timeline")
                primary: true
                enabled: dialog.selected !== null
                onClicked: {
                    backend.zoomToRange(dialog.selected.startMs, dialog.selected.confirmedMs)
                    dialog.close()
                }
            }
        }
    }
}
