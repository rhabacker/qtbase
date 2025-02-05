/****************************************************************************
**
** Copyright (C) 2018 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of the plugins of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:GPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3 or (at your option) any later version
** approved by the KDE Free Qt Foundation. The licenses are as published by
** the Free Software Foundation and appearing in the file LICENSE.GPL3
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-3.0.html.
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "qwasmscreen.h"
#include "qwasmwindow.h"
#include "qwasmeventtranslator.h"
#include "qwasmcompositor.h"
#include "qwasmintegration.h"
#include "qwasmstring.h"

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <QtGui/private/qeglconvenience_p.h>
#ifndef QT_NO_OPENGL
# include <QtGui/private/qeglplatformcontext_p.h>
#endif
#include <qpa/qwindowsysteminterface.h>
#include <QtCore/qcoreapplication.h>
#include <QtGui/qguiapplication.h>
#include <private/qhighdpiscaling_p.h>

using namespace emscripten;

QT_BEGIN_NAMESPACE

const char * QWasmScreen::m_canvasResizeObserverCallbackContextPropertyName = "data-qtCanvasResizeObserverCallbackContext";

QWasmScreen::QWasmScreen(const emscripten::val &containerOrCanvas)
    : m_container(containerOrCanvas)
    , m_canvas(emscripten::val::undefined())
    , m_compositor(new QWasmCompositor(this))
    , m_eventTranslator(new QWasmEventTranslator())
{
    // Each screen is backed by a html canvas element. Use either
    // a user-supplied canvas or create one as a child of the user-
    // supplied root element.
    std::string tagName = containerOrCanvas["tagName"].as<std::string>();
    if (tagName == "CANVAS" || tagName == "canvas") {
        m_canvas = containerOrCanvas;
    } else {
        // Create the canvas (for the correct document) as a child of the container
        m_canvas = containerOrCanvas["ownerDocument"].call<emscripten::val>("createElement", std::string("canvas"));
        containerOrCanvas.call<void>("appendChild", m_canvas);
        std::string screenId = std::string("qtcanvas_") + std::to_string(uint32_t(this));
        m_canvas.set("id", screenId);

        // Make the canvas occupy 100% of parent
        emscripten::val style = m_canvas["style"];
        style.set("width", std::string("100%"));
        style.set("height", std::string("100%"));
    }

    // Configure canvas
    emscripten::val style = m_canvas["style"];
    style.set("border", std::string("0px none"));
    style.set("background-color", std::string("white"));

    // Set contenteditable so that the canvas gets clipboard events,
    // then hide the resulting focus frame, and reset the cursor.
    m_canvas.set("contentEditable", std::string("true"));
    // set inputmode to none to stop mobile keyboard opening
    // when user clicks  anywhere on the canvas.
    m_canvas.set("inputmode", std::string("none"));
    style.set("outline", std::string("0px solid transparent"));
    style.set("caret-color", std::string("transparent"));
    style.set("cursor", std::string("default"));

    // Disable the default context menu; Qt applications typically
    // provide custom right-click behavior.
    m_onContextMenu = std::make_unique<qstdweb::EventCallback>(m_canvas, "contextmenu", [](emscripten::val event){
        event.call<void>("preventDefault");
    });
    
    // Create "specialHTMLTargets" mapping for the canvas. Normally, Emscripten
    // uses the html element id when targeting elements, for example when registering
    // event callbacks. However, this approach is limited to supporting single-document
    // apps/ages only, since Emscripten uses the main document to look up the element.
    // As a workaround for this, Emscripten supports registering custom mappings in the
    // "specialHTMLTargets" object. Add a mapping for the canvas for this screen.
    EM_ASM({
        specialHTMLTargets["!qtcanvas_" + $0] = Emval.toValue($1);
    }, uint32_t(this), m_canvas.as_handle());

    // Install event handlers on the container/canvas. This must be
    // done after the canvas has been created above.
    m_compositor->initEventHandlers();

    updateQScreenAndCanvasRenderSize();
    m_canvas.call<void>("focus");
}

QWasmScreen::~QWasmScreen()
{
    EM_ASM({
        specialHTMLTargets["!qtcanvas_" + $0] = undefined;
    }, uint32_t(this));

    m_canvas.set(m_canvasResizeObserverCallbackContextPropertyName, emscripten::val(intptr_t(0)));
    destroy();
}

void QWasmScreen::destroy()
{
    m_compositor->destroy();
}

QWasmScreen *QWasmScreen::get(QPlatformScreen *screen)
{
    return static_cast<QWasmScreen *>(screen);
}

QWasmScreen *QWasmScreen::get(QScreen *screen)
{
    return get(screen->handle());
}

QWasmCompositor *QWasmScreen::compositor()
{
    return m_compositor.get();
}

QWasmEventTranslator *QWasmScreen::eventTranslator()
{
    return m_eventTranslator.get();
}

emscripten::val QWasmScreen::container() const
{
    return m_container;
}

emscripten::val QWasmScreen::canvas() const
{
    return m_canvas;
}

// Returns the html element id for the screen's canvas.
QString QWasmScreen::canvasId() const
{
    return QWasmString::toQString(m_canvas["id"]);
}

// Returns the canvas _target_ id, for use with Emscripten's
// event registration functions. The target id is a globally
// unique id, unlike the html element id which is only unique
// within one html document. See specialHtmlTargets.
QString QWasmScreen::canvasTargetId() const
{
    return QStringLiteral("!qtcanvas_") + QString::number(int32_t(this));
}

QRect QWasmScreen::geometry() const
{
    return m_geometry;
}

int QWasmScreen::depth() const
{
    return m_depth;
}

QImage::Format QWasmScreen::format() const
{
    return m_format;
}

QDpi QWasmScreen::logicalDpi() const
{
    emscripten::val dpi = emscripten::val::module_property("qtFontDpi");
    if (!dpi.isUndefined()) {
        qreal dpiValue = dpi.as<qreal>();
        return QDpi(dpiValue, dpiValue);
    }
    const qreal defaultDpi = 96;
    return QDpi(defaultDpi, defaultDpi);
}

qreal QWasmScreen::devicePixelRatio() const
{
    // window.devicePixelRatio gives us the scale factor between CSS and device pixels.
    // This property reflects hardware configuration, and also browser zoom on desktop.
    //
    // window.visualViewport.scale gives us the zoom factor on mobile. If the html page is
    // configured with "<meta name="viewport" content="width=device-width">" then this scale
    // factor will be 1. Omitting the viewport configuration typically results on a zoomed-out
    // viewport, with a scale factor <1. User pinch-zoom will change the scale factor; an event
    // handler is installed in the QWasmIntegration constructor. Changing zoom level on desktop
    // does not appear to change visualViewport.scale.
    //
    // The effective devicePixelRatio is the product of these two scale factors, upper-bounded
    // by window.devicePixelRatio in order to avoid e.g. allocating a 10x widget backing store.
    double dpr = emscripten::val::global("window")["devicePixelRatio"].as<double>();
    emscripten::val visualViewport = emscripten::val::global("window")["visualViewport"];
    double scale = visualViewport.isUndefined() ? 1.0 : visualViewport["scale"].as<double>();
    double effectiveDevicePixelRatio = std::min(dpr * scale, dpr);
    return qreal(effectiveDevicePixelRatio);
}

QString QWasmScreen::name() const
{
    return canvasId();
}

QPlatformCursor *QWasmScreen::cursor() const
{
    return const_cast<QWasmCursor *>(&m_cursor);
}

void QWasmScreen::resizeMaximizedWindows()
{
    if (!screen())
        return;
    QPlatformScreen::resizeMaximizedWindows();
}

QWindow *QWasmScreen::topWindow() const
{
    return m_compositor->keyWindow();
}

QWindow *QWasmScreen::topLevelAt(const QPoint &p) const
{
    return m_compositor->windowAt(p);
}

void QWasmScreen::invalidateSize()
{
    m_geometry = QRect();
}

void QWasmScreen::setGeometry(const QRect &rect)
{
    m_geometry = rect;
    QWindowSystemInterface::handleScreenGeometryChange(QPlatformScreen::screen(), geometry(), availableGeometry());
    resizeMaximizedWindows();
}

void QWasmScreen::updateQScreenAndCanvasRenderSize()
{
    // The HTML canvas has two sizes: the CSS size and the canvas render size.
    // The CSS size is determined according to standard CSS rules, while the
    // render size is set using the "width" and "height" attributes. The render
    // size must be set manually and is not auto-updated on CSS size change.
    // Setting the render size to a value larger than the CSS size enables high-dpi
    // rendering.

    QByteArray canvasSelector = canvasTargetId().toUtf8();
    double css_width;
    double css_height;
    emscripten_get_element_css_size(canvasSelector.constData(), &css_width, &css_height);
    QSizeF cssSize(css_width, css_height);

    QSizeF canvasSize = cssSize * devicePixelRatio();

    m_canvas.set("width", canvasSize.width());
    m_canvas.set("height", canvasSize.height());

    // Returns the html elments document/body position
    auto getElementBodyPosition = [](const emscripten::val &element) -> QPoint {
        emscripten::val bodyRect = element["ownerDocument"]["body"].call<emscripten::val>("getBoundingClientRect");
        emscripten::val canvasRect = element.call<emscripten::val>("getBoundingClientRect");
        return QPoint (canvasRect["left"].as<int>() - bodyRect["left"].as<int>(),
                       canvasRect["top"].as<int>() - bodyRect["top"].as<int>());
    };

    setGeometry(QRect(getElementBodyPosition(m_canvas), cssSize.toSize()));
    m_compositor->requestUpdateAllWindows();
}

void QWasmScreen::canvasResizeObserverCallback(emscripten::val entries, emscripten::val)
{
    int count = entries["length"].as<int>();
    if (count == 0)
        return;
    emscripten::val entry = entries[0];
    QWasmScreen *screen =
        reinterpret_cast<QWasmScreen *>(entry["target"][m_canvasResizeObserverCallbackContextPropertyName].as<intptr_t>());
    if (!screen) {
        qWarning() << "QWasmScreen::canvasResizeObserverCallback: missing screen pointer";
        return;
    }

    // We could access contentBoxSize|contentRect|devicePixelContentBoxSize on the entry here, but
    // these are not universally supported across all browsers. Get the sizes from the canvas instead.
    screen->updateQScreenAndCanvasRenderSize();
}

EMSCRIPTEN_BINDINGS(qtCanvasResizeObserverCallback) {
    emscripten::function("qtCanvasResizeObserverCallback", &QWasmScreen::canvasResizeObserverCallback);
}

void QWasmScreen::installCanvasResizeObserver()
{
    emscripten::val ResizeObserver = emscripten::val::global("ResizeObserver");
    if (ResizeObserver == emscripten::val::undefined())
        return; // ResizeObserver API is not available
    emscripten::val resizeObserver = ResizeObserver.new_(emscripten::val::module_property("qtCanvasResizeObserverCallback"));
    if (resizeObserver == emscripten::val::undefined())
        return; // Something went horribly wrong

    // We need to get back to this instance from the (static) resize callback;
    // set a "data-" property on the canvas element.
    m_canvas.set(m_canvasResizeObserverCallbackContextPropertyName, emscripten::val(intptr_t(this)));

    resizeObserver.call<void>("observe", m_canvas);
}

QT_END_NAMESPACE
