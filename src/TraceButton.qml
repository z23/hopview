import QtQuick
import QtQuick.Controls as C

C.Button {
    id: control
    property bool primary: false
    property color accent: "#84d9bd"
    property string hint: ""
    implicitHeight: 32
    implicitWidth: Math.max(32, contentItem.implicitWidth + 22)
    hoverEnabled: true
    leftPadding: 11
    rightPadding: 11
    Accessible.name: text
    contentItem: Text {
        text: control.text
        color: control.primary ? "#101a1d" : control.enabled ? "#dce6ec" : "#798591"
        font.family: "Inter"
        font.pixelSize: 12
        font.weight: Font.DemiBold
        verticalAlignment: Text.AlignVCenter
        horizontalAlignment: Text.AlignHCenter
    }
    background: Rectangle {
        radius: 5
        color: control.primary ? (control.down ? Qt.darker(control.accent, 1.16) : control.accent)
             : control.down ? "#334350" : control.hovered ? "#273644" : "#1d2935"
        border.color: control.visualFocus ? control.accent : control.primary ? control.accent : "#354350"
    }
    C.ToolTip.visible: hovered && hint.length > 0
    C.ToolTip.delay: 450
    C.ToolTip.text: hint
}
