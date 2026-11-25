pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts

ColumnLayout {
    id: editor
    property var node: ({})
    property var activeTrace: null
    property var original: []
    property var draft: []
    property double revision: 0
    property string site: ""
    property string copySite: ""
    readonly property var peers: backend.collectors.filter(n => n.site !== site && n.targets.length > 0)
    readonly property var peerNames: peers.map(n => n.site)
    readonly property var copyPeer: peers.find(n => n.site === copySite) || peers[0] || null
    property string problem: ""
    property bool saving: false
    property bool editing: false
    property int editIndex: -1
    readonly property bool dirty: JSON.stringify(original) !== JSON.stringify(draft)
    readonly property bool stale: node.site === site && Number(node.revision) !== revision
    signal leave()
    signal saved(string site)
    spacing: 8

    function target(t) {
        return Object.assign({}, t, {target: t.target, family: Number(t.family || 0), protocol: t.protocol || "auto",
                port: Number(t.port || 0), intervalMs: Number(t.intervalMs || 1000), interface: t.interface || ""})
    }
    function load(info) {
        site = info.site
        revision = Number(info.revision)
        original = Array.from(info.targets || [], target)
        draft = original.slice()
        copySite = ""
        editing = false
        problem = ""
    }
    function describe(t) {
        const ip = t.family ? "IPv" + t.family : qsTr("Auto IP")
        let method = t.protocol.toUpperCase()
        if (t.port) method += " / " + t.port
        const summary = [ip, method, t.intervalMs + " ms"]
        if (t.interface) summary.push(t.interface)
        if (t.diagnostics?.url) summary.push(t.diagnostics.url)
        if (t.diagnostics?.interface) summary.push(qsTr("%1 counters").arg(t.diagnostics.interface))
        return summary.join("  ·  ")
    }
    function editRow(i, values) {
        editIndex = i
        const t = target(values)
        destination.text = t.target || ""
        family.currentIndex = [0, 4, 6].indexOf(t.family)
        protocol.currentIndex = ["auto", "icmp", "udp", "tcp"].indexOf(t.protocol)
        port.text = String(t.port || (t.protocol === "tcp" ? 443 : 33434))
        interval.text = String(t.intervalMs)
        serviceUrl.text = t.diagnostics?.url || ""
        diagnosticInterface.text = t.diagnostics?.interface || ""
        serviceInterval.text = String(t.diagnostics?.seconds || 30)
        sizeBox.text = String(t.payloadBytes || 16)
        dscpBox.text = String(t.dscp || 0)
        df.checked = !!t.dontFragment
        iface.text = t.interface
        editing = true
        problem = ""
        destination.forceActiveFocus()
    }
    function storeRow() {
        const name = destination.text.trim()
        if (serviceUrl.text.trim() && !serviceInterval.acceptableInput) {
            problem = qsTr("Choose a service interval between 5 and 3600 seconds.")
            return
        }
        if (!name || !sizeBox.acceptableInput || !dscpBox.acceptableInput || !interval.acceptableInput || (protocol.currentIndex !== 1 && !port.acceptableInput)) {
            problem = qsTr("Enter a destination, an interval from 250 to 60000 ms and a valid port.")
            return
        }
        const row = target(Object.assign({}, editIndex < 0 ? {} : draft[editIndex], {target: name, family: [0,4,6][family.currentIndex],
            protocol: ["auto","icmp","udp","tcp"][protocol.currentIndex],
            port: protocol.currentIndex === 1 ? 0 : Number(port.text),
            intervalMs: Number(interval.text), interface: iface.text.trim()}))
        delete row.payloadBytes
        delete row.dscp
        delete row.dontFragment
        if (protocol.currentIndex !== 3 && Number(sizeBox.text) !== 16) row.payloadBytes = Number(sizeBox.text)
        if (Number(dscpBox.text)) row.dscp = Number(dscpBox.text)
        if (protocol.currentIndex !== 3 && df.checked) row.dontFragment = true
        delete row.diagnostics
        if (serviceUrl.text.trim() || diagnosticInterface.text.trim())
            row.diagnostics = {url: serviceUrl.text.trim(), interface: diagnosticInterface.text.trim(), seconds: Number(serviceInterval.acceptableInput ? serviceInterval.text : 30)}
        if (draft.some((t, i) => i !== editIndex && JSON.stringify(t) === JSON.stringify(row))) {
            problem = qsTr("That target is already in this list.")
            return
        }
        const copy = draft.slice()
        if (editIndex < 0) copy.push(row)
        else copy[editIndex] = row
        draft = copy
        problem = ""
        editing = false
    }
    function current() {
        const t = activeTrace
        editRow(-1, {target: t.target, family: t.family, protocol: t.protocol, port: t.port,
                     intervalMs: t.intervalMs, interface: "",payloadBytes:t.payloadBytes,dscp:t.dscp,dontFragment:t.dontFragment})
    }
    Connections {
        target: backend
        function onCollectorTargetsSaved(changedSite, error) {
            if (changedSite !== editor.site || !editor.saving) return
            editor.saving = false
            if (error.length) editor.problem = error
            else editor.saved(changedSite)
        }
    }
    component Info: Text {
        textFormat: Text.PlainText; color: "#dce6ec"; font.pixelSize: 13; wrapMode: Text.WrapAnywhere
    }
    component EditField: C.TextField {
        color: "#e8eff2"; placeholderTextColor: "#9babb7"; selectByMouse: true
        implicitHeight: 32
        background: Rectangle { color: "#101a23"; border.color: parent.activeFocus ? "#7bcfbd" : "#405260"; radius: 4 }
    }
    Info {
        Layout.fillWidth: true; font.bold: true; font.pixelSize: 16
        text: editor.editing ? (editor.editIndex < 0 ? qsTr("Add target") : qsTr("Edit target")) : qsTr("Assigned targets")
    }
    C.ScrollView {
        id: scroll
        Layout.fillWidth: true; Layout.fillHeight: true; clip: true
        contentWidth: availableWidth
        ColumnLayout {
            width: scroll.availableWidth; spacing: 7
            ColumnLayout {
                Layout.fillWidth: true; visible: !editor.editing; spacing: 5
                Info { Layout.fillWidth: true; visible: !editor.draft.length; text: qsTr("This collector has no targets.") }
                Repeater {
                    model: editor.draft
                    delegate: Rectangle {
                        id: row
                        required property int index
                        required property var modelData
                        Layout.fillWidth: true; implicitHeight: rowControls.implicitHeight + 12
                        radius: 3; color: "#15212b"
                        RowLayout {
                            id: rowControls
                            anchors.left: parent.left; anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                            anchors.margins: 6; spacing: 5
                            ColumnLayout {
                                Layout.fillWidth: true; spacing: 2
                                Info { Layout.fillWidth: true; font.bold: true; text: row.modelData.target }
                                Info { Layout.fillWidth: true; color: "#a6b7c4"; font.pixelSize: 12; text: editor.describe(row.modelData) }
                            }
                            TraceButton { enabled: !editor.saving; text: qsTr("Edit"); Accessible.name: qsTr("Edit %1").arg(row.modelData.target); onClicked: editor.editRow(row.index, row.modelData) }
                            TraceButton {
                                objectName: "collectorRemoveTarget" + row.index; enabled: !editor.saving; text: "×"; implicitWidth: 32
                                Accessible.name: qsTr("Remove %1").arg(row.modelData.target); hint: Accessible.name
                                onClicked: { const copy = editor.draft.slice(); copy.splice(row.index, 1); editor.draft = copy; editor.problem = "" }
                            }
                        }
                    }
                }
                Flow {
                    Layout.fillWidth: true; spacing: 6
                    TraceButton { objectName: "collectorAddTarget"; text: qsTr("Add target…"); enabled: !editor.saving && editor.draft.length < 128; onClicked: editor.editRow(-1, {}) }
                    TraceButton { visible: !!editor.activeTrace; enabled: !editor.saving && editor.draft.length < 128; text: qsTr("Use current trace…"); onClicked: editor.current() }
                }
                ColumnLayout {
                    Layout.fillWidth: true; visible: !!editor.copyPeer; spacing: 3
                    Layout.topMargin: 4
                    Info { text: qsTr("Copy targets from"); font.bold: true }
                    RowLayout {
                        Layout.fillWidth: true; spacing: 6
                        TraceCombo {
                            Layout.fillWidth: true; enabled: !editor.saving
                            model: editor.peerNames; currentIndex: Math.max(0, editor.peerNames.indexOf(editor.copySite))
                            hint: qsTr("Collector to copy assigned targets from")
                            onActivated: editor.copySite = currentText
                        }
                        TraceButton {
                            objectName: "collectorCopyTargets"; enabled: !editor.saving
                            text: editor.draft.length ? qsTr("Replace draft") : qsTr("Copy targets")
                            hint: qsTr("Review the copied targets before applying them to this collector")
                            onClicked: {
                                editor.draft = Array.from(editor.copyPeer.targets, editor.target)
                                editor.problem = ""
                            }
                        }
                    }
                }
            }
            GridLayout {
                Layout.fillWidth: true; visible: editor.editing
                columns: 2; columnSpacing: 10; rowSpacing: 6
                Info { text: qsTr("Destination") }
                EditField {
                    id: destination; objectName: "collectorDestination"; Layout.fillWidth: true
                    maximumLength: 255; placeholderText: qsTr("Hostname or IP address"); Accessible.name: qsTr("Destination")
                }
                Info { text: qsTr("IP family") }
                TraceCombo { id: family; objectName: "collectorFamily"; Layout.fillWidth: true; model: [qsTr("Auto IP"), "IPv4", "IPv6"]; hint: qsTr("Address family") }
                Info { text: qsTr("Protocol") }
                TraceCombo {
                    id: protocol; objectName: "collectorProtocol"; Layout.fillWidth: true
                    model: [qsTr("Auto"), "ICMP", "UDP", "TCP"]; hint: qsTr("Probe protocol")
                    onActivated: port.text = currentIndex === 3 ? "443" : "33434"
                }
                Info { visible: protocol.currentIndex !== 1; text: qsTr("Port") }
                EditField {
                    id: port; objectName: "collectorPort"; Layout.fillWidth: true
                    visible: protocol.currentIndex !== 1; maximumLength: 5
                    validator: IntValidator { bottom: 1; top: 65535 }
                    Accessible.name: qsTr("Destination port")
                }
                Info { text: qsTr("Interval (ms)") }
                EditField {
                    id: interval; objectName: "collectorInterval"; Layout.fillWidth: true
                    maximumLength: 5; validator: IntValidator { bottom: 250; top: 60000 }
                    Accessible.name: qsTr("Interval in milliseconds")
                }
                Info { text: qsTr("Payload bytes"); visible: protocol.currentIndex !== 3 }
                EditField { id: sizeBox; visible: protocol.currentIndex !== 3; Layout.fillWidth: true; validator: IntValidator { bottom: 8; top: 65507 } Accessible.name: qsTr("Payload bytes") }
                Info { text: qsTr("DSCP") }
                EditField { id: dscpBox; Layout.fillWidth: true; validator: IntValidator { bottom: 0; top: 63 } Accessible.name: qsTr("DSCP") }
                C.CheckBox { id: df; Layout.columnSpan: 2; visible: protocol.currentIndex !== 3; text: qsTr("Require unfragmented datagrams") }
                Info { text: qsTr("Service URL") }
                EditField { id: serviceUrl; objectName: "collectorServiceUrl"; Layout.fillWidth: true; maximumLength: 2048; placeholderText: qsTr("Optional HTTP(S) endpoint"); Accessible.name: qsTr("Service URL on collector") }
                Info { text: qsTr("Service interval (s)"); visible: serviceUrl.text.length > 0 }
                EditField { id: serviceInterval; visible: serviceUrl.text.length > 0; Layout.fillWidth: true; validator: IntValidator { bottom: 5; top: 3600 }
                    Accessible.name: qsTr("Service interval in seconds") }
                Info { text: qsTr("Interface counters") }
                EditField { id: diagnosticInterface; objectName: "collectorDiagnosticInterface"; Layout.fillWidth: true; maximumLength: 63; placeholderText: qsTr("Optional device on collector"); Accessible.name: qsTr("Interface for traffic counters") }
                Info { text: qsTr("Probe interface") }
                EditField {
                    id: iface; Layout.fillWidth: true; maximumLength: 63
                    placeholderText: qsTr("System route"); Accessible.name: qsTr("Outgoing interface on the collector")
                }
            }
        }
    }
    Info {
        Layout.fillWidth: true; visible: editor.stale && !editor.editing
        color: "#f2c779"; text: qsTr("A newer target list is available. Reload it before applying changes.")
    }
    Info { Layout.fillWidth: true; visible: editor.problem.length > 0; color: "#f1a59e"; text: editor.problem }
    Flow {
        Layout.fillWidth: true; spacing: 6
        TraceButton {
            text: editor.editing ? qsTr("Cancel") : editor.dirty ? qsTr("Discard changes") : qsTr("Back")
            enabled: !editor.saving
            onClicked: { if (editor.editing) { editor.editing = false; editor.problem = "" } else editor.leave() }
        }
        TraceButton {
            objectName: "collectorReloadTargets"; visible: editor.stale && !editor.editing; enabled: !editor.saving; text: qsTr("Reload list")
            onClicked: editor.load(editor.node)
        }
        TraceButton {
            objectName: "collectorStageTarget"; visible: editor.editing
            text: editor.editIndex < 0 ? qsTr("Add to list") : qsTr("Update list")
            primary: true; onClicked: editor.storeRow()
        }
        TraceButton {
            objectName: "collectorApply"; visible: !editor.editing; primary: true
            enabled: editor.dirty && !editor.saving && !editor.stale
            opacity: enabled ? 1.0 : 0.5
            text: editor.saving ? qsTr("Saving…") : !editor.draft.length && editor.dirty ? qsTr("Stop all targets") : qsTr("Apply changes")
            onClicked: { editor.saving = true; editor.problem = ""; backend.saveCollectorTargets(editor.site, editor.revision, editor.draft) }
        }
    }
}
