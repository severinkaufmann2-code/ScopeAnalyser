#include "scope/style/StyleKit.h"

#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QEvent>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QStyle>
#include <QVariant>

#include <cmath>
#include <functional>

namespace scope::style {

namespace {

// Readable on both the light and dark theme.
const QColor kNeutralIcon(0x8a, 0x91, 0x9c);
const QColor kAccentIcon(0x41, 0x8f, 0xe0);

// All glyphs are drawn in a 24×24 design space.
void drawGlyph(QPainter& p, Glyph g, const QColor& c) {
    QPen pen(c, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);

    switch (g) {
        case Glyph::AppLogo: {
            p.setPen(Qt::NoPen);
            p.setBrush(kAccentIcon);
            p.drawRoundedRect(QRectF(1.5, 1.5, 21, 21), 5, 5);
            QPen wave(Qt::white, 2.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
            p.setPen(wave);
            p.setBrush(Qt::NoBrush);
            QPainterPath path(QPointF(5, 12));
            path.cubicTo(8, 4.5, 9.5, 4.5, 12, 12);
            path.cubicTo(14.5, 19.5, 16, 19.5, 19, 12);
            p.drawPath(path);
            break;
        }
        case Glyph::RecordTab:
            p.drawEllipse(QPointF(12, 12), 7.2, 7.2);
            p.setPen(Qt::NoPen);
            p.setBrush(c);
            p.drawEllipse(QPointF(12, 12), 3.2, 3.2);
            break;
        case Glyph::AnalyseTab: {
            QPainterPath path(QPointF(3, 12));
            path.cubicTo(6.5, 3, 8.5, 3, 12, 12);
            path.cubicTo(15.5, 21, 17.5, 21, 21, 12);
            p.drawPath(path);
            break;
        }
        case Glyph::ConvertTab: {
            p.drawLine(QPointF(5, 8.5), QPointF(17, 8.5));
            p.drawLine(QPointF(19, 15.5), QPointF(7, 15.5));
            p.setPen(Qt::NoPen);
            p.setBrush(c);
            QPolygonF headR;
            headR << QPointF(16.2, 4.8) << QPointF(20.6, 8.5) << QPointF(16.2, 12.2);
            p.drawPolygon(headR);
            QPolygonF headL;
            headL << QPointF(7.8, 11.8) << QPointF(3.4, 15.5) << QPointF(7.8, 19.2);
            p.drawPolygon(headL);
            break;
        }
        case Glyph::RecordDot:
            p.setPen(Qt::NoPen);
            p.setBrush(c);
            p.drawEllipse(QPointF(12, 12), 5.8, 5.8);
            break;
        case Glyph::Stop:
            p.setPen(Qt::NoPen);
            p.setBrush(c);
            p.drawRoundedRect(QRectF(7, 7, 10, 10), 2, 2);
            break;
        case Glyph::Plus:
            p.drawLine(QPointF(12, 5), QPointF(12, 19));
            p.drawLine(QPointF(5, 12), QPointF(19, 12));
            break;
        case Glyph::Minus:
            p.drawLine(QPointF(5, 12), QPointF(19, 12));
            break;
        case Glyph::Pencil: {
            p.save();
            p.translate(12, 12);
            p.rotate(45);
            p.setPen(Qt::NoPen);
            p.setBrush(c);
            p.drawRoundedRect(QRectF(-2.1, -9.5, 4.2, 12.5), 1.4, 1.4);
            QPolygonF tip;
            tip << QPointF(-2.1, 4.4) << QPointF(2.1, 4.4) << QPointF(0, 8.8);
            p.drawPolygon(tip);
            p.restore();
            break;
        }
        case Glyph::FolderOpen: {
            QPainterPath path;
            path.moveTo(3.2, 18.5);
            path.lineTo(3.2, 6.5);
            path.quadTo(3.2, 5, 4.7, 5);
            path.lineTo(9.2, 5);
            path.lineTo(11.2, 7.5);
            path.lineTo(19.3, 7.5);
            path.quadTo(20.8, 7.5, 20.8, 9);
            path.lineTo(20.8, 18.5);
            path.quadTo(20.8, 19.5, 19.8, 19.5);
            path.lineTo(4.2, 19.5);
            path.quadTo(3.2, 19.5, 3.2, 18.5);
            p.drawPath(path);
            break;
        }
        case Glyph::Save: {
            // Arrow descending into a tray.
            p.drawLine(QPointF(12, 4), QPointF(12, 13.6));
            QPainterPath head;
            head.moveTo(8.4, 10.2);
            head.lineTo(12, 14);
            head.lineTo(15.6, 10.2);
            p.drawPath(head);
            QPainterPath tray;
            tray.moveTo(4.5, 14.5);
            tray.lineTo(4.5, 18.5);
            tray.quadTo(4.5, 19.5, 5.5, 19.5);
            tray.lineTo(18.5, 19.5);
            tray.quadTo(19.5, 19.5, 19.5, 18.5);
            tray.lineTo(19.5, 14.5);
            p.drawPath(tray);
            break;
        }
        case Glyph::Refresh: {
            QRectF r(5, 5, 14, 14);
            QPainterPath arc;
            arc.arcMoveTo(r, 40);
            arc.arcTo(r, 40, 270);
            p.drawPath(arc);
            // Arrowhead at the arc's start (40°), pointing clockwise.
            const QPointF tip(r.center().x() + r.width() / 2 * std::cos(40 * M_PI / 180),
                              r.center().y() - r.height() / 2 * std::sin(40 * M_PI / 180));
            p.setPen(Qt::NoPen);
            p.setBrush(c);
            QPolygonF head;
            head << tip + QPointF(-1.4, -4.4) << tip + QPointF(4.4, 0.6) << tip + QPointF(-2.6, 2.4);
            p.drawPolygon(head);
            break;
        }
        case Glyph::Plug: {
            p.drawLine(QPointF(9.3, 3.5), QPointF(9.3, 8));
            p.drawLine(QPointF(14.7, 3.5), QPointF(14.7, 8));
            p.drawRoundedRect(QRectF(6.5, 8, 11, 7), 2, 2);
            p.drawLine(QPointF(12, 15), QPointF(12, 20.5));
            break;
        }
        case Glyph::Unplug: {
            p.drawLine(QPointF(9.3, 3.5), QPointF(9.3, 8));
            p.drawLine(QPointF(14.7, 3.5), QPointF(14.7, 8));
            p.drawRoundedRect(QRectF(6.5, 8, 11, 7), 2, 2);
            p.drawLine(QPointF(12, 15), QPointF(12, 20.5));
            p.drawLine(QPointF(4.5, 20), QPointF(20, 4.5));
            break;
        }
    }
}

QIcon paintIcon(const std::function<void(QPainter&, const QRectF&)>& painterFn) {
    QIcon out;
    for (int size : {16, 20, 24, 32, 48}) {
        for (int dpr : {1, 2}) {
            QPixmap pm(size * dpr, size * dpr);
            pm.fill(Qt::transparent);
            {
                QPainter p(&pm);
                p.setRenderHint(QPainter::Antialiasing);
                painterFn(p, QRectF(0, 0, size * dpr, size * dpr));
            }
            pm.setDevicePixelRatio(dpr);
            out.addPixmap(pm);
        }
    }
    return out;
}

}  // namespace

QIcon icon(Glyph glyph, const QColor& color) {
    const QColor c = color.isValid() ? color : kNeutralIcon;
    return paintIcon([glyph, c](QPainter& p, const QRectF& r) {
        p.scale(r.width() / 24.0, r.height() / 24.0);
        drawGlyph(p, glyph, c);
    });
}

QIcon colorSwatch(const QColor& color) {
    return paintIcon([color](QPainter& p, const QRectF& r) {
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        const qreal m = r.width() * 0.18;
        p.drawRoundedRect(r.adjusted(m, m, -m, -m), r.width() * 0.16, r.width() * 0.16);
    });
}

QIcon hollowSwatch(const QColor& color) {
    return paintIcon([color](QPainter& p, const QRectF& r) {
        p.setPen(QPen(color, r.width() / 14.0));
        p.setBrush(Qt::NoBrush);
        const qreal m = r.width() * 0.22;
        p.drawRoundedRect(r.adjusted(m, m, -m, -m), r.width() * 0.14, r.width() * 0.14);
    });
}

QLabel* sectionLabel(const QString& text, QWidget* parent) {
    auto* l = new QLabel(text.toUpper(), parent);
    l->setProperty("scopeRole", "sectionLabel");
    return l;
}

QLabel* makePill(QWidget* parent) {
    auto* l = new QLabel(parent);
    l->setProperty("scopeRole", "pill");
    l->setProperty("pillTone", "neutral");
    return l;
}

void setPill(QLabel* pill, PillTone tone, const QString& text) {
    if (!pill) return;
    const char* toneName = "neutral";
    switch (tone) {
        case PillTone::Neutral: toneName = "neutral"; break;
        case PillTone::Ok:      toneName = "ok";      break;
        case PillTone::Busy:    toneName = "busy";    break;
        case PillTone::Warn:    toneName = "warn";    break;
        case PillTone::Error:   toneName = "error";   break;
        case PillTone::Rec:     toneName = "rec";     break;
    }
    pill->setText(text);
    if (pill->property("pillTone").toString() != toneName) {
        pill->setProperty("pillTone", toneName);
        // Property selectors are only re-evaluated on repolish.
        pill->style()->unpolish(pill);
        pill->style()->polish(pill);
    }
}

void applyToolbarStrip(QWidget* w) {
    if (!w) return;
    w->setProperty("scopeRole", "toolbarStrip");
    // Plain QWidget ignores stylesheet backgrounds without this.
    w->setAttribute(Qt::WA_StyledBackground, true);
}

namespace {

// Self-managing overlay label: tracks the view's model row count and the
// view's size; no external bookkeeping needed after install.
class EmptyHintLabel : public QLabel {
public:
    EmptyHintLabel(QAbstractItemView* view, const QString& text)
        : QLabel(text, view), view_(view) {
        setProperty("scopeRole", "emptyHint");
        setAlignment(Qt::AlignCenter);
        setWordWrap(true);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        // Resize events can be delivered to the view or coalesced into the
        // viewport (and deferred while the page is hidden), so watch both
        // plus Show.
        view_->installEventFilter(this);
        view_->viewport()->installEventFilter(this);
        if (auto* m = view_->model()) {
            auto refresh = [this] { updateVisibility(); };
            connect(m, &QAbstractItemModel::rowsInserted,  this, refresh);
            connect(m, &QAbstractItemModel::rowsRemoved,   this, refresh);
            connect(m, &QAbstractItemModel::modelReset,    this, refresh);
            connect(m, &QAbstractItemModel::layoutChanged, this, refresh);
        }
        updateVisibility();
    }

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override {
        if ((obj == view_ || obj == view_->viewport())
            && (ev->type() == QEvent::Resize || ev->type() == QEvent::Show)) {
            reposition();
        }
        return false;
    }

private:
    void updateVisibility() {
        const auto* m = view_->model();
        setVisible(!m || m->rowCount() == 0);
        reposition();
    }
    void reposition() {
        const QRect r = view_->viewport()->geometry();
        const int margin = 18;
        setGeometry(r.adjusted(margin, margin, -margin, -margin));
        raise();
    }

    QAbstractItemView* view_;
};

}  // namespace

void installEmptyHint(QAbstractItemView* view, const QString& text) {
    if (!view) return;
    new EmptyHintLabel(view, text);   // parented to the view
}

}  // namespace scope::style
