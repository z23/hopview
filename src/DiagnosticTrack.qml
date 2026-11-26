import QtQuick
import QtQuick.Controls as C
import Hopview.Graphics 1.0

Item {
    id: track
    required property var host
    required property var series
    required property real labelWidth
    required property real plotWidth
    property bool service: series.kind === "service"
    readonly property var points: series.points || []
    readonly property var values: service ? ["dns","connect","tls","firstByte"] : ["rx","tx"]
    readonly property var colors: service ? ["#82aaff","#8ad6b4","#e6b779","#cb9be8"] : ["#82aaff","#8ad6b4"]
    readonly property real ceiling: {
        let max = 0
        for (const p of points)
            for (const key of values)
                max = Math.max(max, p[key] || 0)
        return host.niceScale(max)
    }
    readonly property double start: backend.snapshot.viewStart || 0
    readonly property double span: Math.max(1, (backend.snapshot.viewEnd || 0) - start)
    function near(fraction) {
        if (fraction < 0 || !points.length) return null
        const time = start + fraction * span
        let l = 0, r = points.length
        while(l < r) {
            const m = (l + r) >> 1
            if (points[m].t < time) l = m + 1
            else r = m
        }
        let best = null, distance = Infinity
        for(let i = Math.max(0,l-2); i < Math.min(points.length,l+2); ++i) {
            const p = points[i]
            for (const item of [p.marker,p]) {
                if (!item) continue
                const d = Math.abs(item.t-time)
                if (d < distance && d <= Math.max(span/Math.max(1, plotWidth)*2, item.periodMs/2)) {
                    distance=d;best=item
                }
            }
        }
        return best
    }
    readonly property var sample: near(host.cursorFraction)
    function formatRate(v) {
        if (v === undefined || v < 0) return "—"
        const scale = v >= 1000000 ? 1000000 : v >= 1000 ? 1000 : 1
        return Number((v/scale).toFixed(1)) + (scale === 1000000 ? " MB/s" : scale === 1000 ? " kB/s" : " B/s")
    }
    function ms(v) {return v >= 0 ? Number(v.toFixed(1)) + " ms" : "—"}
    function detail(p) {
        if (!p) return qsTr("No sample")
        if (service)
            return p.url + "\n" + [p.address || "—", p.family ? "IPv" + p.family : "", p.status ? "HTTP " + p.status : ""].filter(s=>s).join("  ") +
                   "\nDNS " + ms(p.dns) + "   TCP " + ms(p.connect) + "   TLS " + ms(p.tls) +
                   "\n" + qsTr("First byte wait") + " " + ms(p.firstByte) + "   TTFB " + ms(p.ttfb) + "   " + qsTr("Total") + " " + ms(p.total) +
                   (p.error ? "\n" + p.error : "")
        function delta(n) { return n < 0 ? "—" : n }
        return p.name + "   RX " + formatRate(p.rx) + "   TX " + formatRate(p.tx) +
               "\n" + qsTr("Drops RX/TX") + " " + delta(p.rxDrops) + "/" + delta(p.txDrops) +
               "   " + qsTr("Errors RX/TX") + " " + delta(p.rxErrors) + "/" + delta(p.txErrors) +
               (p.carrier < 0 ? "" : p.carrier ? "\n" + qsTr("Carrier up") : "\n" + qsTr("Carrier down")) +
               (p.ap ? "   BSSID " + p.ap : "") + (p.event ? "\n" + p.event : "")
    }
    Rectangle { anchors.fill: parent; color: host.well }
    component Label: Text {
        textFormat: Text.PlainText
        color: host.mute
        font.family: "Inter"; font.pixelSize: 11
        elide: Text.ElideRight
    }
    Column {
        x: 7; y: 6; spacing: 2; width: labelWidth - 13
        Label { width: parent.width; font.bold: true; color: host.ink; text: service ? qsTr("Service timing") : qsTr("Interface activity") }
        Row {
            spacing: 4
            Repeater {
                model: service ? ["DNS","TCP","TLS",qsTr("Wait")] : ["RX","TX"]
                Label { required property int index; required property string modelData; color: track.colors[index]; text:modelData }
            }
        }
        Label { width: parent.width; text: !sample ? "0 – " + (service ? ms(ceiling) : formatRate(ceiling))
                : service ? (sample.status ? "HTTP " + sample.status + "  ·  " : "") + ms(sample.total)
                : "↓ " + formatRate(sample.rx) + "   ↑ " + formatRate(sample.tx) }
        Label { width: parent.width; text: sample ? (sample.event || (service ? sample.address : sample.name)) :
                            points.length ? (service ? points[points.length-1].url : points[points.length-1].name) : qsTr("No samples in view") }
    }
    Item {
        id: plot
        x: labelWidth; y: 5; width: plotWidth; height: parent.height - 10
        clip: true
        Rectangle {
            readonly property real a: Math.max(0,Math.min(1,(backend.focusStart-track.start)/track.span))
            readonly property real b: Math.max(0,Math.min(1,(backend.focusEnd-track.start)/track.span))
            x: a*parent.width; width: Math.max(0,(b-a)*parent.width); height: parent.height
            color: Qt.rgba(host.accent.r,host.accent.g,host.accent.b,0.07)
            visible: width < parent.width-1
        }
        GraphView {
            id: graph
            objectName: track.service ? "serviceDiagnosticGraph" : "interfaceDiagnosticGraph"
            anchors.fill: parent
            diagnostic: true
            service: track.service
            samples: track.points
            ceiling: track.ceiling
            startMs: track.start
            spanMs: track.span
        }
        Rectangle {
            visible: host.cursorFraction >= 0 && !host.selectingRange
            x: host.cursorFraction*parent.width; height: parent.height; width: 1; color: host.ink; opacity: 0.75
        }
        Rectangle {
            visible: host.selectingRange
            x: (Math.min(host.rangeAnchorMs,host.rangeEndMs)-track.start)/track.span*parent.width
            width: Math.abs(host.rangeEndMs-host.rangeAnchorMs)/track.span*parent.width; height: parent.height
            color: Qt.rgba(host.accent.r,host.accent.g,host.accent.b,0.18); border.color: host.accent
        }
        MouseArea {
            id: mouse
            anchors.fill: parent; hoverEnabled: true; preventStealing: true; cursorShape: Qt.CrossCursor
            property real initialX
            property real previousX
            property double initialTime
            property double initialSpan
            property bool pan
            property bool drag
            function time(xp) { return Math.round(initialTime + Math.max(0,Math.min(1,xp/width))*initialSpan) }
            function clearRange() {host.rangeAnchorMs=-1;host.rangeEndMs=-1;drag=false}
            onPressed: e=>{initialX=previousX=e.x;initialTime=track.start;initialSpan=track.span;pan=!!(e.modifiers&Qt.ShiftModifier);drag=false}
            onPositionChanged: e=>{
                host.cursorFraction=Math.max(0,Math.min(1,e.x/width))
                if(!(pressedButtons&Qt.LeftButton)) return
                if(Math.abs(e.x-initialX)>4) drag=true
                if(drag && pan){backend.panBy(Math.round((previousX-e.x)/width*initialSpan));previousX=e.x}
                else if(drag){host.rangeAnchorMs=time(initialX);host.rangeEndMs=time(e.x)}
            }
            onReleased: e=>{
                if(drag && !pan) backend.zoomToRange(time(initialX),time(e.x))
                else if(!drag) backend.focusAt(time(e.x))
                clearRange()
            }
            onCanceled: clearRange()
            onExited: host.cursorFraction=-1
            C.ToolTip.visible: containsMouse && !!track.sample && !pressed
            C.ToolTip.delay: 250
            C.ToolTip.text: track.detail(track.sample).replace(/&/g,"&amp;").replace(/</g,"&lt;").replace(/>/g,"&gt;").replace(/\n/g,"<br>")
        }
    }
    Rectangle {anchors.bottom: parent.bottom; width: parent.width; height: 1; color: host.edge}
}
