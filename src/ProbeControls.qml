import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts

TraceButton {
    id: button
    objectName: "probeControls"
    readonly property string protocol: ["auto", "icmp", "udp", "tcp"][method.currentIndex]
    readonly property int port: protocol === "icmp" ? 0 : portBox.value
    readonly property string interfaceName: iface.currentIndex > 0 ? iface.currentText : ""
    readonly property int payloadBytes: protocol === "tcp" ? 16 : sizeBox.value
    readonly property int dscp: dscpBox.value
    readonly property bool dontFragment: protocol !== "tcp" && df.checked
    readonly property string comparison: ["off","size","dscp"][pair.currentIndex]
    readonly property int comparisonPayloadBytes: comparison === "size" ? otherSize.value : payloadBytes
    readonly property int comparisonDscp: comparison === "dscp" ? otherDscp.value : dscp
    text: protocol === "auto" ? "Probes" : protocol.toUpperCase()
    hint: "Choose probe protocol, destination port and source interface"
    onClicked: {
        const previous = interfaceName
        const names = ["System route"]
        for (const name of backend.interfaceNames()) names.push(name)
        iface.model = names
        iface.currentIndex = Math.max(0, iface.model.indexOf(previous))
        settings.open()
    }
    C.Popup {
        id: settings
        y: button.height + 4
        x: button.Window.window ? Math.min(0, (button.Window.window.width - button.mapToItem(null, 0, 0).x) / scale - width) : 0
        width: 316
        padding: 14
        focus: true
        scale: button.Window.window ? button.Window.window.uiScale : 1
        transformOrigin: Item.TopLeft
        background: Rectangle { color: "#1b2732"; radius: 5; border.color: "#425461" }
        contentItem: GridLayout {
            columns: 2
            columnSpacing: 10
            rowSpacing: 8
            C.Label { text: "Protocol"; color: "#dce6ec" }
            TraceCombo {
                id: method
                objectName: "probeProtocol"
                Layout.fillWidth: true
                model: ["Auto", "ICMP", "UDP", "TCP"]
                hint: "Probe protocol"
                onActivated: {
                    portBox.value = currentIndex === 3 ? 443 : 33434
                    if (currentIndex === 3 && pair.currentIndex === 1) pair.currentIndex = 0
                }
            }
            C.Label { text: "Destination port"; color: "#dce6ec"; visible: method.currentIndex !== 1 }
            C.SpinBox {
                id: portBox
                objectName: "probePort"
                Layout.fillWidth: true
                visible: method.currentIndex !== 1
                editable: true; from: 1; to: 65535; value: 33434
                Accessible.name: "Destination port"
                textFromValue: (value, locale) => String(value)
                valueFromText: (text, locale) => Number(text)
            }
            C.Label { text: "Interface"; color: "#dce6ec" }
            TraceCombo {
                id: iface
                objectName: "probeInterface"
                Layout.fillWidth: true
                model: ["System route"]
                hint: "Outgoing network interface"
            }
            C.Label { text: qsTr("Payload (bytes)"); color: "#dce6ec"; visible: method.currentIndex !== 3 }
            C.SpinBox { id: sizeBox; Layout.fillWidth: true; visible: method.currentIndex !== 3; from: 8; to: 65507; value: 16; editable: true; Accessible.name: qsTr("Payload bytes") }
            C.Label { text: "DSCP"; color: "#dce6ec" }
            C.SpinBox { id: dscpBox; Layout.fillWidth: true; from: 0; to: 63; value: 0; editable: true; Accessible.name: qsTr("DSCP") }
            C.CheckBox { id: df; Layout.columnSpan: 2; visible: method.currentIndex !== 3; text: qsTr("Require unfragmented datagrams") }
            C.Label { text: qsTr("Paired test"); color: "#dce6ec" }
            TraceCombo {
                id: pair; Layout.fillWidth: true
                model: [qsTr("Off"),qsTr("Payload size"),qsTr("DSCP")]
                onActivated: if (currentIndex === 1 && method.currentIndex === 3) currentIndex = 0
            }
            C.Label { text: qsTr("Second payload"); color: "#dce6ec"; visible: pair.currentIndex === 1 }
            C.SpinBox { id: otherSize; Layout.fillWidth: true; visible: pair.currentIndex === 1; from: 8; to: 65507; value: 1400; editable: true; Accessible.name: qsTr("Comparison payload bytes") }
            C.Label { text: qsTr("Second DSCP"); color: "#dce6ec"; visible: pair.currentIndex === 2 }
            C.SpinBox { id: otherDscp; Layout.fillWidth: true; visible: pair.currentIndex === 2; from: 0; to: 63; value: 46; editable: true; Accessible.name: qsTr("Comparison DSCP") }
            TraceButton { text: "Done"; Layout.columnSpan: 2; Layout.alignment: Qt.AlignRight; onClicked: settings.close() }
        }
    }
}
