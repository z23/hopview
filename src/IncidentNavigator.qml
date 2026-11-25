pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts

C.Dialog {
    id: dialog
    property real uiScale: 1
    property string problem: ""
    objectName: "incidentNavigator"
    scale: uiScale
    anchors.centerIn: parent
    width: Math.min(640, parent.width / uiScale - 24)
    height: Math.min(470, parent.height / uiScale - 24)
    modal: true; focus: true
    title: qsTr("Find incident by UTC time")
    function timestamp(ms) { return new Date(ms).toISOString() }
    function openAt(ms) { open(); from.text = timestamp(ms); through.text = "" }
    function parse(value) {
        const text = value.trim().replace(/ UTC$/i, "Z").replace(" ", "T")
        if (!/^\d{4}-\d\d-\d\dT\d\d:\d\d(:\d\d(\.\d{1,3})?)?(Z|[+-]\d\d:\d\d)?$/i.test(text)) return NaN
        return Date.parse(/(Z|[+-]\d\d:\d\d)$/i.test(text) ? text : text + "Z")
    }
    function jump() {
        const a = parse(from.text)
        const b = through.text.trim() ? parse(through.text) : a + 30000
        if (!isFinite(a) || !isFinite(b) || a < 0 || b <= a) {
            problem = qsTr("Enter a valid UTC start and a later end."); return
        }
        backend.zoomToRange(through.text.trim() ? a : Math.max(0,a - 30000),b)
        close()
    }
    onAboutToShow: { from.text = timestamp(backend.focusStart); through.text = ""; problem = "" }
    contentItem: ColumnLayout {
        C.Label { text: qsTr("UTC start or ticket timestamp"); Layout.fillWidth: true }
        C.TextField { id: from; objectName: "utcStart"; Layout.fillWidth: true; Accessible.name: qsTr("UTC start"); selectByMouse: true }
        C.Label { text: qsTr("UTC end (optional)"); Layout.fillWidth: true }
        C.TextField { id: through; objectName: "utcEnd"; Layout.fillWidth: true; Accessible.name: qsTr("UTC end"); onAccepted: dialog.jump() }
        C.Label { visible: !!dialog.problem; text: dialog.problem; color: "#ef948d" }
        C.Label { text: qsTr("Events in the displayed interval"); font.bold: true }
        ListView {
            id: eventList
            Layout.fillHeight: true; Layout.fillWidth: true; clip: true
            model: dialog.visible ? (backend.snapshot.events || []).slice(0,500) : []
            C.ScrollBar.vertical: C.ScrollBar {}
            delegate: C.ItemDelegate {
                required property var modelData
                width: eventList.width
                text: dialog.timestamp(modelData.t) + "   " + modelData.kind + "   " + modelData.text
                onClicked: { from.text = dialog.timestamp(modelData.t); through.text = "" }
            }
            C.Label { visible: !eventList.count; text: qsTr("No recorded events in this interval.") }
        }
        C.Label { visible: dialog.visible && (backend.snapshot.events || []).length > 500; text: qsTr("Latest 500 events. Narrow the interval to see earlier events.") }
        RowLayout {
            Item { Layout.fillWidth: true }
            TraceButton { text: qsTr("Close"); onClicked: dialog.close() }
            TraceButton { text: qsTr("Show interval"); primary: true; onClicked: dialog.jump() }
        }
    }
}
