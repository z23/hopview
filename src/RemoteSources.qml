pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts

C.Dialog {
    id: dialog
    objectName: "remoteSources"
    property real uiScale: 1
    modal: true; focus: true; anchors.centerIn: parent; scale: uiScale
    width: Math.min(740, parent.width / uiScale - 24)
    height: Math.min(460, parent.height / uiScale - 24)
    padding: 16
    property double clock: Date.now()
    property var sources: backend.sessions.concat(backend.closedSessions).filter(s => s.remoteLive)
    Timer { interval: 1000; running: dialog.opened; repeat: true; onTriggered: dialog.clock = Date.now() }
    background: Rectangle { radius: 6; color: "#16212b"; border.color: "#425461" }
    function timestamp(ms) { return ms ? new Date(ms).toISOString() : qsTr("Unknown") }
    function age(ms) {
        const seconds = Math.max(0,Math.floor((clock - ms) / 1000))
        if (seconds < 60) return qsTr("%1 s ago").arg(seconds)
        if (seconds < 3600) return qsTr("%1 min ago").arg(Math.floor(seconds/60))
        return qsTr("%1 h ago").arg(Math.floor(seconds/3600))
    }
    component Label: Text { color: "#d8e4ec"; font.pixelSize: 12; wrapMode: Text.WrapAnywhere }
    contentItem: ColumnLayout {
        spacing: 8
        Label { text: qsTr("Remote sources"); font.pixelSize: 19; font.bold: true }
        Label {
            Layout.fillWidth: true
            text: qsTr("The receiver and this window share the history file below. Each VM pushes its locally configured traces to the receiver.")
        }
        C.TextField { Layout.fillWidth: true; readOnly: true; selectByMouse: true; text: backend.databasePath; Accessible.name: qsTr("Receiver history path") }
        Label { visible: !dialog.sources.length; text: qsTr("Waiting for the first VM recording. See the collector and receiver setup in docs/telemetry.md.") }
        ListView {
            Layout.fillWidth: true; Layout.fillHeight: true; clip: true
            model: dialog.sources
            C.ScrollBar.vertical: C.ScrollBar {}
            delegate: Rectangle {
                id: row
                required property int index
                required property var modelData
                width: ListView.view.width; height: lines.implicitHeight + 14
                color: index % 2 ? "#16212b" : "#121c25"
                Column {
                    id: lines
                    x: 7; y: 5; width: parent.width - 14; spacing: 3
                    Label { width: parent.width; font.bold: true; text: row.modelData.location + " · " + row.modelData.target + " (" + row.modelData.resolvedAddress + ")" }
                    Label { text: qsTr("Last capture %1   ·   Last delivery %2").arg(dialog.timestamp(row.modelData.endMs)).arg(dialog.age(row.modelData.lastReceivedMs)); width: parent.width }
                    Label { width: parent.width; text: row.modelData.clockErrorMs < 0 ? qsTr("Latest clock estimate unavailable") :
                        qsTr("Latest collector clock offset relative to receiver: %1 ms ± %2 ms").arg(row.modelData.clockOffsetMs).arg(row.modelData.clockErrorMs) }
                }
            }
        }
        TraceButton { Layout.alignment: Qt.AlignRight; text: qsTr("Done"); onClicked: dialog.close() }
    }
}
