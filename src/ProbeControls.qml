import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts

TraceButton {
    id: button
    objectName: "probeControls"
    readonly property string protocol: ["auto", "icmp", "udp", "tcp"][method.currentIndex]
    readonly property int port: protocol === "icmp" ? 0 : portBox.value
    readonly property string interfaceName: iface.currentIndex > 0 ? iface.currentText : ""
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
        width: 264
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
                onActivated: portBox.value = currentIndex === 3 ? 443 : 33434
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
            TraceButton { text: "Done"; Layout.columnSpan: 2; Layout.alignment: Qt.AlignRight; onClicked: settings.close() }
        }
    }
}
