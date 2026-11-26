import QtQuick
import QtQuick.Controls as C

C.ComboBox {
    id: control
    property string hint: ""
    implicitHeight: 32
    implicitWidth: 90
    leftPadding: 10
    rightPadding: 24
    hoverEnabled: true
    Accessible.name: hint
    contentItem: Text {
        text: control.displayText
        color: "#dce6ec"
        font.family: "Inter"
        font.pixelSize: 12
        elide: Text.ElideRight
        verticalAlignment: Text.AlignVCenter
    }
    indicator: Canvas {
        x: control.width - 18
        y: (control.height - height) / 2
        width: 10
        height: 6
        onPaint: {
            let p = getContext("2d")
            p.clearRect(0, 0, width, height)
            p.strokeStyle = "#afbbc4"; p.lineWidth = 1.5
            p.beginPath(); p.moveTo(1, 1); p.lineTo(5, 5); p.lineTo(9, 1); p.stroke()
        }
    }
    background: Rectangle {
        radius: 5
        color: control.hovered ? "#263340" : "#1b2732"
        border.color: control.visualFocus ? "#adc9cd" : "#354350"
    }
    delegate: C.ItemDelegate {
        id: comboItem
        required property var modelData
        required property int index
        width: control.width
        height: 30
        highlighted: control.highlightedIndex === index
        contentItem: Text {
            text: comboItem.modelData
            color: "#e0e9ec"
            verticalAlignment: Text.AlignVCenter
            font.pixelSize: 12
            font.family: "Inter"
        }
        background: Rectangle { color: comboItem.highlighted || comboItem.hovered ? "#344552" : "#1d2934" }
    }
    popup: C.Popup {
        y: control.height + 3
        width: control.width
        scale: control.Window.window ? control.Window.window.uiScale : 1
        transformOrigin: C.Popup.TopLeft
        height: Math.min(contentItem.implicitHeight + 8, 320)
        padding: 4
        contentItem: ListView {
            clip: true
            implicitHeight: contentHeight
            model: control.popup.visible ? control.delegateModel : null
            currentIndex: control.highlightedIndex
            C.ScrollIndicator.vertical: C.ScrollIndicator {}
        }
        background: Rectangle { color: "#1d2934"; radius: 5; border.color: "#425461" }
    }
    C.ToolTip.visible: hovered && !popup.visible && hint.length > 0
    C.ToolTip.delay: 500
    C.ToolTip.text: hint
}
