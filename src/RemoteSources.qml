pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts
import QtCore as Core
import Hopview.Graphics 1.0

C.Dialog {
    id: dialog
    objectName: "remoteSources"
    property real uiScale: 1
    property var activeTrace: null
    property string page: "nodes"
    property string joinKey: ""
    property string problem: ""
    property string notice: ""
    property bool generating: false
    property double clock: Date.now()
    readonly property var recordings: backend.sessions.concat(backend.closedSessions).filter(s => s.remoteLive)
    readonly property var currentNode: backend.collectors.find(n => n.site === targets.site) || ({})
    modal: true; focus: true; anchors.centerIn: parent; scale: uiScale
    width: Math.min(740, parent.width / uiScale - 20)
    height: Math.min(565, parent.height / uiScale - 20)
    padding: 14
    closePolicy: page === "targets" && (targets.dirty || targets.editing || targets.saving)
                 ? C.Popup.NoAutoClose : C.Popup.CloseOnEscape
    onAboutToShow: {
        page = "nodes"; problem = ""; notice = ""; clock = Date.now()
        address.text = preferences.receiverAddress
        backend.refreshCollectors()
    }
    onClosed: { joinKey = ""; generating = false; problem = "" }
    onPageChanged: { if (page !== "join") joinKey = ""; problem = "" }
    function timestamp(ms) { return ms ? new Date(ms).toISOString() : qsTr("Unknown") }
    function age(ms) {
        const seconds = Math.max(0, Math.floor((clock - ms) / 1000))
        if (seconds < 60) return qsTr("%1 s ago").arg(seconds)
        if (seconds < 3600) return qsTr("%1 min ago").arg(Math.floor(seconds / 60))
        return qsTr("%1 h ago").arg(Math.floor(seconds / 3600))
    }
    function status(info) {
        if (info.applyError) return qsTr("Could not apply · %1").arg(info.applyError)
        return Number(info.revision) === Number(info.appliedRevision)
             ? qsTr("Applied · revision %1").arg(info.revision)
             : qsTr("Pending · revision %1 (running %2)").arg(info.revision).arg(info.appliedRevision)
    }
    function manage(info) { notice = ""; targets.load(info); page = "targets" }
    Core.Settings {
        id: preferences
        location: Core.StandardPaths.writableLocation(Core.StandardPaths.AppConfigLocation) + "/collectors.ini"
        category: encodeURIComponent(backend.databasePath)
        property string receiverAddress: ""
    }
    LocalReceiver { id: listener; database: backend.collectorManagementAvailable ? backend.databasePath : "" }
    Timer {
        interval: 2000; running: dialog.opened; repeat: true
        onTriggered: { dialog.clock = Date.now(); backend.refreshCollectors() }
    }
    Connections {
        target: backend
        function onCollectorKeyCreated(key, error) {
            if (!dialog.generating || !dialog.opened || dialog.page !== "join") return
            dialog.generating = false
            dialog.joinKey = key
            dialog.problem = error
        }
        function onCollectorListError(error) { if (dialog.opened) dialog.problem = error }
    }
    component Label: Text { textFormat: Text.PlainText; color: "#d8e4ec"; font.pixelSize: 13; wrapMode: Text.WrapAnywhere }
    component Field: C.TextField {
        selectByMouse: true; color: "#e4edf1"; placeholderTextColor: "#93a6b4"
        implicitHeight: 34
        background: Rectangle { color: "#101a23"; radius: 4; border.color: parent.activeFocus ? "#7bcfbd" : "#405260" }
    }
    background: Rectangle { color: "#1b2732"; radius: 6; border.color: "#425461" }
    contentItem: ColumnLayout {
        spacing: 8
        RowLayout {
            Layout.fillWidth: true
            Label {
                font.pixelSize: 20; font.bold: true; Layout.fillWidth: true
                text: dialog.page === "join" ? qsTr("Add collector") : dialog.page === "targets" ? targets.site : qsTr("Collectors")
            }
            TraceButton {
                objectName: "openCollectorJoin"; visible: dialog.page === "nodes" || dialog.page === "recordings"
                text: qsTr("Add collector…"); primary: true
                onClicked: { dialog.page = "join"; dialog.notice = ""; address.forceActiveFocus() }
            }
        }
        RowLayout {
            visible: dialog.page === "nodes" || dialog.page === "recordings"
            TraceButton {
                text: qsTr("Nodes (%1)").arg(backend.collectors.length); primary: dialog.page === "nodes"
                onClicked: dialog.page = "nodes"
            }
            TraceButton {
                text: qsTr("Recordings (%1)").arg(dialog.recordings.length); primary: dialog.page === "recordings"
                onClicked: dialog.page = "recordings"
            }
        }
        ListView {
            id: nodeList; objectName: "collectorList"
            visible: dialog.page === "nodes"; Layout.fillWidth: true; Layout.fillHeight: true; clip: true
            spacing: 4; model: backend.collectors
            C.ScrollBar.vertical: C.ScrollBar {}
            Label {
                visible: !nodeList.count; anchors.top: parent.top; width: parent.width
                text: qsTr("Choose Add collector to connect your first VM.")
            }
            delegate: Rectangle {
                id: nodeRow
                required property var modelData
                width: nodeList.width; height: details.implicitHeight + 16
                color: "#14212a"; radius: 3
                RowLayout {
                    id: details; anchors.left: parent.left; anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                    anchors.margins: 8; spacing: 6
                    ColumnLayout {
                        Layout.fillWidth: true; spacing: 3
                        Label { Layout.fillWidth: true; font.bold: true; text: nodeRow.modelData.site }
                        Label { Layout.fillWidth: true; visible: nodeRow.modelData.hostname !== nodeRow.modelData.site; text: nodeRow.modelData.hostname }
                        Label {
                            Layout.fillWidth: true; color: dialog.clock - nodeRow.modelData.lastSeenMs < 45000 ? "#86d1bd" : "#b4c1cb"
                            text: (dialog.clock - nodeRow.modelData.lastSeenMs < 45000 ? qsTr("Online") : qsTr("Offline")) +
                                  " · " + dialog.age(nodeRow.modelData.lastSeenMs) + " · " + (nodeRow.modelData.targets.length === 1 ? qsTr("1 assigned target") : qsTr("%1 assigned targets").arg(nodeRow.modelData.targets.length))
                        }
                        Label { Layout.fillWidth: true; color: nodeRow.modelData.applyError ? "#f1a59e" : "#b4c1cb"; text: dialog.status(nodeRow.modelData) }
                    }
                    TraceButton {
                        objectName: "editCollector-" + nodeRow.modelData.site
                        text: qsTr("Targets…"); Accessible.name: qsTr("Edit targets for %1").arg(nodeRow.modelData.site)
                        onClicked: dialog.manage(nodeRow.modelData)
                    }
                }
            }
        }
        ListView {
            id: records
            visible: dialog.page === "recordings"; Layout.fillWidth: true; Layout.fillHeight: true; clip: true
            model: dialog.recordings
            C.ScrollBar.vertical: C.ScrollBar {}
            Label { visible: !records.count; width: parent.width; text: qsTr("Remote traces appear after a collector sends its first measurements.") }
            delegate: Rectangle {
                id: row
                required property int index
                required property var modelData
                width: records.width; height: lines.implicitHeight + 14
                color: index % 2 ? "#16212b" : "#121c25"
                Column {
                    id: lines; x: 7; y: 5; width: parent.width - 14; spacing: 3
                    Label { width: parent.width; font.bold: true; text: row.modelData.location + " · " + row.modelData.target + " (" + row.modelData.resolvedAddress + ")" }
                    Label { text: qsTr("Last capture %1 · Last delivery %2").arg(dialog.timestamp(row.modelData.endMs)).arg(dialog.age(row.modelData.lastReceivedMs)); width: parent.width }
                    Label { width: parent.width; text: row.modelData.clockErrorMs < 0 ? qsTr("Latest clock estimate unavailable") :
                        qsTr("Collector clock offset relative to receiver: %1 ms ± %2 ms").arg(row.modelData.clockOffsetMs).arg(row.modelData.clockErrorMs) }
                }
            }
        }
        C.ScrollView {
            id: enrollment
            visible: dialog.page === "join"; Layout.fillWidth: true; Layout.fillHeight: true
            clip: true; contentWidth: availableWidth
            ColumnLayout {
                width: enrollment.availableWidth; spacing: 6
                Label { text: qsTr("Receiver address"); font.bold: true }
                Field {
                    id: address; objectName: "collectorReceiverAddress"
                    Layout.fillWidth: true; maximumLength: 320; enabled: !listener.busy && !listener.running && !dialog.generating && !dialog.joinKey
                    Accessible.name: qsTr("Receiver address"); placeholderText: qsTr("Hostname or address[:port]")
                    readonly property string joinCommand: backend.collectorJoinCommand(text)
                    readonly property bool valid: joinCommand.length > 0
                    onTextEdited: dialog.problem = ""
                }
                Label {
                    Layout.fillWidth: true; color: "#b2c2cd"
                    text: address.text && !address.valid ? qsTr("Use a hostname, IPv4 address or bracketed IPv6 address, with an optional port.")
                          : qsTr("Use the address your VM can reach. Start the receiver here, or open your dedicated receiver’s history file in Hopview.")
                }
                RowLayout {
                    TraceButton {
                        objectName: "collectorListenerToggle"; enabled: !listener.busy && !dialog.generating && (listener.running || address.valid)
                        text: listener.busy ? qsTr("Please wait…") : listener.running ? qsTr("Stop receiver") : qsTr("Start receiver")
                        onClicked: {
                            dialog.joinKey = ""
                            if (listener.running) listener.stop()
                            else { preferences.receiverAddress = address.text; listener.start(address.text) }
                        }
                    }
                    Label { Layout.fillWidth: true; visible: !!listener.message; text: listener.message }
                }
                Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: "#354653"; Layout.topMargin: 4; Layout.bottomMargin: 4 }
                Label { text: qsTr("On the VM"); font.bold: true }
                Label { Layout.fillWidth: true; text: qsTr("After installing the node package, run this command. Paste a new key when prompted.") }
                RowLayout {
                    Layout.fillWidth: true
                    Field {
                        id: command; objectName: "collectorJoinCommand"; Layout.fillWidth: true; readOnly: true
                        Accessible.name: qsTr("Join command")
                        text: address.joinCommand || "sudo hopview-node join RECEIVER[:PORT]"
                    }
                    TraceButton { text: qsTr("Copy"); Accessible.name: qsTr("Copy join command"); enabled: address.valid; onClicked: { command.selectAll(); command.copy() } }
                }
                TraceButton {
                    objectName: "generateCollectorKey"; visible: !dialog.joinKey; primary: true
                    enabled: address.valid && !dialog.generating && !listener.busy
                    opacity: enabled ? 1.0 : 0.55
                    text: dialog.generating ? qsTr("Generating…") : qsTr("Generate node key")
                    onClicked: { preferences.receiverAddress = address.text; dialog.generating = true; dialog.problem = ""; backend.createCollectorKey() }
                }
                Label { visible: !!dialog.joinKey; text: qsTr("Node key · shown once"); font.bold: true }
                RowLayout {
                    visible: !!dialog.joinKey; Layout.fillWidth: true
                    Field { id: keyField; objectName: "collectorEnrollmentKey"; readOnly: true; Layout.fillWidth: true; text: dialog.joinKey; Accessible.name: qsTr("Node key") }
                    TraceButton { text: qsTr("Copy"); Accessible.name: qsTr("Copy node key"); onClicked: { keyField.selectAll(); keyField.copy() } }
                }
                Label { visible: !!dialog.joinKey; Layout.fillWidth: true; color: "#b2c2cd"; text: qsTr("The VM appears under Nodes using its hostname, ready for its first target.") }
                Label {
                    visible: !!dialog.joinKey && dialog.joinKey.split(".").length < 3
                    Layout.fillWidth: true; color: "#f2c779"
                    text: qsTr("This key requires a certificate trusted by the VM. For a private certificate, start the receiver and generate a new key.")
                }
            }
        }
        CollectorTargets {
            id: targets; objectName: "collectorTargetsEditor"
            visible: dialog.page === "targets"; Layout.fillWidth: true; Layout.fillHeight: true
            activeTrace: dialog.activeTrace; node: dialog.currentNode
            onLeave: dialog.page = "nodes"
            onSaved: (site) => { dialog.page = "nodes"; dialog.notice = qsTr("Target list saved for %1.").arg(site) }
        }
        Label { visible: !!dialog.problem; text: dialog.problem; Layout.fillWidth: true; color: "#f1a59e" }
        Label { visible: !!dialog.notice && dialog.page === "nodes"; text: dialog.notice; Layout.fillWidth: true; color: "#9ad6c2" }
        RowLayout {
            visible: dialog.page !== "targets"; Layout.fillWidth: true
            Field {
                visible: dialog.page === "recordings"; Layout.fillWidth: true; readOnly: true
                text: backend.databasePath; Accessible.name: qsTr("Receiver history path")
            }
            Item { visible: dialog.page !== "recordings"; Layout.fillWidth: true }
            TraceButton {
                objectName: "collectorBack"
                text: dialog.page === "join" ? qsTr("Back to nodes") : qsTr("Done")
                enabled: !dialog.generating
                onClicked: { if (dialog.page === "join") dialog.page = "nodes"; else dialog.close() }
            }
        }
    }
}
