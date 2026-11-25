pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts
import QtQuick.Dialogs as D

C.Dialog {
    id: dialog
    property real uiScale: 1
    property int hop: 1
    property var responders: []
    property string message: ""
    scale: uiScale
    anchors.centerIn: parent
    modal: true
    focus: true
    width: Math.min(620,parent.width / uiScale - 20)
    height: Math.min(420,parent.height / uiScale - 20)
    title: qsTr("Responders in focus")
    function showHop(number) { hop = number; picker.value = hop; open(); refresh() }
    function refresh() {
        responders = []; message = qsTr("Reading recorded replies…"); backend.requestResponders(hop)
    }
    onAboutToShow: { picker.value = hop; refresh() }
    Connections {
        target: backend
        function onRespondersReady(id, number, rows, error) {
            if (dialog.hop !== number || backend.sessionId !== id) return
            dialog.responders = rows; dialog.message = error || (!rows.length ? qsTr("No raw replies in this interval.") : "")
        }
    }
    contentItem: ColumnLayout {
        RowLayout {
            C.Label { text: qsTr("Hop") }
            C.SpinBox { id: picker; from: 1; to: 255; editable: true
                onValueModified: { dialog.hop = value; dialog.refresh() } }
            C.Label { Layout.fillWidth: true; text: qsTr("Flow mode · %1").arg(backend.snapshot.measurement?.flowModes?.join(" → ") || qsTr("Unknown")) }
        }
        C.Label { text: qsTr("Counts are successful replies at this TTL in the focus interval."); wrapMode: Text.Wrap; Layout.fillWidth: true }
        C.Label { visible: !!dialog.message; text: dialog.message }
        ListView {
            id: list
            Layout.fillWidth: true; Layout.fillHeight: true; clip: true
            model: dialog.responders
            C.ScrollBar.vertical: C.ScrollBar {}
            delegate: C.Label {
                required property var modelData
                readonly property var info: modelData.identity || ({})
                width: list.width; padding: 6; wrapMode: Text.Wrap
                text: modelData.address + "   " + qsTr("%1 replies").arg(modelData.replies)
                      + (info.prefix ? "\n" + [info.prefix,info.asn,info.pop,info.peer,info.circuit].filter(Boolean).join(" · ")
                         + "\n" + qsTr("Source %1 · imported %2").arg(info.source).arg(info.updated) : "")
                      + (info.originSource ? "\n" + qsTr("Origin %1 · %2 · imported %3").arg(info.originPrefix).arg(info.originSource).arg(info.originUpdated) : "")
            }
        }
        RowLayout {
            TraceButton { visible: !backend.replayMode; text: qsTr("Import network inventory…"); onClicked: file.open() }
            Item { Layout.fillWidth: true }
            TraceButton { text: qsTr("Close"); onClicked: dialog.close() }
        }
    }
    D.FileDialog { id: file; title: qsTr("Import prefix inventory (tab-separated)")
        onAccepted: { backend.importInventory(selectedFile); dialog.close() } }
}
