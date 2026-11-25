import QtQuick
import QtQuick.Controls as C
import QtQuick.Layouts
import QtCore as Core
import QtQuick.Dialogs as Dialogs
import Hopview.Graphics 1.0

C.ApplicationWindow {
    id: app
    width: 1220
    height: 800
    minimumWidth: 760
    minimumHeight: 530
    property bool startHidden: false
    visible: !startHidden
    title: activeTarget.length ? activeTarget + " — Hopview" : "Hopview"
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
    Core.Settings {
        id: preferences
        location: Core.StandardPaths.writableLocation(Core.StandardPaths.AppConfigLocation) + "/appearance.ini"
        property int scaleStep: 0
    }
    readonly property real uiScale: 1 + Math.max(-2, Math.min(8, preferences.scaleStep)) / 8
    function changeScale(step) {
        preferences.scaleStep = Math.max(-2, Math.min(8, step))
        preferences.sync()
    }
    readonly property var rows: backend.snapshot.rows || []
    readonly property var charts: backend.snapshot.charts || []
    readonly property var diagnosticTracks: {
        const tracks = Array.from(backend.snapshot.diagnostics || [])
        const settings = backend.diagnosticsSettings[String(backend.sessionId)] || {}
        for (const key of ["service", "interface"])
            if ((key === "service" ? settings.url : settings.interface) && !tracks.some(t => t.kind === key))
                tracks.push({kind:key, points:[]})
        return tracks
    }
    readonly property var currentSession: {
        for (const s of backend.sessions)
            if (s.id === backend.sessionId) return s
        return null
    }
    readonly property string activeTarget: currentSession ? currentSession.target : ""
    readonly property var focusOptions: [0, -1, 10, 30, 60, 300, 900, 3600, 21600, 86400]
    readonly property var viewOptions: [10, 30, 60, 300, 900, 3600, 21600, 86400, 604800]
    readonly property int rowSize: 29
    readonly property int numWidth: workspace.width >= 1060 ? 61 : 47
    readonly property int hopWidth: 69
    readonly property int graphWidth: Math.floor(Math.max(155, routeContent.width * 0.34))
    readonly property int hostWidth: Math.max(90, routeContent.width - graphWidth - 7 * numWidth - hopWidth)
    readonly property int chartLabelWidth: workspace.width >= 950 ? 171 : 127
    readonly property int axisRight: 18
    readonly property real chartWidth: Math.max(1, timelineFrame.width - 26 - chartLabelWidth - axisRight)
    readonly property real viewSpan: Math.max(1, backend.viewEnd - backend.viewStart)
    readonly property real profileCeiling: scaleCeiling(rows)
    readonly property real timeCeiling: niceScale(backend.snapshot.timeMax || 0)
    property real cursorFraction: -1
    function cursorSample(series, interval) {
        if (cursorFraction < 0 || !series.length) return null
        const pixels = backend.snapshot.pixels || 1
        const where = cursorFraction * pixels
        let left = 0, right = series.length
        while (left < right) {
            const mid = (left + right) >> 1
            if (series[mid].x < where) left = mid + 1
            else right = mid
        }
        let sample = series[left]
        if (left && (!sample || where - series[left - 1].x < sample.x - where)) sample = series[left - 1]
        return sample && Math.abs(sample.x - where) <= Math.max(1, pixels * interval / (2 * viewSpan)) ? sample : null
    }
    property double rangeAnchorMs: -1
    property double rangeEndMs: -1
    readonly property bool selectingRange: rangeAnchorMs >= 0 && rangeEndMs >= 0
    function failureText(f) {
        if (!f || !f.kind) return ""
        const name = {"local-unsent":qsTr("Local send failed"), "unreachable":qsTr("Unreachable"),
            "filtered":qsTr("Administratively filtered"), "packet-too-big":qsTr("Packet too big"),
            "icmp-error":qsTr("ICMP error")}[f.kind] || f.kind
        return name + (f.router ? " · " + f.router : "") + (f.type >= 0 ? " · ICMP " + f.type + "/" + f.code : "")
            + (f.mtu > 0 ? " · MTU " + f.mtu : "") + (f.kind === "local-unsent" ? " · errno " + f.errno : "")
    }
    function sourceLabel(info) {
        if (!info) return ""
        return info.location ? info.location + " · " + info.target : info.target
    }
    function profileLabel(info, measured) {
        if (!info) return ""
        const v = measured || {}
        const family = v.effectiveFamily || info.family
        let method = (v.effectiveProtocol || info.protocol || "auto").toUpperCase()
        const port = v.effectiveProtocol ? v.effectivePort : info.port
        if (port && (method === "UDP" || method === "TCP")) method += "/" + port
        if (method === "AUTO") method = qsTr("Auto")
        let label = (family ? "IPv" + family : "Auto IP") + " " + method
        if (info.payloadBytes && info.payloadBytes !== 16) label += " " + info.payloadBytes + " B"
        if (info.dscp) label += " DSCP " + info.dscp
        if (info.dontFragment) label += " DF"
        return label
    }
    function measurementLabel(info, measured) {
        if (!measured || !measured.effectiveProtocol) return qsTr("Effective settings unknown · configured %1").arg(profileLabel(info))
        let result = profileLabel(info, measured) + " · " + measured.resolvedAddress
        if (measured.effectiveInterface) result += " · " + measured.effectiveInterface
        if (measured.mixed) result += qsTr(" · settings changed within focus")
        if (measured.partial) result += qsTr(" · earlier settings unknown")
        if (info && info.location)
            result += measured.clockErrorMs >= 0 ? qsTr(" · clock offset %1 ± %2 ms").arg(measured.clockOffsetMs).arg(measured.clockErrorMs)
                        : qsTr(" · clock alignment unknown")
        return result
    }
    readonly property bool endpointMismatch: {
        const primary = backend.snapshot.measurement || {}
        return charts.some(c => c.comparisonId && (primary.mixed || c.measurement?.mixed ||
            (primary.resolvedAddress && c.measurement?.resolvedAddress && c.measurement.resolvedAddress !== primary.resolvedAddress)))
    }
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
        const iso = new Date(t).toISOString()
        if (detailed) return iso.replace("T"," ").replace("Z"," UTC")
        return viewSpan >= 86400000 ? iso.slice(5,16).replace("T"," ") : iso.slice(11, viewSpan < 10000 ? 23 : 19)
    }
    function durationText(ms) {
        if (ms < 1000) return Math.round(ms) + " ms"
        if (ms < 60000) return Number((ms / 1000).toFixed(2)) + " s"
        if (ms < 3600000) return Number((ms / 60000).toFixed(2)) + " min"
        return Number((ms / 3600000).toFixed(2)) + " hr"
    }
    function percent(v) { return v >= 0 ? (+Number(v).toFixed(1)) + "%" : "—" }
    function findRow(hop) {
        for (const row of rows) if (row.hop === hop) return row
        return null
    }
    function selected(hop) {
        for (const c of charts) if (!c.comparisonId && c.hop === hop) return true
        return false
    }
    function moveZoom(direction) {
        const seconds = viewSpan / 1000
        if (direction < 0) {
            for (let i = viewOptions.length - 1; i >= 0; --i)
                if (viewOptions[i] < seconds) { backend.setViewSeconds(viewOptions[i]); return }
        } else {
            for (const preset of viewOptions)
                if (preset > seconds) { backend.setViewSeconds(preset); return }
        }
    }
    function chooseExport() {
        if (!backend.sessionId) return
        exportDialog.open()
    }
    function correctionText(trace) {
        if (!trace || !trace.imported) return ""
        const ms = trace.manualCorrectionMs || 0
        return qsTr("UTC adjustment %1 ms").arg((ms > 0 ? "+" : "") + ms)
    }
    function chooseIncident(csv) {
        incidentDialog.csv = csv
        incidentDialog.session = backend.sessionId
        incidentDialog.peers = Array.from(backend.comparisonIds)
        incidentDialog.hops = Array.from(backend.selectedHops)
        incidentDialog.fromMs = Math.max(0, backend.focusStart)
        incidentDialog.throughMs = backend.focusEnd
        incidentDialog.open()
    }
    function launch() {
        let target = destination.text.trim()
        if (!target.length) { inputError = true; destination.forceActiveFocus(); return }
        inputError = false
        const cadence = [250, 500, 1000, 2500, 5000][interval.currentIndex]
        const dual = family.currentIndex === 3
        const af = [0, 4, 6, 4][family.currentIndex]
        const profile = probeControls.comparison || "off"
        if (dual && profile !== "off") {
            app.exportStatus = qsTr("Choose one address family for a size or DSCP comparison.")
            exportMessageTimer.restart()
            return
        }
        if (dual || profile !== "off") {
            backend.startTracePair(target, af, dual ? 6 : af, cadence, probeControls.protocol, probeControls.port,
                probeControls.interfaceName, probeControls.dontFragment, probeControls.payloadBytes,
                profile === "size" ? probeControls.comparisonPayloadBytes : probeControls.payloadBytes,
                probeControls.dscp, profile === "dscp" ? probeControls.comparisonDscp : probeControls.dscp)
        } else {
            backend.startTrace(target, af, cadence, probeControls.protocol, probeControls.port, probeControls.interfaceName,
                               false, probeControls.payloadBytes, probeControls.dscp, probeControls.dontFragment)
        }
    }
    onChartWidthChanged: pixelTimer.restart()
    onUiScaleChanged: pixelTimer.restart()
    Timer { id: pixelTimer; interval: 100; onTriggered: backend.setPixels(Math.max(1, Math.floor(app.chartWidth * app.uiScale))) }
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
        font.pixelSize: workspace.width >= 1060 ? 12 : 11
        horizontalAlignment: Text.AlignRight
        rightPadding: 9
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
    component Rule: Rectangle { implicitHeight: 1; color: app.edge }

    Flickable {
        id: viewport
        objectName: "workspaceViewport"
        anchors.fill: parent
        clip: true
        contentWidth: Math.max(width, app.minimumWidth * app.uiScale)
        contentHeight: Math.max(height, app.minimumHeight * app.uiScale)
        interactive: contentWidth > width || contentHeight > height
        boundsBehavior: Flickable.StopAtBounds
        C.ScrollBar.horizontal: C.ScrollBar {}
        C.ScrollBar.vertical: C.ScrollBar {}
    }

    ColumnLayout {
        id: workspace
        objectName: "workspace"
        parent: viewport.contentItem
        width: viewport.contentWidth / app.uiScale
        height: viewport.contentHeight / app.uiScale
        scale: app.uiScale
        transformOrigin: Item.TopLeft
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
                Heading { text: "Hopview"; font.pixelSize: 22 }
                Rectangle { color: app.edge; Layout.leftMargin: 9; Layout.rightMargin: 8; implicitWidth: 1; implicitHeight: 25 }
                C.TextField {
                    id: destination
                    visible: !backend.replayMode
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
                TraceCombo { id: family; visible: !backend.replayMode; implicitWidth: 83; model: ["Auto IP", "IPv4", "IPv6", "IPv4 + IPv6"]; hint: "Address family or paired dual-stack comparison" }
                TraceCombo { id: interval; visible: !backend.replayMode; implicitWidth: 85; model: ["0.25 s", "0.5 s", "1 s", "2.5 s", "5 s"]; currentIndex: 2; hint: "Interval between trace rounds" }
                ProbeControls { id: probeControls; visible: !backend.replayMode; }
                TraceButton { text: "Start trace"; visible: !backend.replayMode; primary: true; accent: app.accent; onClicked: app.launch() }
                Caption { visible: backend.replayMode; text: "Offline incident replay" }
                Item { Layout.fillWidth: true; visible: workspace.width > 950 || backend.replayMode }
                TraceButton {
                    visible: backend.sessions.length > 0 && workspace.width > 1060
                    text: qsTr("Site health"); hint: qsTr("Compare destination health across sites")
                    onClicked: fleet.open()
                }
                TraceButton {
                    visible: backend.collectorManagementAvailable && backend.sessions.length > 0 && workspace.width > 950
                    text: qsTr("Collectors…"); hint: qsTr("Enroll a VM or manage its targets")
                    onClicked: sourcesDialog.open()
                }
            }
        }
        Rule { Layout.fillWidth: true }
        Rectangle {
            visible: backend.sessions.length > 0 || backend.closedSessions.length > 0
            Layout.fillWidth: true
            implicitHeight: 42
            color: app.well
            ListView {
                id: sessionsList
                anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom
                anchors.right: reopenButton.visible ? reopenButton.left : parent.right
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
                    objectName: "sessionTab" + (modelData.id || "")
                    height: sessionsList.height
                    padding: 0
                    width: Math.min(280, tabName.implicitWidth + 66)
                    hoverEnabled: true
                    Accessible.name: app.sourceLabel(modelData) + ", " + app.timeText(modelData.startMs, true)
                    onClicked: backend.selectSession(modelData.id)
                    background: Rectangle {
                        color: tab.active ? app.panel : tab.hovered ? "#1b2934" : "transparent"
                        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 2; color: app.accent; visible: tab.active }
                    }
                    contentItem: Row {
                        leftPadding: 12
                        spacing: 7
                        Rectangle { width: 6; height: 6; radius: 3; anchors.verticalCenter: parent.verticalCenter; color: tab.modelData.running ? app.accent : app.faint }
                        Caption { id: tabName; width: Math.min(210, implicitWidth); anchors.verticalCenter: parent.verticalCenter; color: tab.active ? app.ink : app.mute; text: app.sourceLabel(tab.modelData) + " · " + app.profileLabel(tab.modelData, tab.modelData) }
                    }
                    TapHandler {
                        enabled: !backend.replayMode
                        acceptedButtons: Qt.MiddleButton
                        onTapped: backend.closeSession(tab.modelData.id)
                    }
                    C.ToolButton {
                        id: closeTab
                        visible: !backend.replayMode
                        objectName: "closeTab" + (tab.modelData.id || "")
                        anchors.right: parent.right; anchors.rightMargin: 3; anchors.verticalCenter: parent.verticalCenter
                        width: 25; height: 26
                        hoverEnabled: true
                        Accessible.name: qsTr("Close trace to %1").arg(tab.modelData.target)
                        onClicked: backend.closeSession(tab.modelData.id)
                        contentItem: Text {
                            text: "×"; color: closeTab.hovered || closeTab.visualFocus ? app.ink : app.faint
                            font.pixelSize: 17
                            horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
                        }
                        background: Rectangle {
                            radius: 4; color: closeTab.hovered ? "#344552" : "transparent"
                            border.color: closeTab.visualFocus ? app.accent : "transparent"
                        }
                        C.ToolTip.visible: hovered
                        C.ToolTip.delay: 450
                        C.ToolTip.text: tab.modelData.running ? qsTr("Stop and close trace. Recording is kept.") : qsTr("Close tab. Recording is kept.")
                    }
                    C.ToolTip.visible: hovered && !closeTab.hovered
                    C.ToolTip.delay: 600
                    C.ToolTip.text: modelData.target + " · " + app.timeText(modelData.startMs, true)
                                    + (modelData.imported ? "\n" + app.correctionText(modelData) : "")
                                    + "\n" + (modelData.protocol || "auto").toUpperCase()
                                    + (modelData.protocol === "auto" ? " (UDP/" + modelData.port + " fallback)" : modelData.port ? "/" + modelData.port : "")
                                    + (modelData.interfaceName ? " · " + modelData.interfaceName : "")
                }
                WheelHandler {
                    enabled: sessionsList.contentWidth > sessionsList.width
                    acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                    onWheel: event => sessionsList.contentX = Math.max(0, Math.min(sessionsList.contentWidth - sessionsList.width, sessionsList.contentX - event.angleDelta.y))
                }
            }
            TraceButton {
                id: reopenButton
                objectName: "reopenTraces"
                visible: backend.closedSessions.length > 0
                anchors.right: parent.right; anchors.rightMargin: 12; anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Reopen trace")
                hint: qsTr("Open a closed recording")
                onClicked: closedPopup.open()
                C.Popup {
                    id: closedPopup
                    objectName: "closedTracesPopup"
                    x: reopenButton.width - width
                    y: reopenButton.height + 4
                    scale: app.uiScale
                    transformOrigin: C.Popup.TopLeft
                    width: Math.min(330, workspace.width - 30)
                    height: Math.min(300, 8 + backend.closedSessions.length * 38)
                    padding: 4
                    focus: true
                    onOpened: { archiveList.currentIndex = 0; archiveList.forceActiveFocus() }
                    function restore(index) {
                        const session = backend.closedSessions[index]
                        if (!session) return
                        close()
                        backend.reopenSession(session.id)
                    }
                    background: Rectangle { radius: 5; color: "#1d2934"; border.color: "#425461" }
                    contentItem: ListView {
                        id: archiveList
                        clip: true
                        keyNavigationEnabled: true
                        model: closedPopup.visible ? backend.closedSessions : []
                        Keys.onReturnPressed: closedPopup.restore(currentIndex)
                        Keys.onEnterPressed: closedPopup.restore(currentIndex)
                        C.ScrollIndicator.vertical: C.ScrollIndicator {}
                        delegate: C.ItemDelegate {
                            id: archiveItem
                            required property var modelData
                            required property int index
                            objectName: "reopenTrace" + modelData.id
                            width: archiveList.width; height: 38
                            highlighted: archiveList.currentIndex === index
                            Accessible.name: modelData.target + " " + app.timeText(modelData.startMs, true)
                            onClicked: closedPopup.restore(index)
                            contentItem: RowLayout {
                                Caption { text: archiveItem.modelData.target; Layout.fillWidth: true; color: app.ink }
                                Caption { text: app.timeText(archiveItem.modelData.startMs, true); font.pixelSize: 11 }
                            }
                            background: Rectangle { radius: 3; color: archiveItem.highlighted || archiveItem.hovered ? "#344552" : "transparent" }
                        }
                    }
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
                Row {
                    anchors.horizontalCenter: parent.horizontalCenter; spacing: 8
                    TraceButton { visible: backend.groupNames.length > 0; text: qsTr("Saved comparisons…"); onClicked: compareDialog.open() }
                    TraceButton { text: qsTr("Site health…"); onClicked: fleet.open() }
                    TraceButton { text: "Open incident…"; onClicked: openIncident.open() }
                    TraceButton { objectName: "openCollectors"; visible: backend.collectorManagementAvailable; text: qsTr("Collectors…"); onClicked: sourcesDialog.open() }
                    TraceButton { visible: backend.backgroundAvailable; text: "Recording and alerts…"; onClicked: alertsDialog.open() }
                }
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
                        Caption { visible: workspace.width >= 875; text: backend.live ? app.rows.length + (app.rows.length === 1 ? " hop" : " hops")
                            : app.timeText(backend.focusStart, false) + " – " + app.timeText(backend.focusEnd, false) }
                        Item { Layout.fillWidth: true }
                        TraceButton {
                            id: actionsButton; objectName: "traceActions"
                            text: "Trace actions ▾"
                            onClicked: actionsMenu.open()
                            C.Menu {
                                id: actionsMenu
                                y: actionsButton.height
                                width: 230
                                background: Rectangle { color: app.panel; border.color: app.edge; radius: 5 }
                                ActionItem { visible: backend.collectorManagementAvailable; text: qsTr("Collectors…"); onTriggered: sourcesDialog.open() }
                                ActionItem { text: qsTr("Site health…"); onTriggered: fleet.open() }
                                ActionItem { text: qsTr("Choose comparison sites…"); onTriggered: siteComparison.open() }
                                ActionItem { text: qsTr("Compare time ranges…"); onTriggered: baselineDialog.open() }
                                ActionItem { text: qsTr("Mark focus as baseline"); onTriggered: backend.markBaseline() }
                                ActionItem { objectName: "diagnosticsAction"; text: qsTr("Service and interface…"); enabled: !backend.replayMode && !!app.currentSession && !app.currentSession.remote && !app.currentSession.imported; onTriggered: diagnosticSettings.open() }
                                ActionItem { text: qsTr("Find incident by UTC time…"); onTriggered: incidentNavigator.open() }
                                ActionItem { text: qsTr("Hop responders and inventory…"); onTriggered: hopResponders.open() }
                                ActionItem { text: qsTr("Recording storage…"); enabled: !backend.replayMode; onTriggered: storageDialog.open() }
                                ActionItem { text: "Route history…"; onTriggered: routeHistory.open() }
                                ActionItem { text: "Recording and alerts…"; enabled: backend.backgroundAvailable; onTriggered: alertsDialog.open() }
                                C.MenuSeparator {}
                                ActionItem { objectName: "copySummary"; text: qsTr("Copy incident summary"); onTriggered: backend.copyIncidentSummary() }
                                ActionItem { text: "Save graph as PNG…"; onTriggered: app.chooseExport() }
                                ActionItem { text: "Export focus as CSV…"; onTriggered: app.chooseIncident(true) }
                                ActionItem { objectName: "saveIncidentAction"; text: "Save replayable incident…"; onTriggered: app.chooseIncident(false) }
                                ActionItem { text: qsTr("Add recording to comparison…"); onTriggered: addRecording.open() }
                                ActionItem { text: "Open incident…"; onTriggered: openIncident.open() }
                            }
                        }
                        Caption { text: "Focus" }
                        TraceCombo {
                            implicitWidth: 85
                            model: ["Auto", "All", "10 sec", "30 sec", "1 min", "5 min", "15 min", "1 hr", "6 hr", "1 day"]
                            currentIndex: app.focusOptions.indexOf(backend.focusSeconds)
                            displayText: currentIndex >= 0 ? currentText : backend.focusSeconds + " sec"
                            onActivated: i => backend.setFocusSeconds(app.focusOptions[i])
                            hint: "Time window used for hop statistics"
                        }
                        TraceButton { objectName: "pauseButton"; visible: !backend.replayMode && !app.currentSession?.remoteLive && !app.currentSession?.imported; text: backend.running ? "Pause" : "Resume"; onClicked: backend.toggleRunning(); hint: backend.running ? "Pause collection" : "Resume collection for this target" }
                    }
                    Caption {
                        id: impactLabel
                        objectName: "destinationImpact"
                        readonly property var impact: backend.snapshot.impact || {}
                        visible: impact.sent !== undefined
                        Layout.fillWidth: true; Layout.leftMargin: 16; Layout.rightMargin: 16
                        Layout.preferredHeight: 22
                        color: impact.state === "affected" ? "#f7b891" : app.ink
                        text: impact.description ? qsTr("Destination %1/%2 replies · %3").arg(impact.received).arg(impact.sent).arg(impact.description) : ""
                        HoverHandler { id: impactHover }
                        C.ToolTip.visible: impactHover.hovered
                        C.ToolTip.text: impactLabel.text + qsTr("\nPersistence requires evidence in both halves of the focus. Latency is compared with the preceding equal interval.")
                    }
                    Caption {
                        objectName: "roundReplies"
                        Layout.fillWidth: true; Layout.leftMargin: 12; Layout.rightMargin: 12
                        visible: backend.live && backend.running && backend.provisionalReplies.length > 0
                        text: qsTr("Latest replies · ") + backend.provisionalReplies
                        elide: Text.ElideRight
                        C.ToolTip.visible: previewHover.hovered
                        C.ToolTip.text: backend.provisionalReplies
                        HoverHandler { id: previewHover }
                    }
                    Caption {
                        Layout.fillWidth: true; Layout.leftMargin: 12
                        visible: !!backend.snapshot.endpointFailure?.kind
                        text: qsTr("Latest endpoint error in focus · ") + app.failureText(backend.snapshot.endpointFailure)
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
                                        MouseArea { x: 22; width: parent.width - x; height: parent.height; acceptedButtons: Qt.LeftButton | Qt.RightButton
                                            onClicked: e => { if (e.button === Qt.RightButton) hopResponders.showHop(hopRow.modelData.hop); else backend.toggleHop(hopRow.modelData.hop) } }
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
                                                text: (hopRow.modelData.routeChanged ? "↪ " : "") + (hopRow.modelData.hostname || hopRow.modelData.address || app.failureText(hopRow.modelData.lastFailure) || "No reply") + (hopRow.modelData.identity?.prefix
                                                      ? "  " + [hopRow.modelData.identity.asn,hopRow.modelData.identity.pop,hopRow.modelData.identity.peer,hopRow.modelData.identity.circuit,hopRow.modelData.identity.prefix].filter(Boolean).join(" · ") : "")
                                                color: hopRow.modelData.destination ? app.accent : hopRow.modelData.address ? app.ink : app.faint
                                                font.family: "monospace"; font.pixelSize: 12
                                                C.ToolTip.visible: addressHover.hovered && (!!hopRow.modelData.address || !!hopRow.modelData.lastFailure?.kind)
                                                C.ToolTip.text: (hopRow.modelData.lastFailure?.kind ? qsTr("Latest error in focus · ") + app.failureText(hopRow.modelData.lastFailure) + "\n" : "") + (hopRow.modelData.hostname ? hopRow.modelData.hostname + " · " : "")
                                                    + (hopRow.modelData.address || "") + (hopRow.modelData.identity?.source ? "\n" + hopRow.modelData.identity.prefix + " · " + hopRow.modelData.identity.source + " · " + hopRow.modelData.identity.updated : "") + (hopRow.modelData.routeChanged
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
                                                    color: backend.snapshot.impact?.state === "answered" ? "#394b58" : "#783940"
                                                }
                                                Metric { text: app.percent(hopRow.modelData.loss); color: hopRow.modelData.loss > 0 && backend.snapshot.impact?.state !== "answered" ? app.lossColor : app.mute }
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
                        Heading { text: qsTr("Timelines · UTC") }
                        Caption { visible: !!backend.snapshot.resolution && backend.snapshot.resolution !== "Raw probes"
                            text: backend.snapshot.resolution || ""; color: "#e2bd86" }
                        Caption { visible: workspace.width > 970; text: "● RTT"; color: app.accent }
                        Caption { visible: workspace.width > 970; text: "▮ Loss"; color: app.lossColor }
                        Caption {
                            visible: workspace.width > 1020 && app.timeCeiling > 100
                            text: "■ 100–200 ms"; color: "#d5b777"
                        }
                        Caption {
                            visible: workspace.width > 1100 && app.timeCeiling > 200
                            text: "■ >200 ms"; color: "#ce9398"
                        }
                        Item { Layout.fillWidth: true }
                        TraceButton {
                            objectName: "compareTargets"
                            visible: backend.sessions.length > 1 || backend.groupNames.length > 0
                            text: backend.comparisonIds.length ? "Compare (" + backend.comparisonIds.length + ")" : "Compare…"
                            onClicked: compareDialog.open()
                        }
                        Caption { text: "View" }
                        TraceCombo {
                            objectName: "timelineDuration"
                            implicitWidth: 92
                            model: ["10 sec", "30 sec", "1 min", "5 min", "15 min", "1 hr", "6 hr", "1 day", "7 days"]
                            currentIndex: app.viewOptions.indexOf(app.viewSpan / 1000)
                            displayText: currentIndex >= 0 ? currentText : app.durationText(app.viewSpan)
                            onActivated: i => backend.setViewSeconds(app.viewOptions[i])
                            hint: "Drag on a timeline to zoom to a range. Ctrl+wheel changes duration."
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
                    Caption {
                        visible: app.endpointMismatch; Layout.fillWidth: true; Layout.leftMargin: 16
                        color: "#f7ca92"; text: qsTr("Compared tracks include different or changing endpoints. Inspect each track’s probe settings.")
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
                                    readonly property bool peer: !!modelData.comparisonId
                                    readonly property var settings: peer ? modelData.measurement : backend.snapshot.measurement
                                    readonly property var source: peer ? modelData : app.currentSession
                                    readonly property var info: peer ? null : app.findRow(modelData.hop)
                                    readonly property int intervalMs: modelData.intervalMs || (app.currentSession ? app.currentSession.intervalMs : 1000)
                                    readonly property var cursorSample: app.cursorSample(modelData.buckets, intervalMs)
                                    readonly property var trackNotes: peer ? (modelData.notes || []) : (backend.snapshot.notes || [])
                                    readonly property string trackKey: peer ? "peer" + modelData.comparisonId : "hop" + modelData.hop
                                    width: trackColumn.width
                                    height: Math.max(76, tracks.height / Math.max(1, app.charts.length + app.diagnosticTracks.length))
                                    Rectangle { anchors.fill: parent; color: app.well }
                                    Caption {
                                        x: 7; y: 8; width: app.chartLabelWidth - 13
                                        text: track.peer || track.modelData.destination ? (track.source?.location || (track.peer ? qsTr("Local") : qsTr("Destination"))) + " · " + app.profileLabel(track.source, track.settings) : "Hop " + track.modelData.hop
                                        color: app.ink; font.weight: Font.DemiBold
                                    }
                                    Caption {
                                        x: 7; y: 28; width: app.chartLabelWidth - 13
                                        text: track.peer ? track.modelData.target : track.modelData.destination ? app.activeTarget
                                            : track.info ? (track.info.hostname || track.info.address || "No reply") : ""
                                        font.family: "monospace"; font.pixelSize: 11
                                    }
                                    MouseArea {
                                        width: app.chartLabelWidth - 8; height: 48
                                        acceptedButtons: Qt.NoButton; hoverEnabled: true
                                        C.ToolTip.visible: containsMouse && (track.peer || track.modelData.destination)
                                        C.ToolTip.text: app.measurementLabel(track.source, track.settings)
                                    }
                                    Caption {
                                        x: 7; anchors.bottom: parent.bottom; anchors.bottomMargin: 9
                                        width: app.chartLabelWidth - 13
                                        objectName: "cursorValue" + track.trackKey
                                        text: track.cursorSample
                                            ? (track.cursorSample.avg >= 0 ? app.msLabel(+track.cursorSample.avg.toFixed(1)) : "No reply")
                                                + "  ·  " + app.percent(track.cursorSample.loss) + " loss"
                                            : app.cursorFraction >= 0 ? "No samples"
                                            : !track.peer && track.modelData.destination && backend.savedBaseline.p95 >= 0
                                              ? qsTr("Baseline p95 %1 ms").arg(backend.savedBaseline.p95.toFixed(1))
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
                                            baselineP95: !track.peer && track.modelData.destination ? (backend.savedBaseline.p95 ?? -1) : -1
                                            maxGap: Math.max(3, app.chartWidth * track.intervalMs * 3 / app.viewSpan)
                                        }
                                        Repeater {
                                            model: track.trackNotes.length
                                            Rectangle {
                                                required property int index
                                                readonly property var modelData: track.trackNotes[index] || ({timeMs: 0})
                                                x: (modelData.timeMs - backend.viewStart) / app.viewSpan * plotArea.width
                                                width: 1; height: plotArea.height; color: "#d9b47c"
                                                Rectangle { width: 5; height: 5; color: parent.color }
                                            }
                                        }
                                        Rectangle {
                                            x: app.cursorFraction * parent.width
                                            visible: app.cursorFraction >= 0 && !app.selectingRange; width: 1; height: parent.height; color: app.ink; opacity: 0.75
                                        }
                                        Rectangle {
                                            visible: app.selectingRange
                                            x: (Math.min(app.rangeAnchorMs, app.rangeEndMs) - backend.viewStart) / app.viewSpan * parent.width
                                            width: Math.abs(app.rangeEndMs - app.rangeAnchorMs) / app.viewSpan * parent.width
                                            height: parent.height
                                            color: Qt.rgba(app.accent.r, app.accent.g, app.accent.b, 0.18)
                                            border.width: 1
                                            border.color: app.accent
                                        }
                                        MouseArea {
                                            id: chartMouse
                                            objectName: track.peer ? "compareInput" + track.modelData.comparisonId : "timelineInput" + track.modelData.hop
                                            anchors.fill: parent
                                            acceptedButtons: Qt.LeftButton | Qt.RightButton
                                            hoverEnabled: true
                                            cursorShape: panGesture && mouseDrag ? Qt.ClosedHandCursor : Qt.CrossCursor
                                            preventStealing: true
                                            property real startX: 0
                                            property real previousX: 0
                                            property bool mouseDrag: false
                                            property bool panGesture: false
                                            property double pressViewStart: 0
                                            property double pressSpan: 0
                                            function dragTime(xp) {
                                                return Math.round(pressViewStart + Math.max(0, Math.min(1, xp / width)) * pressSpan)
                                            }
                                            function clearDrag() {
                                                app.rangeAnchorMs = -1
                                                app.rangeEndMs = -1
                                                mouseDrag = false
                                                panGesture = false
                                            }
                                            function updateCursor(xp) {
                                                app.cursorFraction = Math.max(0, Math.min(1, xp / width))
                                            }
                                            onPressed: e => {
                                                startX = e.x; previousX = e.x; mouseDrag = false
                                                if (e.button === Qt.RightButton) { if (!track.peer) app.openNote(e.x / width); return }
                                                panGesture = !!(e.modifiers & Qt.ShiftModifier)
                                                pressViewStart = backend.viewStart
                                                pressSpan = app.viewSpan
                                            }
                                            onPositionChanged: e => {
                                                if (pressedButtons & Qt.LeftButton) {
                                                    if (Math.abs(e.x - startX) > 4) mouseDrag = true
                                                    if (mouseDrag && panGesture) {
                                                        const delta = Math.round((previousX - e.x) / width * app.viewSpan)
                                                        if (delta) backend.panBy(delta)
                                                        previousX = e.x
                                                    } else if (mouseDrag) {
                                                        app.rangeAnchorMs = dragTime(startX)
                                                        app.rangeEndMs = dragTime(e.x)
                                                    }
                                                }
                                                updateCursor(e.x)
                                            }
                                            onReleased: e => {
                                                if (e.button !== Qt.LeftButton) return
                                                if (mouseDrag || Math.abs(e.x - startX) > 4) {
                                                    if (!panGesture) backend.zoomToRange(dragTime(startX), dragTime(e.x))
                                                } else {
                                                    backend.focusAt(dragTime(e.x))
                                                }
                                                clearDrag()
                                            }
                                            onCanceled: clearDrag()
                                            onExited: app.cursorFraction = -1
                                            onWheel: e => {
                                                if (e.modifiers & Qt.ControlModifier) { app.moveZoom(e.angleDelta.y > 0 ? -1 : 1); e.accepted = true }
                                                else e.accepted = false
                                            }
                                            readonly property var hoveredNote: containsMouse && !noteDialog.visible ? app.nearbyNote(app.cursorFraction, track.trackNotes) : null
                                            C.ToolTip.visible: hoveredNote !== null
                                            C.ToolTip.delay: 350
                                            C.ToolTip.text: hoveredNote ? hoveredNote.text : ""
                                        }
                                    }
                                    Rule { anchors.bottom: parent.bottom; width: parent.width; height: 1 }
                                }
                            }
                            Repeater {
                                model: app.diagnosticTracks.length
                                DiagnosticTrack {
                                    required property int index
                                    host: app
                                    series: app.diagnosticTracks[index] || ({kind:"service",points:[]})
                                    width: trackColumn.width; labelWidth: app.chartLabelWidth; plotWidth: app.chartWidth
                                    height: Math.max(82, tracks.height / Math.max(1, app.charts.length + app.diagnosticTracks.length))
                                }
                            }
                        }
                        Caption {
                            anchors.centerIn: parent; visible: app.charts.length + app.diagnosticTracks.length === 0
                            text: app.rows.length ? "Select a hop in the route profile to view its timeline." : "Waiting for timeline data…"
                        }
                    }
                    Item {
                        Layout.fillWidth: true
                        implicitHeight: lane.events.length ? 17 : 0
                        visible: implicitHeight > 0
                        Caption { x: 16; height: 17; text: qsTr("Events · UTC"); font.pixelSize: 10 }
                        Item {
                            id: lane
                            readonly property var events: backend.snapshot.events || []
                            x: 13 + app.chartLabelWidth; width: app.chartWidth; height: parent.height
                            clip: true
                            Repeater {
                                model: Math.min(500, lane.events.length)
                                Rectangle {
                                    id: marker
                                    required property int index
                                    readonly property var modelData: lane.events[index] || ({t: 0, kind: "", text: ""})
                                    x: (modelData.t - backend.viewStart) / app.viewSpan * (lane.width - 5)
                                    width: 5; height: 12; radius: 2
                                    color: ({route:"#c99bf4",alert:"#ee8581",gap:"#95a2ad",note:"#e5c576",service:"#7dcabb","interface":"#88aedf"})[modelData.kind] || app.faint
                                    C.ToolTip.visible: hoverMarker.hovered
                                    C.ToolTip.text: hoverMarker.hovered ? new Date(modelData.t).toISOString() + " · " + modelData.text : ""
                                    HoverHandler { id: hoverMarker }
                                    TapHandler { onTapped: incidentNavigator.openAt(marker.modelData.t) }
                                }
                            }
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
                    text: app.selectingRange ? "Zoom to " + app.durationText(Math.abs(app.rangeAnchorMs - app.rangeEndMs))
                                             : app.timeText(app.timeAt(app.cursorFraction), true)
                }
                Caption { visible: app.currentSession !== null && workspace.width >= 820 && app.cursorFraction < 0; text: "Click to focus  ·  Drag to zoom  ·  Shift-drag to pan  ·  Right-click to annotate"; font.pixelSize: 11 }
                Caption { visible: !!app.currentSession; text: "Interval"; font.pixelSize: 11 }
                TraceCombo {
                    enabled: !backend.replayMode && !app.currentSession?.remoteLive && !app.currentSession?.imported; visible: !!app.currentSession; implicitHeight: 26; implicitWidth: 77
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
    function nearbyNote(fraction, notes = backend.snapshot.notes) {
        if (fraction < 0) return null
        const stamp = timeAt(fraction), radius = 7 * viewSpan / chartWidth
        for (const n of (notes || []))
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
        width: Math.min(app.width / app.uiScale - 40, 425)
        scale: app.uiScale
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
    component ActionItem: C.MenuItem {
        id: action
        implicitHeight: 34
        contentItem: Caption { text: action.text; color: action.enabled ? app.ink : app.faint; leftPadding: 8 }
        background: Rectangle { color: action.highlighted ? "#344552" : "transparent" }
    }
    DiagnosticSettings { id: diagnosticSettings; uiScale: app.uiScale }
    AlertsDialog { id: alertsDialog; parent: C.Overlay.overlay; uiScale: app.uiScale }
    SiteComparison { id: siteComparison; parent: C.Overlay.overlay; uiScale: app.uiScale
        source: app.currentSession; profile: app.profileLabel(app.currentSession) }
    FleetDialog { id: fleet; parent: C.Overlay.overlay; uiScale: app.uiScale }
    RemoteSources { id: sourcesDialog; parent: C.Overlay.overlay; uiScale: app.uiScale; activeTrace: app.currentSession }
    BaselineDialog { id: baselineDialog; parent: C.Overlay.overlay; uiScale: app.uiScale }
    IncidentNavigator { id: incidentNavigator; parent: C.Overlay.overlay; uiScale: app.uiScale }
    StorageDialog { id: storageDialog; parent: C.Overlay.overlay; uiScale: app.uiScale }
    HopResponders { id: hopResponders; parent: C.Overlay.overlay; uiScale: app.uiScale }
    RouteHistory { id: routeHistory; parent: C.Overlay.overlay; uiScale: app.uiScale }
    C.Dialog {
        id: compareDialog
        objectName: "compareDialog"
        modal: true; focus: true; parent: C.Overlay.overlay
        anchors.centerIn: parent; scale: app.uiScale
        width: Math.min(460, app.width / app.uiScale - 30)
        padding: 16
        background: Rectangle { color: app.panel; border.color: app.edge; radius: 6 }
        contentItem: ColumnLayout {
            spacing: 10
            Heading { text: "Compare destinations" }
            RowLayout {
                visible: !!app.currentSession
                TraceButton { text: qsTr("All matching sites"); onClicked: { backend.compareSites(); compareDialog.close() } }
                TraceButton { text: qsTr("Choose sites…"); onClicked: { compareDialog.close(); siteComparison.open() } }
            }
            RowLayout {
                visible: backend.groupNames.length > 0
                TraceCombo { id: groupPicker; objectName: "savedGroupPicker"; model: backend.groupNames; Layout.fillWidth: true; hint: qsTr("Saved comparison groups") }
                TraceButton { text: qsTr("Open group"); onClicked: { backend.openGroup(groupPicker.currentText); compareDialog.close() } }
                TraceButton { text: qsTr("Remove"); hint: qsTr("Remove this group from saved comparisons"); onClicked: backend.deleteGroup(groupPicker.currentText) }
            }
            ListView {
                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(contentHeight, Math.max(70, app.height / app.uiScale - 230))
                clip: true
                model: compareDialog.visible ? backend.sessions.filter(s => s.id !== backend.sessionId) : []
                C.ScrollBar.vertical: C.ScrollBar {}
                delegate: C.CheckDelegate {
                    id: peerChoice
                    required property var modelData
                    objectName: "compareChoice" + modelData.id
                    width: ListView.view.width
                    checked: backend.comparisonIds.includes(modelData.id)
                    text: app.sourceLabel(modelData) + "  ·  " + (modelData.family ? "IPv" + modelData.family : "Auto IP")
                    Accessible.name: text
                    onClicked: backend.toggleComparison(modelData.id)
                    background: Rectangle { color: peerChoice.hovered ? "#344552" : app.well }
                    contentItem: Column {
                        leftPadding: 6
                        Caption { text: peerChoice.text; color: app.ink; width: peerChoice.width - 60 }
                        Caption { text: app.timeText(peerChoice.modelData.startMs, true) + (peerChoice.modelData.imported ? "  ·  " + app.correctionText(peerChoice.modelData) : ""); font.pixelSize: 11 }
                    }
                }
            }
            RowLayout {
                visible: !backend.replayMode && backend.comparisonIds.length > 0
                C.TextField {
                    id: groupName; objectName: "groupName"; Layout.fillWidth: true; maximumLength: 96
                    placeholderText: qsTr("Name this comparison"); color: app.ink; placeholderTextColor: app.faint
                    background: Rectangle { radius: 4; color: app.well; border.color: app.edge }
                    onAccepted: if (text.trim()) backend.saveGroup(text.trim())
                }
                TraceButton {
                    objectName: "saveGroup"; enabled: groupName.text.trim().length > 0
                    text: backend.groupNames.includes(groupName.text.trim()) ? qsTr("Update group") : qsTr("Save group")
                    onClicked: backend.saveGroup(groupName.text.trim())
                }
            }
            RowLayout {
                TraceButton { text: qsTr("Add recording…"); onClicked: { compareDialog.close(); addRecording.open() } }
                Item { Layout.fillWidth: true }
                TraceButton { text: "Done"; onClicked: compareDialog.close() }
            }
        }
    }
    Dialogs.FileDialog {
        id: incidentDialog
        objectName: "incidentExportDialog"
        property bool csv: false
        property double session
        property var peers: []
        property var hops: [0]
        property double fromMs
        property double throughMs
        fileMode: Dialogs.FileDialog.SaveFile
        title: (csv ? "Export CSV" : "Save incident") + " · " + app.timeText(fromMs, false) + " – " + app.timeText(throughMs, false)
        nameFilters: csv ? ["CSV data (*.csv)"] : ["Hopview incident (*.hopview)"]
        defaultSuffix: csv ? "csv" : "hopview"
        onAccepted: backend.exportIncident(session, fromMs, throughMs, selectedFile, csv, peers, hops)
    }
    Dialogs.FileDialog {
        id: addRecording
        title: qsTr("Add recording to this comparison")
        nameFilters: ["Incident recordings (*.hopview *.hopline)", "All files (*)"]
        onAccepted: {
            alignment.file = selectedFile
            fallbackSite.text = ""
            adjustment.text = "0"
            alignment.open()
        }
    }
    C.Dialog {
        id: alignment
        objectName: "recordingAlignment"
        property url file
        modal: true; focus: true; parent: C.Overlay.overlay; anchors.centerIn: parent
        width: Math.min(460, app.width / app.uiScale - 30); padding: 16; scale: app.uiScale
        background: Rectangle { color: app.panel; border.color: app.edge; radius: 6 }
        contentItem: ColumnLayout {
            spacing: 8
            Heading { text: qsTr("Align recording") }
            Caption { text: qsTr("Site name for traces without a recorded location") }
            C.TextField {
                id: fallbackSite; Layout.fillWidth: true; maximumLength: 255
                placeholderText: qsTr("e.g. Melbourne office"); color: app.ink; placeholderTextColor: app.faint
                background: Rectangle { radius: 4; color: app.well; border.color: app.edge }
            }
            Caption { text: qsTr("Add to recorded UTC timestamps (milliseconds)") }
            C.TextField {
                id: adjustment; Layout.fillWidth: true; text: "0"; color: app.ink; maximumLength: 16
                validator: RegularExpressionValidator { regularExpression: /[+-]?[0-9]{1,14}/ }
                background: Rectangle { radius: 4; color: app.well; border.color: app.edge }
            }
            RowLayout {
                Item { Layout.fillWidth: true }
                TraceButton { text: qsTr("Cancel"); onClicked: alignment.close() }
                TraceButton {
                    text: qsTr("Add to comparison"); enabled: adjustment.acceptableInput
                    onClicked: { backend.addIncident(alignment.file, fallbackSite.text.trim(), Number(adjustment.text)); alignment.close() }
                }
            }
        }
    }
    Dialogs.FileDialog {
        id: openIncident
        title: "Open incident"
        nameFilters: ["Incident recordings (*.hopview *.hopline)", "All files (*)"]
        onAccepted: backend.openReplay(selectedFile)
    }
    Connections {
        target: backend
        function onIncidentAdded(ok, message) { app.exportStatus = message; exportMessageTimer.restart() }
        function onIncidentExported(ok, message) { app.exportStatus = message; exportMessageTimer.restart() }
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
                viewport.grabToImage(result => {
                    app.exportStatus = result.saveToFile(url) ? "Graph image saved." : "Could not save the graph image."
                    exportMessageTimer.restart()
                })
            })
        }
    }
    Timer { id: exportMessageTimer; interval: 4500; onTriggered: app.exportStatus = "" }
    Shortcut { sequence: "Ctrl+Shift+S"; onActivated: app.chooseExport() }
    Shortcut { sequence: "Ctrl+L"; onActivated: { destination.forceActiveFocus(); destination.selectAll() } }
    Shortcut { sequences: ["Ctrl++", "Ctrl+=", "Ctrl+Shift+=", "Ctrl+Shift++"]; onActivated: app.changeScale(preferences.scaleStep + 1) }
    Shortcut { sequence: "Ctrl+-"; onActivated: app.changeScale(preferences.scaleStep - 1) }
    Shortcut { sequence: "Ctrl+0"; onActivated: app.changeScale(0) }
}
