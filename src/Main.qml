import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts
import QtQuick.Dialogs as Dialogs
import Hopline.Graphics 1.0

C.ApplicationWindow {
    id: app
    width: 1220
    height: 800
    minimumWidth: 760
    minimumHeight: 530
    visible: true
    title: activeTarget.length ? activeTarget + " — Hopline" : "Hopline"
    color: base
    font.family: "Inter"
    palette.window: base
    palette.text: ink
    palette.windowText: ink
    palette.highlight: accent
    readonly property color base: "#101820"
    readonly property color panel: "#16212b"
    readonly property color well: "#121c25"
    readonly property color edge: "#2c3945"
    readonly property color ink: "#e8eff2"
    readonly property color mute: "#a3b3bf"
    readonly property color faint: "#8295a3"
    readonly property color accent: themeAccent.hslLightness < 0.50 ? Qt.lighter(themeAccent, 1.55) : themeAccent
    readonly property color lossColor: "#ed8086"
    readonly property var rows: backend.snapshot.rows || []
    readonly property var charts: backend.snapshot.charts || []
    readonly property var currentSession: {
        for (const s of backend.sessions)
            if (s.id === backend.sessionId) return s
        return null
    }
    readonly property string activeTarget: currentSession ? currentSession.target : ""
    readonly property var focusOptions: [0, -1, 10, 30, 60, 300, 900, 3600, 21600, 86400]
    readonly property var viewOptions: [10, 30, 60, 300, 900, 3600, 21600, 86400, 604800]
    readonly property int rowSize: 29
    readonly property int numWidth: width >= 1060 ? 61 : 47
    readonly property int hopWidth: 69
    readonly property int graphWidth: Math.floor(Math.max(155, routeContent.width * 0.34))
    readonly property int hostWidth: Math.max(90, routeContent.width - graphWidth - 7 * numWidth - hopWidth)
    readonly property int chartLabelWidth: app.width >= 950 ? 171 : 127
    readonly property int axisRight: 18
    readonly property real chartWidth: Math.max(1, timelineFrame.width - 26 - chartLabelWidth - axisRight)
    readonly property real viewSpan: Math.max(1, backend.viewEnd - backend.viewStart)
    readonly property real profileCeiling: scaleCeiling(rows)
    readonly property real timeCeiling: niceScale(backend.snapshot.timeMax || 0)
    property real cursorFraction: -1
    property int cursorHop: -1
    property var cursorBucket: null
    property bool inputError: false
    property string exportStatus: ""

    function niceScale(v) {
        if (!(v > 0)) return 10
        const magnitude = Math.pow(10, Math.floor(Math.log10(v)))
        for (const step of [1, 2, 4, 5, 8, 10])
            if (step * magnitude >= v) return step * magnitude
        return 10 * magnitude
    }
    function scaleCeiling(rs) {
        let value = 0
        for (const r of rs) value = Math.max(value, r.max)
        return niceScale(value)
    }
    function number(v) { return v !== null && v !== undefined && v >= 0 ? Number(v).toFixed(v < 100 ? 1 : 0) : "—" }
    function msLabel(v) { return +Number(v).toFixed(2) + " ms" }
    function timeAt(x) { return Math.round(backend.viewStart + Math.max(0, Math.min(1, x)) * viewSpan) }
    function timeText(t, detailed) {
        if (t <= 0) return "—"
        const pattern = detailed ? "ddd d MMM, HH:mm:ss" : viewSpan >= 86400000 ? "ddd HH:mm" : "HH:mm:ss"
        return Qt.formatDateTime(new Date(t), pattern)
    }
    function percent(v) { return v >= 0 ? (+Number(v).toFixed(1)) + "%" : "—" }
    function findRow(hop) {
        for (const row of rows) if (row.hop === hop) return row
        return null
    }
    function selected(hop) {
        for (const c of charts) if (c.hop === hop) return true
        return false
    }
    function moveZoom(direction) {
        const i = viewOptions.indexOf(backend.viewSeconds)
        backend.setViewSeconds(viewOptions[Math.max(0, Math.min(viewOptions.length - 1, i + direction))])
    }
    function chooseExport() {
        if (!backend.sessionId) return
        exportDialog.open()
    }
    function launch() {
        let target = destination.text.trim()
        if (!target.length) { inputError = true; destination.forceActiveFocus(); return }
        inputError = false
        backend.startTrace(target, [0, 4, 6][family.currentIndex], [250, 500, 1000, 2500, 5000][interval.currentIndex])
    }
    onChartWidthChanged: pixelTimer.restart()
    Timer { id: pixelTimer; interval: 100; onTriggered: backend.setPixels(Math.max(1, Math.floor(app.chartWidth))) }
    Component.onCompleted: pixelTimer.restart()

    component Caption: Text {
        color: app.mute
        font.family: "Inter"
        font.pixelSize: 12
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
    component Heading: Text {
        color: app.ink
        font.family: "Inter"
        font.pixelSize: 18
        font.weight: Font.DemiBold
        verticalAlignment: Text.AlignVCenter
    }
    component Metric: Text {
        width: app.numWidth
        height: app.rowSize
        color: app.ink
        font.family: "monospace"
        font.pixelSize: app.width >= 1060 ? 12 : 11
        horizontalAlignment: Text.AlignRight
        rightPadding: 9
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
    component Rule: Rectangle { implicitHeight: 1; color: app.edge }

    ColumnLayout {
        id: workspace
        anchors.fill: parent
        spacing: 0
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 61
            color: "#17232d"
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 18
                anchors.rightMargin: 18
                spacing: 9
                Canvas {
                    implicitWidth: 30; implicitHeight: 30
                    onPaint: {
                        const p = getContext("2d")
                        p.clearRect(0,0,30,30); p.lineWidth = 2.8
                        p.lineCap = "round"; p.lineJoin = "round"; p.strokeStyle = app.accent
                        p.beginPath(); p.moveTo(1.5,19); p.lineTo(7.7,19); p.lineTo(13.1,6)
                        p.lineTo(17,24.4); p.lineTo(21.6,13); p.lineTo(28.4,13); p.stroke()
                    }
                }
                Heading { text: "Hopline"; font.pixelSize: 22 }
                Rectangle { color: app.edge; Layout.leftMargin: 9; Layout.rightMargin: 8; implicitWidth: 1; implicitHeight: 25 }
                C.TextField {
                    id: destination
                    objectName: "destinationInput"
                    Layout.fillWidth: true
                    Layout.maximumWidth: 410
                    Layout.preferredWidth: 235
                    Layout.minimumWidth: 110
                    implicitHeight: 32
                    placeholderText: "Hostname or IP address"
                    placeholderTextColor: "#91a2ae"
                    color: app.ink
                    font.pixelSize: 13
                    leftPadding: 10
                    selectByMouse: true
                    Accessible.name: "Trace destination"
                    onTextEdited: app.inputError = false
                    onAccepted: app.launch()
                    background: Rectangle {
                        radius: 5
                        color: "#101a23"
                        border.color: app.inputError ? app.lossColor : destination.activeFocus ? app.accent : "#3a4956"
                    }
                }
                TraceCombo { id: family; implicitWidth: 83; model: ["Auto IP", "IPv4", "IPv6"]; hint: "Address family" }
                TraceCombo { id: interval; implicitWidth: 85; model: ["0.25 s", "0.5 s", "1 s", "2.5 s", "5 s"]; currentIndex: 2; hint: "Interval between trace rounds" }
                TraceButton { text: "Start trace"; primary: true; accent: app.accent; onClicked: app.launch() }
                Item { Layout.fillWidth: true; visible: app.width > 950 }
            }
        }
        Rule { Layout.fillWidth: true }
        Rectangle {
            visible: backend.sessions.length > 0
            Layout.fillWidth: true
            implicitHeight: 42
            color: app.well
            ListView {
                id: sessionsList
                anchors.fill: parent
                anchors.leftMargin: 12; anchors.rightMargin: 12
                orientation: ListView.Horizontal
                clip: true
                spacing: 2
                model: backend.sessions.length
                currentIndex: { for (let i = 0; i < backend.sessions.length; i++) if (backend.sessions[i].id === backend.sessionId) return i; return -1 }
                highlightRangeMode: ListView.ApplyRange
                delegate: C.AbstractButton {
                    id: tab
                    required property int index
                    readonly property var modelData: backend.sessions[index] || ({})
                    readonly property bool active: modelData.id === backend.sessionId
                    height: sessionsList.height
                    padding: 0
                    width: Math.min(260, tabName.implicitWidth + 50)
                    hoverEnabled: true
                    Accessible.name: modelData.target + ", " + app.timeText(modelData.startMs, true)
                    onClicked: backend.selectSession(modelData.id)
                    background: Rectangle {
                        color: tab.active ? app.panel : tab.hovered ? "#1b2934" : "transparent"
                        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 2; color: app.accent; visible: tab.active }
                    }
                    contentItem: Row {
                        leftPadding: 12
                        spacing: 7
                        Rectangle { width: 6; height: 6; radius: 3; anchors.verticalCenter: parent.verticalCenter; color: tab.modelData.running ? app.accent : app.faint }
                        Caption { id: tabName; width: Math.min(210, implicitWidth); anchors.verticalCenter: parent.verticalCenter; color: tab.active ? app.ink : app.mute; text: tab.modelData.target }
                    }
                    C.ToolTip.visible: hovered
                    C.ToolTip.delay: 600
                    C.ToolTip.text: modelData.target + " · " + app.timeText(modelData.startMs, true)
                }
                WheelHandler {
                    enabled: sessionsList.contentWidth > sessionsList.width
                    acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                    onWheel: event => sessionsList.contentX = Math.max(0, Math.min(sessionsList.contentWidth - sessionsList.width, sessionsList.contentX - event.angleDelta.y))
                }
            }
        }
        Item {
            visible: backend.sessions.length === 0
            Layout.fillWidth: true
            Layout.fillHeight: true
            Column {
                anchors.centerIn: parent; spacing: 11
                Rectangle {
                    width: 62; height: 49; radius: 8; color: app.panel; border.color: app.edge
                    anchors.horizontalCenter: parent.horizontalCenter
                    Canvas {
                        anchors.fill: parent
                        onPaint: {
                            const c = getContext("2d")
                            c.clearRect(0,0,width,height); c.strokeStyle=app.accent; c.lineWidth=2
                            c.beginPath(); c.moveTo(11,31); c.lineTo(23,31); c.lineTo(29,17); c.lineTo(34,32); c.lineTo(40,23); c.lineTo(51,23); c.stroke()
                        }
                    }
                }
                Heading { text: "Start a trace"; anchors.horizontalCenter: parent.horizontalCenter }
                Caption { text: "Enter a hostname or IP address to compare latency and loss at each hop."; anchors.horizontalCenter: parent.horizontalCenter }
            }
        }
        C.SplitView {
            id: split
            visible: backend.sessions.length > 0
            Layout.fillHeight: true; Layout.fillWidth: true
            orientation: Qt.Vertical
            handle: Rectangle {
                implicitHeight: 7
                color: C.SplitHandle.pressed ? "#385160" : C.SplitHandle.hovered ? "#2d424f" : app.base
                Rectangle { anchors.centerIn: parent; width: 34; height: 2; radius: 1; color: "#4d5a64" }
            }
            Rectangle {
                id: routePanel
                color: app.panel
                C.SplitView.minimumHeight: 139
                C.SplitView.preferredHeight: Math.max(170, Math.min(split.height * 0.52, 95 + app.rows.length * app.rowSize))
                ColumnLayout {
                    anchors.fill: parent
                    spacing: 0
                    RowLayout {
                        Layout.fillWidth: true; Layout.leftMargin: 16; Layout.rightMargin: 16
                        implicitHeight: 50; spacing: 10
                        Heading { text: "Route profile" }
                        Caption { visible: app.width >= 875; text: backend.live ? app.rows.length + (app.rows.length === 1 ? " hop" : " hops")
                            : app.timeText(backend.focusStart, false) + " – " + app.timeText(backend.focusEnd, false) }
                        Item { Layout.fillWidth: true }
                        TraceButton { text: "Save PNG"; hint: "Save the visible graphs (Ctrl+Shift+S)"; onClicked: app.chooseExport() }
                        Caption { text: "Focus" }
                        TraceCombo {
                            implicitWidth: 85
                            model: ["Auto", "All", "10 sec", "30 sec", "1 min", "5 min", "15 min", "1 hr", "6 hr", "1 day"]
                            currentIndex: app.focusOptions.indexOf(backend.focusSeconds)
                            displayText: currentIndex >= 0 ? currentText : backend.focusSeconds + " sec"
                            onActivated: i => backend.setFocusSeconds(app.focusOptions[i])
                            hint: "Time window used for hop statistics"
                        }
                        TraceButton { objectName: "pauseButton"; text: backend.running ? "Pause" : "Resume"; onClicked: backend.toggleRunning(); hint: backend.running ? "Pause collection" : "Resume collection for this target" }
                    }
                    Rectangle {
                        Layout.fillWidth: true; implicitHeight: 29
                        color: "#1c2935"
                        Row {
                            anchors.left: parent.left; anchors.leftMargin: 12
                            Caption { width: app.hopWidth; height: 29; text: "   Hop"; font.weight: Font.DemiBold }
                            Caption { width: app.hostWidth; height: 29; text: "Address / host"; font.weight: Font.DemiBold }
                            Repeater {
                                model: ["Sent", "Loss", "Min", "Avg", "Max", "Last", "Jitter"]
                                Metric { required property string modelData; text: modelData; font.family: "Inter"; font.weight: Font.DemiBold; color: app.mute }
                            }
                            Caption { width: app.graphWidth; height: 29; leftPadding: 6; text: "Round-trip time"; font.weight: Font.DemiBold
                                HoverHandler { id: graphHeaderHover }
                                C.ToolTip.visible: graphHeaderHover.hovered
                                C.ToolTip.delay: 400
                                C.ToolTip.text: "Round-trip latency in milliseconds. Shading starts at 100 ms (amber) and 200 ms (red)."
                            }
                        }
                    }
                    Flickable {
                        id: routeScroll
                        Layout.fillHeight: true; Layout.fillWidth: true
                        clip: true
                        contentHeight: Math.max(height, app.rows.length * app.rowSize)
                        boundsBehavior: Flickable.StopAtBounds
                        flickableDirection: Flickable.VerticalFlick
                        C.ScrollBar.vertical: C.ScrollBar { policy: routeScroll.contentHeight > routeScroll.height ? C.ScrollBar.AsNeeded : C.ScrollBar.AlwaysOff }
                        Item {
                            id: routeContent
                            x: 12; width: routeScroll.width - 30
                            height: app.rows.length * app.rowSize
                            Column {
                                width: parent.width
                                Repeater {
                                    model: app.rows.length
                                    Rectangle {
                                        id: hopRow
                                        required property int index
                                        readonly property var modelData: app.rows[index] || ({})
                                        objectName: "profileRow" + (modelData.hop || "")
                                        readonly property bool checked: app.selected(modelData.hop)
                                        width: routeContent.width
                                        height: app.rowSize
                                        color: hover.hovered ? "#21313d" : index % 2 === 1 ? "#17242e" : app.panel
                                        HoverHandler { id: hover }
                                        MouseArea { x: 22; width: parent.width - x; height: parent.height; onClicked: backend.toggleHop(hopRow.modelData.hop) }
                                        Row {
                                            width: parent.width
                                            Item {
                                                height: app.rowSize; width: app.hopWidth
                                                C.AbstractButton {
                                                    id: check
                                                    anchors.verticalCenter: parent.verticalCenter
                                                    width: 21; height: 23
                                                    Accessible.name: "Timeline for hop " + hopRow.modelData.hop
                                                    Accessible.checkable: true; Accessible.checked: hopRow.checked
                                                    onClicked: backend.toggleHop(hopRow.modelData.hop)
                                                    background: Item {
                                                        Rectangle {
                                                            width: 12; height: 12; radius: 3; anchors.centerIn: parent
                                                            color: hopRow.checked ? app.accent : "transparent"
                                                            border.color: check.visualFocus ? "#ffffff" : hopRow.checked ? app.accent : "#637582"
                                                            Text { anchors.centerIn: parent; text: "✓"; font.pixelSize: 11; font.bold: true; color: "#122329"; visible: hopRow.checked }
                                                        }
                                                    }
                                                }
                                                Metric { x: 25; width: app.hopWidth - 30; horizontalAlignment: Text.AlignLeft; text: hopRow.modelData.hop; color: hopRow.checked ? app.accent : app.mute }
                                            }
                                            Caption {
                                                width: app.hostWidth; height: app.rowSize
                                                rightPadding: 7
                                                text: (hopRow.modelData.routeChanged ? "↪ " : "") + (hopRow.modelData.hostname || hopRow.modelData.address || "No reply")
                                                color: hopRow.modelData.destination ? app.accent : hopRow.modelData.address ? app.ink : app.faint
                                                font.family: "monospace"; font.pixelSize: 12
                                                C.ToolTip.visible: addressHover.hovered && !!hopRow.modelData.address
                                                C.ToolTip.text: (hopRow.modelData.hostname ? hopRow.modelData.hostname + " · " : "")
                                                    + (hopRow.modelData.address || "") + (hopRow.modelData.routeChanged
                                                    ? "\nResponder changed during focus · first " + hopRow.modelData.firstAddress : "")
                                                C.ToolTip.delay: 500
                                                HoverHandler { id: addressHover }
                                            }
                                            Metric { text: hopRow.modelData.sent }
                                            Item {
                                                width: app.numWidth; height: app.rowSize
                                                Rectangle {
                                                    visible: hopRow.modelData.loss > 0
                                                    x: 3; anchors.verticalCenter: parent.verticalCenter; height: 16; radius: 2
                                                    width: Math.max(2, (parent.width - 9) * Math.min(1, hopRow.modelData.loss / 100))
                                                    color: "#783940"
                                                }
                                                Metric { text: app.percent(hopRow.modelData.loss); color: hopRow.modelData.loss > 0 ? app.lossColor : app.mute }
                                            }
                                            Metric { text: app.number(hopRow.modelData.min) }
                                            Metric { text: app.number(hopRow.modelData.avg); font.bold: true }
                                            Metric { text: app.number(hopRow.modelData.max) }
                                            Metric { text: app.number(hopRow.modelData.cur) }
                                            Metric { text: app.number(hopRow.modelData.jitter); color: app.mute }
                                        }
                                    }
                                }
                            }
                            GraphView {
                                anchors.right: parent.right
                                width: app.graphWidth; height: parent.height
                                samples: app.rows; profile: true; ceiling: app.profileCeiling
                                rowHeight: app.rowSize; accent: app.accent
                            }
                        }
                        Caption {
                            visible: !app.rows.length; anchors.centerIn: parent
                            text: backend.running ? "Waiting for the first trace round…" : "No hop samples in this focus window"
                        }
                    }
                    Item {
                        Layout.fillWidth: true; implicitHeight: 24
                        Row {
                            x: 16; height: parent.height; spacing: 8
                            Caption { text: "● Avg"; color: app.accent; anchors.verticalCenter: parent.verticalCenter }
                            Caption { text: "↔ Min–max"; anchors.verticalCenter: parent.verticalCenter }
                            Caption { text: "× Last"; color: "#f4c778"; anchors.verticalCenter: parent.verticalCenter }
                        }
                        Item {
                            anchors.right: parent.right; anchors.rightMargin: 18; width: app.graphWidth; height: parent.height
                            Caption { x: 4; anchors.verticalCenter: parent.verticalCenter; text: "0" }
                            Caption { anchors.centerIn: parent; text: app.msLabel(app.profileCeiling / 2) }
                            Caption { anchors.right: parent.right; anchors.rightMargin: 3; anchors.verticalCenter: parent.verticalCenter; text: app.msLabel(app.profileCeiling) }
                        }
                    }
                }
            }
            Rectangle {
                id: timelineFrame
                C.SplitView.fillHeight: true
                C.SplitView.minimumHeight: 150
                color: app.panel
                ColumnLayout {
                    anchors.fill: parent
                    spacing: 0
                    RowLayout {
                        Layout.fillWidth: true; Layout.leftMargin: 16; Layout.rightMargin: 16
                        implicitHeight: 50; spacing: 9
                        Heading { text: "Timelines" }
                        Caption { visible: app.width > 970; text: "● RTT"; color: app.accent }
                        Caption { visible: app.width > 970; text: "▮ Loss"; color: app.lossColor }
                        Item { Layout.fillWidth: true }
                        Caption { text: "View" }
                        TraceCombo {
                            implicitWidth: 85
                            model: ["10 sec", "30 sec", "1 min", "5 min", "15 min", "1 hr", "6 hr", "1 day", "7 days"]
                            currentIndex: app.viewOptions.indexOf(backend.viewSeconds)
                            onActivated: i => backend.setViewSeconds(app.viewOptions[i])
                            hint: "Displayed duration · Ctrl+wheel to zoom"
                        }
                        TraceButton {
                            text: "‹"; font.pixelSize: 18; hint: "Previous half window"
                            onClicked: backend.panBy(-Math.round(app.viewSpan / 2))
                        }
                        TraceButton {
                            text: "›"; hint: "Next half window"
                            onClicked: backend.panBy(Math.round(app.viewSpan / 2))
                        }
                        TraceButton {
                            text: backend.live ? (backend.running ? "● Live view" : "● Latest") : (backend.running ? "Go live" : "Latest")
                            accent: app.accent; primary: backend.live
                            hint: "Follow the end of this trace"; onClicked: backend.goLive()
                        }
                    }
                    Flickable {
                        id: tracks
                        Layout.fillHeight: true; Layout.fillWidth: true
                        clip: true
                        contentHeight: Math.max(height, trackColumn.implicitHeight)
                        boundsBehavior: Flickable.StopAtBounds
                        flickableDirection: Flickable.VerticalFlick
                        C.ScrollBar.vertical: C.ScrollBar { policy: tracks.contentHeight > tracks.height ? C.ScrollBar.AsNeeded : C.ScrollBar.AlwaysOff }
                        Column {
                            id: trackColumn
                            x: 13; width: tracks.width - 26
                            Repeater {
                                model: app.charts.length
                                Item {
                                    id: track
                                    required property int index
                                    readonly property var modelData: app.charts[index] || ({hop: 0, buckets: []})
                                    readonly property var info: app.findRow(modelData.hop)
                                    width: trackColumn.width
                                    height: Math.max(76, tracks.height / Math.max(1, app.charts.length))
                                    Rectangle { anchors.fill: parent; color: app.well }
                                    Caption {
                                        x: 7; y: 8; width: app.chartLabelWidth - 13
                                        text: track.modelData.destination ? "Destination" : "Hop " + track.modelData.hop
                                        color: app.ink; font.weight: Font.DemiBold
                                    }
                                    Caption {
                                        x: 7; y: 28; width: app.chartLabelWidth - 13
                                        text: track.modelData.destination ? app.activeTarget
                                            : track.info ? (track.info.hostname || track.info.address || "No reply") : ""
                                        font.family: "monospace"; font.pixelSize: 11
                                    }
                                    Caption {
                                        x: 7; anchors.bottom: parent.bottom; anchors.bottomMargin: 9
                                        width: app.chartLabelWidth - 13
                                        text: app.cursorHop === track.modelData.hop && app.cursorBucket
                                            ? (app.cursorBucket.avg >= 0 ? app.msLabel(+app.cursorBucket.avg.toFixed(1)) : "No reply")
                                                + "  ·  " + app.percent(app.cursorBucket.loss) + " loss"
                                            : "0 – " + app.msLabel(app.timeCeiling)
                                        color: app.faint; font.pixelSize: 11
                                    }
                                    Item {
                                        id: plotArea
                                        x: app.chartLabelWidth; y: 5; width: app.chartWidth; height: parent.height - 10
                                        clip: true
                                        Rectangle {
                                            id: focusBand
                                            readonly property real startFraction: Math.max(0, Math.min(1, (backend.focusStart - backend.viewStart) / app.viewSpan))
                                            readonly property real endFraction: Math.max(0, Math.min(1, (backend.focusEnd - backend.viewStart) / app.viewSpan))
                                            x: startFraction * parent.width; width: Math.max(0, (endFraction - startFraction) * parent.width)
                                            height: parent.height; color: Qt.rgba(app.accent.r, app.accent.g, app.accent.b, 0.065)
                                            visible: width < parent.width - 1
                                            Rectangle { width: 1; height: parent.height; color: app.accent; opacity: 0.45 }
                                            Rectangle { anchors.right: parent.right; width: 1; height: parent.height; color: app.accent; opacity: 0.45 }
                                        }
                                        GraphView {
                                            anchors.fill: parent
                                            samples: track.modelData.buckets
                                            pixels: Math.max(1, backend.snapshot.pixels || 1)
                                            ceiling: app.timeCeiling; accent: app.accent
                                            maxGap: Math.max(3, app.chartWidth * (app.currentSession ? app.currentSession.intervalMs : 1000) * 3 / app.viewSpan)
                                        }
                                        Repeater {
                                            model: backend.snapshot.notes || []
                                            Rectangle {
                                                required property var modelData
                                                x: (modelData.timeMs - backend.viewStart) / app.viewSpan * plotArea.width
                                                width: 1; height: plotArea.height; color: "#d9b47c"
                                                Rectangle { width: 5; height: 5; color: parent.color }
                                            }
                                        }
                                        Rectangle {
                                            x: app.cursorFraction * parent.width
                                            visible: app.cursorFraction >= 0; width: 1; height: parent.height; color: app.ink; opacity: 0.75
                                        }
                                        MouseArea {
                                            id: chartMouse
                                            objectName: "timelineInput" + track.modelData.hop
                                            anchors.fill: parent
                                            acceptedButtons: Qt.LeftButton | Qt.RightButton
                                            hoverEnabled: true
                                            cursorShape: mouseDrag ? Qt.ClosedHandCursor : Qt.CrossCursor
                                            preventStealing: true
                                            property real startX: 0
                                            property real previousX: 0
                                            property bool mouseDrag: false
                                            function updateCursor(xp) {
                                                app.cursorFraction = Math.max(0, Math.min(1, xp / width))
                                                app.cursorHop = track.modelData.hop
                                                const samples = track.modelData.buckets
                                                const where = app.cursorFraction * (backend.snapshot.pixels || 1)
                                                let left = 0, right = samples.length
                                                while (left < right) {
                                                    const mid = (left + right) >> 1
                                                    if (samples[mid].x < where) left = mid + 1
                                                    else right = mid
                                                }
                                                let best = samples[left]
                                                if (left > 0 && (!best || where - samples[left - 1].x < best.x - where)) best = samples[left - 1]
                                                app.cursorBucket = best && Math.abs(best.x - where) <= Math.max(3, width * (app.currentSession ? app.currentSession.intervalMs : 1000) / app.viewSpan) ? best : null
                                            }
                                            onPressed: e => {
                                                startX = e.x; previousX = e.x; mouseDrag = false
                                                if (e.button === Qt.RightButton) app.openNote(e.x / width)
                                            }
                                            onPositionChanged: e => {
                                                if (pressedButtons & Qt.LeftButton) {
                                                    if (Math.abs(e.x - startX) > 4) mouseDrag = true
                                                    if (mouseDrag) {
                                                        const delta = Math.round((previousX - e.x) / width * app.viewSpan)
                                                        if (delta) backend.panBy(delta)
                                                        previousX = e.x
                                                    }
                                                }
                                                updateCursor(e.x)
                                            }
                                            onReleased: e => {
                                                if (e.button === Qt.LeftButton && !mouseDrag) backend.focusAt(app.timeAt(e.x / width))
                                                mouseDrag = false
                                            }
                                            onExited: { app.cursorFraction = -1; app.cursorBucket = null; app.cursorHop = -1 }
                                            onWheel: e => {
                                                if (e.modifiers & Qt.ControlModifier) { app.moveZoom(e.angleDelta.y > 0 ? -1 : 1); e.accepted = true }
                                                else e.accepted = false
                                            }
                                            C.ToolTip.visible: containsMouse && app.nearbyNote(app.cursorFraction) !== null && !noteDialog.visible
                                            C.ToolTip.delay: 350
                                            C.ToolTip.text: { const n = app.nearbyNote(app.cursorFraction); return n ? n.text : "" }
                                        }
                                    }
                                    Rule { anchors.bottom: parent.bottom; width: parent.width; height: 1 }
                                }
                            }
                        }
                        Caption {
                            anchors.centerIn: parent; visible: app.charts.length === 0
                            text: app.rows.length ? "Select a hop in the route profile to view its timeline." : "Waiting for timeline data…"
                        }
                    }
                    Item {
                        Layout.fillWidth: true; implicitHeight: 24
                        Item {
                            x: 13 + app.chartLabelWidth; width: app.chartWidth; height: parent.height
                            Repeater {
                                model: 5
                                Caption {
                                    required property int index
                                    anchors.verticalCenter: parent.verticalCenter
                                    x: (app.chartWidth - 1) * index / 4 - (index === 4 ? width : index === 0 ? 0 : width / 2)
                                    text: app.timeText(app.timeAt(index / 4), false); font.pixelSize: 11
                                }
                            }
                        }
                    }
                }
            }
        }
        Rule { Layout.fillWidth: true }
        Rectangle {
            Layout.fillWidth: true; implicitHeight: backend.sessions.length ? 37 : 31
            color: "#15202a"
            RowLayout {
                anchors.fill: parent; anchors.leftMargin: 14; anchors.rightMargin: 14; spacing: 8
                Rectangle { width: 6; height: 6; radius: 3; color: backend.running ? app.accent : app.faint }
                Caption {
                    Layout.fillWidth: true
                    text: app.exportStatus || (app.inputError ? "Enter a hostname or IP address." : backend.status || (backend.running ? "Collecting" : "Ready"))
                    color: app.inputError ? app.lossColor : app.mute
                }
                Caption {
                    visible: app.cursorFraction >= 0
                    text: app.timeText(app.timeAt(app.cursorFraction), true)
                }
                Caption { visible: app.currentSession !== null && app.width >= 820 && app.cursorFraction < 0; text: "Click to focus  ·  Drag to pan  ·  Right-click to annotate"; font.pixelSize: 11 }
                Caption { visible: !!app.currentSession; text: "Interval"; font.pixelSize: 11 }
                TraceCombo {
                    visible: !!app.currentSession; implicitHeight: 26; implicitWidth: 77
                    readonly property var choices: [250, 500, 1000, 2500, 5000]
                    model: ["0.25 s", "0.5 s", "1 s", "2.5 s", "5 s"]
                    currentIndex: app.currentSession ? choices.indexOf(app.currentSession.intervalMs) : 2
                    displayText: currentIndex >= 0 ? currentText : (app.currentSession ? app.currentSession.intervalMs / 1000 + " s" : "")
                    onActivated: i => backend.setIntervalMs(choices[i])
                    hint: "Change the interval of the selected trace"
                }
            }
        }
    }
    function nearbyNote(fraction) {
        if (fraction < 0) return null
        const stamp = timeAt(fraction), radius = 7 * viewSpan / chartWidth
        for (const n of (backend.snapshot.notes || []))
            if (Math.abs(n.timeMs - stamp) <= radius) return n
        return null
    }
    function openNote(fraction) {
        const note = nearbyNote(fraction)
        noteDialog.stamp = note ? note.timeMs : timeAt(fraction)
        noteDialog.existing = !!note
        noteText.text = note ? note.text : ""
        noteDialog.open()
        noteText.forceActiveFocus()
    }
    C.Dialog {
        id: noteDialog
        objectName: "noteDialog"
        modal: true
        width: Math.min(app.width - 40, 425)
        anchors.centerIn: parent
        padding: 18
        property double stamp: 0
        property bool existing: false
        background: Rectangle { radius: 8; color: app.panel; border.color: "#425461" }
        contentItem: ColumnLayout {
            spacing: 12
            Heading { text: noteDialog.existing ? "Edit annotation" : "Add annotation" }
            Caption { text: app.timeText(noteDialog.stamp, true) }
            C.TextField {
                id: noteText; Layout.fillWidth: true
                placeholderText: "What happened here?"; placeholderTextColor: app.faint
                color: app.ink; selectByMouse: true; maximumLength: 1024; Accessible.name: "Annotation"
                background: Rectangle { color: app.well; radius: 4; border.color: app.accent }
                onAccepted: if (text.trim()) { backend.saveNote(noteDialog.stamp, text.trim()); noteDialog.close() }
            }
            RowLayout {
                Layout.fillWidth: true
                TraceButton { visible: noteDialog.existing; text: "Delete"; onClicked: { backend.saveNote(noteDialog.stamp, ""); noteDialog.close() } }
                Item { Layout.fillWidth: true }
                TraceButton { text: "Cancel"; onClicked: noteDialog.close() }
                TraceButton { text: "Save"; enabled: noteText.text.trim().length > 0; primary: true; accent: app.accent; onClicked: { backend.saveNote(noteDialog.stamp, noteText.text.trim()); noteDialog.close() } }
            }
        }
    }
    Dialogs.FileDialog {
        id: exportDialog
        objectName: "exportDialog"
        title: "Save graph image"
        fileMode: Dialogs.FileDialog.SaveFile
        nameFilters: ["PNG image (*.png)"]
        defaultSuffix: "png"
        onAccepted: {
            const url = selectedFile
            Qt.callLater(() => {
                workspace.grabToImage(result => {
                    app.exportStatus = result.saveToFile(url) ? "Graph image saved." : "Could not save the graph image."
                    exportMessageTimer.restart()
                })
            })
        }
    }
    Timer { id: exportMessageTimer; interval: 4500; onTriggered: app.exportStatus = "" }
    Shortcut { sequence: "Ctrl+Shift+S"; onActivated: app.chooseExport() }
    Shortcut { sequence: "Ctrl+L"; onActivated: { destination.forceActiveFocus(); destination.selectAll() } }
}
