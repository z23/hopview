import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts

C.Dialog {
    id: dialog
    objectName: "alertsDialog"
    property real uiScale: 1
    property string target: ""
    property double traceId: 0
    property int latency: 200
    property int loss: 20
    property int seconds: 30
    anchors.centerIn: parent
    width: Math.min(565, parent.width / uiScale - 20)
    height: Math.min(590, parent.height / uiScale - 20)
    scale: uiScale
    modal: true
    padding: 16
    background: Rectangle { color: "#16212b"; radius: 8; border.color: "#425461" }
    palette.window: "#16212b"
    palette.button: "#263540"
    palette.base: "#101820"
    palette.text: "#e8eff2"
    palette.buttonText: "#e8eff2"
    onAboutToShow: {
        traceId = backend.sessionId
        target = ""
        for (const trace of backend.sessions)
            if (trace.id === traceId) target = trace.location ? trace.location + " · " + trace.target : trace.target
        const rule = backend.alertRules[String(traceId)]
        latency = rule ? rule.latency : 0
        loss = rule ? rule.loss : 0
        seconds = rule ? rule.seconds : 30
        events.positionViewAtBeginning()
    }
    component Label: Text {
        color: "#e8eff2"
        font.family: "Inter"
        font.pixelSize: 13
        wrapMode: Text.Wrap
    }
    contentItem: ColumnLayout {
        spacing: 8
        Label { text: "Recording and alerts"; font.pixelSize: 19; font.bold: true }
        ListView {
            id: events
            clip: true
            Layout.fillWidth: true
            Layout.fillHeight: true
            boundsBehavior: Flickable.StopAtBounds
            model: dialog.visible ? backend.alertEvents : []
            C.ScrollBar.vertical: C.ScrollBar { }
            header: ColumnLayout {
                width: events.width - 12
                spacing: 8
                C.CheckBox {
                    objectName: "keepRecording"
                    text: "Keep recording after closing the window"
                    checked: backend.backgroundEnabled
                    enabled: backend.backgroundAvailable
                    onClicked: backend.setBackgroundEnabled(checked)
                }
                Label {
                    Layout.fillWidth: true
                    text: "Reopen Hopview to see running traces. Starting it again after exit resumes those traces."
                    color: "#a3b3bf"
                }
                Label {
                    Layout.fillWidth: true
                    Layout.topMargin: 8
                    text: dialog.traceId ? "Destination thresholds · " + dialog.target : "Open a trace to configure its destination alerts."
                    font.bold: true
                }
                GridLayout {
                    visible: dialog.traceId > 0
                    columns: 2
                    columnSpacing: 10
                    rowSpacing: 4
                    Label { text: "Mean RTT (ms)" }
                    C.SpinBox {
                        editable: true; from: 0; to: 60000; value: dialog.latency
                        Accessible.name: "Mean RTT threshold in milliseconds"
                        onValueModified: dialog.latency = value
                    }
                    Label { text: "Loss (%)" }
                    C.SpinBox {
                        editable: true; from: 0; to: 100; value: dialog.loss
                        Accessible.name: "Destination packet loss threshold in percent"
                        onValueModified: dialog.loss = value
                    }
                    Label { text: "Window (seconds)" }
                    C.SpinBox {
                        editable: true; from: 1; to: 86400; value: dialog.seconds
                        Accessible.name: "Alert evaluation window in seconds"
                        onValueModified: dialog.seconds = value
                    }
                }
                Label {
                    visible: dialog.traceId > 0
                    Layout.fillWidth: true
                    text: "A threshold must be reached for two consecutive windows. Two healthy windows confirm recovery. Set a threshold to 0 to disable it."
                    color: "#a3b3bf"
                }
                TraceButton {
                    visible: dialog.traceId > 0
                    objectName: "saveAlertThresholds"
                    text: "Save thresholds"
                    enabled: {
                        const saved = backend.alertRules[String(dialog.traceId)]
                        return saved ? (dialog.loss !== saved.loss || dialog.latency !== saved.latency || dialog.seconds !== saved.seconds)
                                     : dialog.loss > 0 || dialog.latency > 0
                    }
                    onClicked: backend.setAlertRule(dialog.traceId, dialog.latency, dialog.loss, dialog.seconds)
                }
                Label {
                    Layout.topMargin: 10
                    text: "Recent events"
                    font.bold: true
                }
                Label {
                    visible: events.count === 0
                    text: "No destination alerts recorded."
                    color: "#a3b3bf"
                }
            }
            delegate: C.ItemDelegate {
                id: row
                required property var modelData
                objectName: "alertEvent" + modelData.id
                width: events.width - 12
                implicitHeight: summary.implicitHeight + 14
                Accessible.name: (modelData.sourceLabel || modelData.target) + " " + modelData.label + ", view interval"
                background: Rectangle {
                    color: row.hovered || row.visualFocus ? "#243442" : "#16212b"
                }
                contentItem: ColumnLayout {
                    id: summary
                    spacing: 3
                    Label {
                        Layout.fillWidth: true
                        text: row.modelData.label + " · " + (row.modelData.sourceLabel || row.modelData.target)
                        color: row.modelData.recovered ? "#8cdac2" : "#ed9696"
                    }
                    Label {
                        Layout.fillWidth: true
                        text: Qt.formatDateTime(new Date(row.modelData.endMs), "ddd d MMM yyyy, HH:mm:ss") + "   ·   View interval ›"
                        color: "#a3b3bf"; font.pixelSize: 12
                    }
                }
                onClicked: {
                    backend.showAlert(modelData.session, modelData.startMs, modelData.endMs)
                    dialog.close()
                }
            }
        }
        RowLayout {
            Layout.fillWidth: true
            TraceButton {
                text: "Quit Hopview"
                hint: "Flush pending samples and exit the recorder"
                onClicked: Qt.quit()
            }
            Item { Layout.fillWidth: true }
            TraceButton { text: "Done"; onClicked: dialog.close() }
        }
    }
}
