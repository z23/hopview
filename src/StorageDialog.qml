pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts
import QtQuick.Dialogs as D

C.Dialog {
    id: dialog
    property real uiScale: 1
    property var stats: ({})
    property string message: ""
    property bool busy: false
    scale: uiScale; modal: true; focus: true; anchors.centerIn: parent
    width: Math.min(550,parent.width / uiScale - 24)
    title: qsTr("Recording storage")
    function act(action, file) { busy = true; message = ""; backend.storageAction(action, raw.value, summary.value, file || "") }
    onAboutToShow: act("status")
    Connections {
        target: backend
        function onStorageUpdated(settings, text) {
            dialog.stats = settings; dialog.message = text; dialog.busy = false
            raw.value = settings.rawDays || 0; summary.value = settings.summaryDays || 0
        }
    }
    contentItem: ColumnLayout {
        C.Label { Layout.fillWidth: true; wrapMode: Text.Wrap
            text: qsTr("Database %1 MiB · %2 GiB available · %3 pinned intervals").arg(((dialog.stats.bytes || 0)/1048576).toFixed(1)).arg(((dialog.stats.available || 0)/1073741824).toFixed(1)).arg(dialog.stats.pins || 0) }
        C.Label { Layout.fillWidth: true; wrapMode: Text.Wrap
            text: dialog.stats.growthPerDay >= 0 ? qsTr("Observed growth %1 MiB/day").arg((dialog.stats.growthPerDay/1048576).toFixed(1))
                   + (dialog.stats.daysToFull >= 0 ? qsTr(" · approximately %1 days of disk space").arg(Math.floor(dialog.stats.daysToFull)) : "")
                   : qsTr("Growth forecast needs two hourly observations.") }
        GridLayout {
            columns: 2
            C.Label { text: qsTr("Raw retention (days)") }
            C.SpinBox { id: raw; from: 0; to: 36500; editable: true }
            C.Label { text: qsTr("Summary retention (days)") }
            C.SpinBox { id: summary; from: 0; to: 36500; editable: true }
        }
        C.Label { Layout.fillWidth: true; wrapMode: Text.Wrap
            text: qsTr("Zero keeps data indefinitely. Applying retention expires unpinned history immediately. Pinning preserves whole UTC hours for the traces in focus.") }
        RowLayout {
            TraceButton { text: qsTr("Pin focus"); enabled: !dialog.busy && backend.sessionId > 0; onClicked: dialog.act("pin") }
            TraceButton { text: qsTr("Online backup…"); enabled: !dialog.busy; onClicked: backup.open() }
            TraceButton { text: qsTr("Apply retention"); enabled: !dialog.busy && (raw.value !== dialog.stats.rawDays || summary.value !== dialog.stats.summaryDays)
                onClicked: confirm.open() }
        }
        RowLayout {
            visible: !!dialog.stats.pinList?.length
            Layout.fillWidth: true
            TraceCombo {
                id: pinChoice
                Layout.fillWidth: true
                model: (dialog.stats.pinList || []).map(p => (p.site ? p.site + " · " : "") + p.target + " · " +
                    new Date(p.from).toISOString().slice(0,16) + " — " + new Date(p.through).toISOString().slice(0,16) + " UTC")
                hint: qsTr("Pinned recording and covered hours")
            }
            TraceButton { text: qsTr("Release pin…"); enabled: !dialog.busy && pinChoice.currentIndex >= 0
                onClicked: { release.pin = dialog.stats.pinList[pinChoice.currentIndex]; release.open() } }
        }
        C.Label { text: dialog.busy ? qsTr("Working…") : dialog.message; Layout.fillWidth: true; wrapMode: Text.Wrap }
    }
    standardButtons: C.Dialog.Close
    D.FileDialog { id: backup; fileMode: D.FileDialog.SaveFile; title: qsTr("Save a new database backup")
        onAccepted: dialog.act("backup",selectedFile) }
    C.Dialog {
        id: release; property var pin: ({})
        modal: true; anchors.centerIn: parent; width: Math.min(410,dialog.width-20); implicitHeight: 172
        title: qsTr("Release this pin?")
        standardButtons: C.Dialog.Apply | C.Dialog.Cancel
        contentItem: C.Label { wrapMode: Text.Wrap; text: qsTr("Retention will immediately expire eligible data for %1 in these hours. Overlapping pins stay protected.").arg(release.pin.target || "") }
        onApplied: { close(); dialog.busy = true; backend.releasePin(pin.id, pin.from, pin.through) }
    }
    C.Dialog { id: confirm; modal: true; anchors.centerIn: parent; title: qsTr("Expire unpinned history?")
        standardButtons: C.Dialog.Apply | C.Dialog.Cancel
        onApplied: { close(); dialog.act("save") } }
}
