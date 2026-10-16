import QtQuick
import QtQuick.Window
import QtWebEngine

Window {
    width: 1280
    height: 720
    visible: true
    title: "Leak test - " + backendName + " - " + secondsLeft + " s left"

    WebEngineView {
        anchors.fill: parent
        url: "qrc:/page.html"
    }
}
