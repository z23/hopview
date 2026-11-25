pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts

C.Dialog {
    id: dialog
    objectName: "siteComparisonDialog"
    property real uiScale: 1
    required property string profile
    required property var source
    property var choices: []
    property var picks: []
    property bool applying: false
    property string notice: ""
    function reload() { choices = backend.comparisonSites() }
    function toggle(row) {
        if (picks.some(p => p.site === row.site)) picks = picks.filter(p => p.site !== row.site)
        else picks = picks.concat([{site:row.site,revision:row.revision || 0,assign:!row.recording && !row.assigned}])
    }
    onOpened: { notice = ""; picks = []; reload(); backend.refreshCollectors() }
    Connections {
        target: backend
        function onCollectorsChanged() { if (dialog.visible) dialog.reload() }
        function onSessionsChanged() { if (dialog.visible && !dialog.applying) dialog.reload() }
        function onSelectionChanged() { if (dialog.visible && !dialog.applying) dialog.close() }
        function onSiteComparisonApplied(message) {
            if (!dialog.applying) return
            dialog.applying = false
            dialog.notice = message
            dialog.picks = []
            dialog.reload()
        }
    }
    modal: true; focus: true; anchors.centerIn: parent
    width: Math.min(570, parent.width / uiScale - 24)
    height: Math.min(430, parent.height / uiScale - 24)
    scale: uiScale
    padding: 16
    background: Rectangle { color: "#16212b"; border.color: "#425461"; radius: 6 }
    component Label: Text { font.pixelSize: 13; color: "#dce6ec"; wrapMode: Text.Wrap; textFormat: Text.PlainText }
    contentItem: ColumnLayout {
        spacing: 8
        Label { text: qsTr("Compare sites"); font.bold: true; font.pixelSize: 19 }
        Label { Layout.fillWidth: true; text: (dialog.source?.target || "") + " · " + dialog.profile + " · "
            + (dialog.source?.intervalMs || 0) + " ms · " + (dialog.source?.interfaceName || qsTr("System route")) }
        Label { Layout.fillWidth: true; text: qsTr("Select sites to reuse their recordings. New assignments follow incoming measurements from now.") }
        Label { Layout.fillWidth: true; visible: !dialog.choices.length; text: qsTr("No matching recordings or managed collectors at other sites.") }
        ListView {
            Layout.fillWidth: true; Layout.fillHeight: true; clip: true
            model: dialog.choices
            C.ScrollBar.vertical: C.ScrollBar {}
            delegate: C.CheckDelegate {
                id: choice
                required property var modelData
                objectName: "siteChoice" + modelData.site
                width: ListView.view.width
                enabled: !dialog.applying && (!!modelData.recording || !!modelData.managed)
                checked: dialog.picks.some(p => p.site === modelData.site)
                text: modelData.site + " · " + (modelData.recording ? qsTr("Use recording") : modelData.assigned
                      ? qsTr("Await assigned target") : qsTr("Add target to collector"))
                onClicked: dialog.toggle(modelData)
                contentItem: Label { text: choice.text; width: parent.width; leftPadding: 8; rightPadding: 36; verticalAlignment: Text.AlignVCenter }
            }
        }
        Label { Layout.fillWidth: true; visible: dialog.notice.length > 0; text: dialog.notice }
        RowLayout {
            Item { Layout.fillWidth: true }
            TraceButton { text: qsTr("Close"); onClicked: dialog.close() }
            TraceButton {
                objectName: "applySiteComparison"
                text: dialog.applying ? qsTr("Applying…") : dialog.picks.some(p => p.assign) ? qsTr("Assign and compare") : qsTr("Compare")
                enabled: !dialog.applying && dialog.picks.length > 0
                primary: true
                onClicked: { dialog.applying = true; backend.applySiteComparison(dialog.picks) }
            }
        }
    }
}
