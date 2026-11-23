import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts

C.Dialog {
    id: dialog
    objectName: "diagnosticSettings"
    property real uiScale: 1
    property double traceId: 0
    property string problem: ""
    parent: C.Overlay.overlay
    anchors.centerIn: parent
    scale: uiScale
    width: Math.min(470, parent.width / uiScale - 24)
    height: Math.min(body.implicitHeight + 2 * padding + 2, parent.height / uiScale - 24)
    padding: 16
    modal: true
    focus: true
    background: Rectangle { color: "#16212b"; radius: 5; border.color: "#425461" }
    palette.window: "#16212b"
    palette.base: "#101820"
    palette.text: "#e8eff2"
    palette.button: "#263540"
    palette.buttonText: "#e8eff2"
    onAboutToShow: {
        traceId = backend.sessionId
        problem = ""
        const stored = backend.diagnosticsSettings[String(traceId)] || {}
        url.text = stored.url || ""
        period.value = stored.seconds || 30
        let names = [qsTr("Off")]
        for (const i of backend.interfaceNames()) names.push(i)
        if (stored.interface && !names.includes(stored.interface)) names.push(stored.interface)
        device.model = names
        device.currentIndex = Math.max(0, names.indexOf(stored.interface || ""))
    }
    contentItem: Flickable {
        id: scroll
        clip: true
        contentHeight: body.implicitHeight
        boundsBehavior: Flickable.StopAtBounds
        C.ScrollBar.vertical: C.ScrollBar {}
        ColumnLayout {
            id: body
            width: scroll.width - (scroll.height < scroll.contentHeight ? 12 : 0)
            spacing: 9
            C.Label { text: qsTr("Service and interface"); font.bold: true; font.pixelSize: 18 }
            C.Label { text: qsTr("HTTP(S) service URL"); Layout.fillWidth: true }
            C.TextField {
                id: url; objectName: "serviceUrl"
                Layout.fillWidth: true; selectByMouse: true; maximumLength: 2048
                placeholderText: qsTr("Leave empty to disable service checks")
                Accessible.name: qsTr("Service URL")
            }
            RowLayout {
                C.Label { text: qsTr("Check every") }
                C.SpinBox {
                    id: period; objectName: "servicePeriod"; editable: true; from: 5; to: 3600
                    Accessible.name: qsTr("Check interval in seconds")
                }
                C.Label { text: qsTr("seconds") }
            }
            C.Label { text: qsTr("Local interface to monitor") }
            TraceCombo { id: device; objectName: "diagnosticInterface"; Layout.fillWidth: true }
            C.Label {
                Layout.fillWidth: true; wrapMode: Text.WordWrap; color: "#a3b3bf"
                text: qsTr("Measurements run while this trace collects. The service check makes a fresh HEAD request. Interface rates cover all traffic on the selected device.")
            }
            C.Label { visible: dialog.problem.length > 0; text: dialog.problem; color: "#ed9696"; wrapMode: Text.Wrap; Layout.fillWidth: true }
            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                TraceButton { text: qsTr("Cancel"); onClicked: dialog.close() }
                TraceButton {
                    objectName: "saveDiagnostics"; text: qsTr("Save")
                    onClicked: backend.setDiagnostics(dialog.traceId, url.text, device.currentIndex > 0 ? device.currentText : "", period.value)
                }
            }
        }
    }
    Connections {
        target: backend
        function onDiagnosticSettingsSaved(id, error) {
            if (dialog.visible && id === dialog.traceId) {
                if (error) dialog.problem = error
                else dialog.close()
            }
        }
    }
}
