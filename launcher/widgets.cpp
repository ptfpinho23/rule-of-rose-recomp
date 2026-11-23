#include "widgets.h"
#include "style.h"

#include <QApplication>
#include <QEasingCurve>
#include <QGraphicsOpacityEffect>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QVariantAnimation>

using namespace theme;

namespace {

// Text with letter spacing has a trailing gap after the last glyph; shift the
// rectangle so the visible glyphs are what gets centred.
void drawTracked(QPainter &p, const QRectF &r, const QString &text, const QFont &f,
                 int flags = Qt::AlignCenter)
{
    p.setFont(f);
    const qreal tr = f.letterSpacing();
    p.drawText((flags & Qt::AlignHCenter) ? r.adjusted(tr, 0, 0, 0) : r, flags, text);
}

QVariantAnimation *makeAnim(QObject *parent, int ms)
{
    auto *a = new QVariantAnimation(parent);
    a->setDuration(ms);
    a->setEasingCurve(QEasingCurve::OutCubic);
    return a;
}

// The overlay's panel: dark sheet, hairline frame, a rule under the title
// that starts with a short stroke of the accent.
class Card : public QWidget {
public:
    using QWidget::QWidget;

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        p.setPen(QPen(alpha(chalk, 0.20), 1));
        p.setBrush(alpha(panel, 0.975));
        p.drawRect(r);
        p.setPen(QPen(alpha(chalk, 0.13), 1));
        p.drawLine(QPointF(40, 62.5), QPointF(width() - 40, 62.5));
        p.setPen(QPen(rose, 1));
        p.drawLine(QPointF(40, 62.5), QPointF(40 + 34, 62.5));
    }
};

} // namespace

// ---------------------------------------------------------------- hover --
HoverButton::HoverButton(QWidget *parent) : QAbstractButton(parent)
{
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::NoFocus);
    setAttribute(Qt::WA_Hover);
    m_anim = makeAnim(this, 170);
    connect(m_anim, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
        m_hover = v.toReal();
        update();
    });
}

void HoverButton::animateTo(qreal target)
{
    m_anim->stop();
    m_anim->setStartValue(m_hover);
    m_anim->setEndValue(target);
    m_anim->start();
}

void HoverButton::enterEvent(QEnterEvent *e)
{
    if (isEnabled())
        animateTo(1);
    QAbstractButton::enterEvent(e);
}

void HoverButton::leaveEvent(QEvent *e)
{
    animateTo(0);
    QAbstractButton::leaveEvent(e);
}

// ----------------------------------------------------------------- play --
PlayButton::PlayButton(const QString &text, QWidget *parent) : HoverButton(parent)
{
    setText(text);
    resize(sizeHint());
}

void PlayButton::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    const qreal h = isEnabled() ? hover() : 0;
    const bool down = isDown() && isEnabled();

    QColor fill = alpha(rose, down ? 0.62 : 0.92 * h);
    QColor line = mix(alpha(chalk, 0.62), roseLight, h);
    QColor label = mix(chalk, QColor(0xf6, 0xf2, 0xe8), h);
    if (!isEnabled()) {
        fill = Qt::transparent;
        line = alpha(chalk, 0.16);
        label = alpha(chalk, 0.26);
    }
    p.setPen(Qt::NoPen);
    p.setBrush(alpha(ink, 0.35));
    p.drawRect(r);
    p.setBrush(fill);
    p.setPen(QPen(line, 1));
    p.drawRect(r);
    // A second, inner hairline: the frame of a title card.
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(alpha(line, line.alphaF() * 0.35), 1));
    p.drawRect(r.adjusted(4, 4, -4, -4));

    p.setPen(label);
    drawTracked(p, r.adjusted(0, -1, 0, -1), text(), serif(21, SemiBold, 9));
}

// ----------------------------------------------------------------- link --
LinkButton::LinkButton(const QString &text, Kind kind, QWidget *parent)
    : HoverButton(parent), m_kind(kind)
{
    setLabel(text);
}

QFont LinkButton::linkFont() const
{
    switch (m_kind) {
    case SmallCaps:   return serif(13, SemiBold, 3.2);
    case InlineLarge: return serif(23, Regular, 0.3);
    default:          return serif(17, Italic, 0.2);
    }
}

void LinkButton::setLabel(const QString &text)
{
    setText(m_kind == SmallCaps ? text.toUpper() : text);
    resize(sizeHint());
    update();
}

QSize LinkButton::sizeHint() const
{
    const QFontMetricsF fm(linkFont());
    const qreal tr = linkFont().letterSpacing();
    return QSizeF(fm.horizontalAdvance(text()) - tr + 2, fm.height() + 6).toSize();
}

void LinkButton::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const qreal h = hover();
    const QFont f = linkFont();
    const QFontMetricsF fm(f);
    const QColor rest = m_kind == SmallCaps ? (m_active ? chalk : chalkDim) : alpha(chalk, 0.86);
    const QColor c = isDown() ? roseLight : mix(rest, QColor(0xf6, 0xf2, 0xe8), h);
    p.setPen(c);
    p.setFont(f);
    const qreal base = fm.ascent() + 2;
    p.drawText(QPointF(0, base), text());

    // Underline: always there (faint) for inline links, grows from the left
    // on hover for the small-caps ones.
    const qreal w = width() - 1;
    const qreal y = qRound(base + (m_kind == SmallCaps ? 6 : 4)) + 0.5;
    if (m_kind != SmallCaps) {
        p.setPen(QPen(alpha(c, 0.38 + 0.5 * h), 1));
        p.drawLine(QPointF(0, y), QPointF(w, y));
    } else if (m_active || h > 0.01) {
        p.setPen(QPen(alpha(rose, 0.95), 1));
        p.drawLine(QPointF(0, y), QPointF(m_active ? w : w * h, y));
    }
}

// ---------------------------------------------------------------- glyph --
GlyphButton::GlyphButton(Glyph g, QWidget *parent) : HoverButton(parent), m_glyph(g)
{
    resize(sizeHint());
    setCursor(Qt::ArrowCursor);
}

void GlyphButton::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const qreal h = hover();
    if (m_glyph == Close)
        p.fillRect(rect(), alpha(rose, 0.85 * h));
    else
        p.fillRect(rect(), alpha(chalk, 0.10 * h));
    p.setPen(QPen(mix(alpha(chalk, 0.55), QColor(0xf6, 0xf2, 0xe8), h), 1.15));
    const QPointF c = QRectF(rect()).center();
    const qreal s = 5;
    if (m_glyph == Close) {
        p.drawLine(c + QPointF(-s, -s), c + QPointF(s, s));
        p.drawLine(c + QPointF(-s, s), c + QPointF(s, -s));
    } else {
        p.drawLine(c + QPointF(-s, 4.5), c + QPointF(s, 4.5));
    }
}

// --------------------------------------------------------------- toggle --
Toggle::Toggle(QWidget *parent) : HoverButton(parent)
{
    setCheckable(true);
    resize(sizeHint());
    m_slide = makeAnim(this, 160);
    connect(m_slide, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
        m_pos = v.toReal();
        update();
    });
    connect(this, &QAbstractButton::toggled, this, [this](bool on) {
        if (!isVisible()) {       // set from code before showing: no animation
            m_pos = on ? 1 : 0;
            update();
            return;
        }
        m_slide->stop();
        m_slide->setStartValue(m_pos);
        m_slide->setEndValue(on ? 1.0 : 0.0);
        m_slide->start();
    });
}

void Toggle::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const bool en = isEnabled();
    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    const qreal rad = r.height() / 2;
    if (en) {
        p.setPen(QPen(mix(alpha(chalk, 0.40 + 0.3 * hover()), roseLight, m_pos), 1));
        p.setBrush(mix(alpha(ink, 0.5), alpha(rose, 0.95), m_pos));
    } else {
        p.setPen(QPen(alpha(chalk, 0.14), 1));
        p.setBrush(alpha(chalk, 0.05 * m_pos));
    }
    p.drawRoundedRect(r, rad, rad);
    const qreal d = r.height() - 8;
    const qreal x = r.left() + 4 + (r.width() - 8 - d) * m_pos;
    p.setPen(Qt::NoPen);
    p.setBrush(en ? mix(alpha(chalk, 0.62), QColor(0xf6, 0xf2, 0xe8), m_pos) : mix(panel, chalk, 0.28));
    p.drawEllipse(QRectF(x, r.top() + 4, d, d));
}

// ------------------------------------------------------------ segmented --
Segmented::Segmented(const QStringList &items, QWidget *parent)
    : QWidget(parent), m_items(items)
{
    setMouseTracking(true);
    setCursor(Qt::PointingHandCursor);
    const QFontMetricsF fm(serif(17, SemiBold, 0.5));
    for (const QString &t : items)
        m_cell = qMax(m_cell, int(fm.horizontalAdvance(t)) + 30);
    resize(sizeHint());
}

QSize Segmented::sizeHint() const { return {int(m_items.size()) * m_cell + 1, 34}; }

void Segmented::setCurrent(int i)
{
    i = qBound(0, i, int(m_items.size()) - 1);
    if (i == m_current)
        return;
    m_current = i;
    update();
    emit changed(i);
}

int Segmented::cellAt(const QPoint &p) const
{
    const int i = p.x() / m_cell;
    return rect().contains(p) && i < m_items.size() ? i : -1;
}

void Segmented::mouseMoveEvent(QMouseEvent *e)
{
    const int hot = cellAt(e->pos());
    if (hot != m_hot) {
        m_hot = hot;
        update();
    }
}

void Segmented::leaveEvent(QEvent *)
{
    m_hot = -1;
    update();
}

void Segmented::changeEvent(QEvent *e)
{
    if (e->type() == QEvent::EnabledChange) {
        m_hot = -1;
        setCursor(isEnabled() ? Qt::PointingHandCursor : Qt::ArrowCursor);
        update();
    }
    QWidget::changeEvent(e);
}

void Segmented::mousePressEvent(QMouseEvent *e)
{
    const int i = cellAt(e->pos());
    if (e->button() == Qt::LeftButton && i >= 0)
        setCurrent(i);
}

void Segmented::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const bool en = isEnabled();
    const QFont f = serif(17, SemiBold, 0.5);
    for (int i = 0; i < m_items.size(); ++i) {
        const QRectF cell(i * m_cell + 0.5, 0.5, m_cell, height() - 1);
        const bool on = i == m_current;
        p.setPen(QPen(alpha(chalk, en ? 0.26 : 0.12), 1));
        if (on)
            p.setBrush(en ? alpha(rose, 0.95) : alpha(chalk, 0.10));
        else
            p.setBrush(alpha(chalk, i == m_hot ? 0.09 : 0.0));
        p.drawRect(cell);
        if (!en)
            p.setPen(alpha(chalk, on ? 0.42 : 0.24));
        else
            p.setPen(on ? QColor(0xf6, 0xf2, 0xe8) : (i == m_hot ? chalk : chalkDim));
        drawTracked(p, cell.adjusted(0, -1, 0, -1), m_items[i], f);
    }
    // Accent outline on the chosen cell, drawn last so neighbours don't cover it.
    if (en) {
        p.setPen(QPen(roseLight, 1));
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(m_current * m_cell + 0.5, 0.5, m_cell, height() - 1));
    }
}

// --------------------------------------------------------------- slider --
namespace {
const qreal kKnob = 13;   // handle diameter
}

Slider::Slider(int min, int max, int def, QWidget *parent)
    : QWidget(parent), m_min(min), m_max(max), m_def(def), m_value(def)
{
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::PointingHandCursor);
    setToolTip("Double-click to reset");
    resize(sizeHint());
}

void Slider::setValue(int v)
{
    v = qBound(m_min, v, m_max);
    if (v == m_value)
        return;
    m_value = v;
    update();
    emit changed(v);
}

void Slider::setFromX(qreal x)
{
    const qreal t = (x - kKnob / 2) / (width() - kKnob);
    setValue(m_min + qRound(qBound(0.0, t, 1.0) * (m_max - m_min)));
}

void Slider::mousePressEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton)
        setFromX(e->position().x());
}

void Slider::mouseMoveEvent(QMouseEvent *e)
{
    if (e->buttons() & Qt::LeftButton)
        setFromX(e->position().x());
}

void Slider::mouseDoubleClickEvent(QMouseEvent *) { setValue(m_def); }

void Slider::keyPressEvent(QKeyEvent *e)
{
    const int page = qMax(1, (m_max - m_min) / 10);
    switch (e->key()) {
    case Qt::Key_Left:     case Qt::Key_Down: setValue(m_value - 1); break;
    case Qt::Key_Right:    case Qt::Key_Up:   setValue(m_value + 1); break;
    case Qt::Key_PageDown: setValue(m_value - page); break;
    case Qt::Key_PageUp:   setValue(m_value + page); break;
    case Qt::Key_Home:     setValue(m_min); break;
    case Qt::Key_End:      setValue(m_max); break;
    case Qt::Key_Backspace: case Qt::Key_Delete: setValue(m_def); break;
    default: QWidget::keyPressEvent(e);
    }
}

void Slider::changeEvent(QEvent *e)
{
    if (e->type() == QEvent::EnabledChange) {
        setCursor(isEnabled() ? Qt::PointingHandCursor : Qt::ArrowCursor);
        update();
    }
    QWidget::changeEvent(e);
}

void Slider::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const bool en = isEnabled();
    const qreal y = qRound(height() / 2.0) + 0.5;
    const qreal x0 = kKnob / 2, x1 = width() - kKnob / 2;
    const qreal t = qreal(m_value - m_min) / (m_max - m_min);
    const qreal x = x0 + (x1 - x0) * t;
    // Track, a tick at the default, the filled part, the handle.
    p.setPen(QPen(alpha(chalk, en ? 0.24 : 0.10), 1));
    p.drawLine(QPointF(x0, y), QPointF(x1, y));
    const qreal xd = x0 + (x1 - x0) * qreal(m_def - m_min) / (m_max - m_min);
    p.drawLine(QPointF(qRound(xd) + 0.5, y - 4), QPointF(qRound(xd) + 0.5, y + 4));
    if (en) {
        p.setPen(QPen(rose, 2, Qt::SolidLine, Qt::FlatCap));
        p.drawLine(QPointF(x0, y), QPointF(x, y));
    }
    if (hasFocus() && en) {
        p.setPen(QPen(alpha(roseLight, 0.9), 1));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QPointF(x, y), kKnob / 2 + 3, kKnob / 2 + 3);
    }
    p.setPen(Qt::NoPen);
    p.setBrush(en ? QColor(0xee, 0xea, 0xdf) : mix(panel, chalk, 0.28));
    p.drawEllipse(QPointF(x, y), kKnob / 2, kKnob / 2);
}

// ------------------------------------------------------------- dropdown --
// The unrolled list. A child of the launcher window (not a window of its
// own), placed under its Dropdown and raised above everything.
class DropList : public QWidget {
public:
    static constexpr int kRow = 28;
    DropList(Dropdown *owner, QWidget *window) : QWidget(window), m_owner(owner)
    {
        setMouseTracking(true);
        setCursor(Qt::PointingHandCursor);
        resize(owner->width(), int(owner->m_items.size()) * kRow + 2);
        m_hot = owner->m_current;
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(alpha(chalk, 0.30), 1));
        p.setBrush(QColor(0x0c, 0x11, 0x0e));
        p.drawRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5));
        const QFont f = serif(17, SemiBold, 0.4), nf = serif(13, Italic, 0.2);
        for (int i = 0; i < m_owner->m_items.size(); ++i) {
            const Dropdown::Item &it = m_owner->m_items[i];
            const QRectF r(1, 1 + i * kRow, width() - 2, kRow);
            const bool cur = i == m_owner->m_current;
            if (i == m_hot)
                p.fillRect(r, alpha(chalk, 0.09));
            if (cur)
                p.fillRect(QRectF(r.left(), r.top() + 6, 2, r.height() - 12), rose);
            p.setPen(cur || i == m_hot ? QColor(0xf6, 0xf2, 0xe8) : mix(panel, chalk, 0.72));
            p.setFont(f);
            p.drawText(r.adjusted(13, -1, -8, -1), Qt::AlignVCenter | Qt::AlignLeft, it.label);
            if (!it.note.isEmpty()) {
                p.setPen(mix(panel, chalk, 0.42));
                p.setFont(nf);
                p.drawText(r.adjusted(13, 0, -10, 0), Qt::AlignVCenter | Qt::AlignRight, it.note);
            }
        }
    }
    void mouseMoveEvent(QMouseEvent *e) override
    {
        const int hot = qBound(0, (e->pos().y() - 1) / kRow, int(m_owner->m_items.size()) - 1);
        if (hot != m_hot) {
            m_hot = hot;
            update();
        }
    }
    void mousePressEvent(QMouseEvent *e) override
    {
        const int i = (e->pos().y() - 1) / kRow;
        Dropdown *o = m_owner;
        if (i >= 0 && i < o->m_items.size())
            o->setCurrent(i);
        o->closeList();
    }

private:
    Dropdown *m_owner;
    int m_hot;
};

Dropdown::Dropdown(int width, QWidget *parent) : QWidget(parent), m_width(width)
{
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::PointingHandCursor);
    resize(sizeHint());
}

void Dropdown::setItems(const QList<Item> &items)
{
    closeList();
    m_items = items;
    m_current = qBound(0, m_current, qMax(0, int(items.size()) - 1));
    update();
}

void Dropdown::setCurrent(int i)
{
    i = qBound(0, i, int(m_items.size()) - 1);
    if (i == m_current)
        return;
    m_current = i;
    update();
    if (m_list)
        m_list->update();
    emit changed(i);
}

void Dropdown::setCurrentValue(const QString &value)
{
    for (int i = 0; i < m_items.size(); ++i)
        if (m_items[i].value == value)
            setCurrent(i);
}

void Dropdown::openList()
{
    if (m_list || !isEnabled() || m_items.isEmpty())
        return;
    QWidget *win = window();
    m_list = new DropList(this, win);
    // Below the field; above it when there is no room below.
    QPoint at = mapTo(win, QPoint(0, height() + 3));
    if (at.y() + m_list->height() > win->height() - 8) {
        at = mapTo(win, QPoint(0, -m_list->height() - 3));
        if (at.y() < 8)       // no room either way: as low as it goes, over the field
            at.setY(win->height() - 8 - m_list->height());
    }
    m_list->move(at);
    m_list->show();
    m_list->raise();
    qApp->installEventFilter(this);
    update();
}

void Dropdown::closeList()
{
    if (!m_list)
        return;
    qApp->removeEventFilter(this);
    m_list->hide();
    m_list->deleteLater();
    m_list = nullptr;
    update();
}

// While the list is open: a click anywhere else, or Esc, rolls it up.
bool Dropdown::eventFilter(QObject *o, QEvent *e)
{
    if (!m_list)
        return false;
    if (e->type() == QEvent::MouseButtonPress && o != m_list && o->isWidgetType()) {
        closeList();
        return o == this;      // a click on the field itself only closes
    }
    if (e->type() == QEvent::KeyPress && static_cast<QKeyEvent *>(e)->key() == Qt::Key_Escape) {
        closeList();
        return true;
    }
    return false;
}

void Dropdown::mousePressEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton)
        openList();
}

void Dropdown::keyPressEvent(QKeyEvent *e)
{
    switch (e->key()) {
    case Qt::Key_Up:    setCurrent(m_current - 1); break;
    case Qt::Key_Down:  setCurrent(m_current + 1); break;
    case Qt::Key_Home:  setCurrent(0); break;
    case Qt::Key_End:   setCurrent(int(m_items.size()) - 1); break;
    case Qt::Key_Space: case Qt::Key_Return: case Qt::Key_Enter:
        m_list ? closeList() : openList();
        break;
    default: QWidget::keyPressEvent(e);
    }
}

void Dropdown::changeEvent(QEvent *e)
{
    if (e->type() == QEvent::EnabledChange) {
        closeList();
        setCursor(isEnabled() ? Qt::PointingHandCursor : Qt::ArrowCursor);
        update();
    }
    QWidget::changeEvent(e);
}

void Dropdown::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const bool en = isEnabled(), open = m_list != nullptr;
    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    QColor line = alpha(chalk, en ? (m_hot || open ? 0.55 : 0.26) : 0.12);
    if (en && (hasFocus() || open))
        line = roseLight;
    p.setPen(QPen(line, 1));
    p.setBrush(alpha(chalk, en && m_hot ? 0.07 : 0.0));
    p.drawRect(r);
    const QString text = !en && !m_disabledText.isEmpty() ? m_disabledText
                                                          : m_items.value(m_current).label;
    p.setPen(en ? QColor(0xf0, 0xec, 0xe1) : mix(panel, chalk, 0.36));
    p.setFont(serif(17, SemiBold, 0.4));
    p.drawText(r.adjusted(13, -1, -30, -1), Qt::AlignVCenter | Qt::AlignLeft, text);
    // Chevron.
    const QPointF c(r.right() - 16, r.center().y() + (open ? 1.5 : -0.5));
    const qreal d = open ? -4 : 4;
    p.setPen(QPen(en ? mix(panel, chalk, 0.75) : mix(panel, chalk, 0.25), 1.2));
    p.drawLine(c + QPointF(-4.5, -d / 2), c + QPointF(0, d / 2));
    p.drawLine(c + QPointF(0, d / 2), c + QPointF(4.5, -d / 2));
}

// --------------------------------------------------------------- keymap --
KeyMap::KeyMap(QWidget *parent) : QWidget(parent) { resize(sizeHint()); }

void KeyMap::paintEvent(QPaintEvent *)
{
    // PS2 button, keys, gamepad buttons by Xbox name, by PlayStation name.
    // '|' separates caps; "@x" is a drawn PlayStation face-button symbol.
    struct Row { const char *ps2; const char *keys; const char *xbox; const char *ps; };
    static const Row rows[2][7] = {
        {{"D-pad", "Arrows", "D-pad", "D-pad"},
         {"Cross", "Z", "A", "@cross"},
         {"Circle", "X", "B", "@circle"},
         {"Square", "A", "X", "@square"},
         {"Triangle", "S", "Y", "@triangle"},
         {"Start", "Enter", "Start", "Options"},
         {"Select", "Backspace", "Back", "Share|Create"}},
        {{"L1 / R1", "Q|W", "LB|RB", "L1|R1"},
         {"L2 / R2", "E|R", "LT|RT", "L2|R2"},
         {"L3 / R3", "", "Stick clicks", "L3|R3"},
         {"Left stick", "", "Left stick", "Left stick"},
         {"Right stick", "", "Right stick", "Right stick"},
         {"Quit the game", "Esc", "", ""},
         {"", "", "", ""}},
    };
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QFont capFont = serif(14, SemiBold, 0.5), nameFont = serif(17, Regular, 0.2);
    const QFont headFont = serif(11, SemiBold, 2.6);
    const QFontMetricsF cfm(capFont);
    const qreal gap = 36, blockW = (width() - gap) / 2.0;
    const qreal headH = 26, rowH = 36, capH = 26;
    const qreal keyX = 118, padX = 226;      // column starts inside a block

    // The four PlayStation symbols, drawn (no font has to supply them).
    auto symbol = [&](const QRectF &cap, const QString &name) {
        const QPointF c = cap.center();
        const qreal r = 5.2;
        p.setPen(QPen(chalk, 1.3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        if (name == "@cross") {
            p.drawLine(c + QPointF(-r + 0.6, -r + 0.6), c + QPointF(r - 0.6, r - 0.6));
            p.drawLine(c + QPointF(-r + 0.6, r - 0.6), c + QPointF(r - 0.6, -r + 0.6));
        } else if (name == "@circle") {
            p.drawEllipse(c, r, r);
        } else if (name == "@square") {
            p.drawRect(QRectF(c.x() - r + 0.5, c.y() - r + 0.5, 2 * r - 1, 2 * r - 1));
        } else {
            QPolygonF tri;
            tri << c + QPointF(0, -r - 0.3) << c + QPointF(r + 0.4, r - 0.6)
                << c + QPointF(-r - 0.4, r - 0.6);
            p.drawPolygon(tri);
        }
    };
    // Caps: square-cornered for keys, pill-shaped for gamepad buttons.
    auto caps = [&](qreal x, qreal y, const char *list, bool pad) {
        if (!*list) {
            p.setPen(mix(panel, chalk, 0.30));
            p.setFont(nameFont);
            p.drawText(QRectF(x + 2, y, 30, rowH), Qt::AlignVCenter | Qt::AlignLeft, "–");
            return;
        }
        for (const QString &k : QString(list).split('|')) {
            const bool sym = k.startsWith('@');
            const qreal w = sym ? capH + 2 : qMax(capH + 2, cfm.horizontalAdvance(k) + 18);
            const QRectF cap(qRound(x) + 0.5, qRound(y + (rowH - capH) / 2) + 0.5, w, capH);
            p.setPen(QPen(alpha(chalk, pad ? 0.34 : 0.42), 1));
            p.setBrush(alpha(chalk, pad ? 0.0 : 0.06));
            p.drawRoundedRect(cap, pad ? capH / 2 : 3, pad ? capH / 2 : 3);
            if (sym) {
                symbol(cap, k);
            } else {
                p.setPen(chalk);
                drawTracked(p, cap.adjusted(0, -1, 0, -1), k, capFont);
            }
            x += w + 6;
        }
    };
    for (int c = 0; c < 2; ++c) {
        const qreal x0 = c * (blockW + gap);
        p.setPen(mix(panel, chalk, 0.48));
        p.setFont(headFont);
        p.drawText(QRectF(x0, 0, keyX, headH - 8), Qt::AlignLeft | Qt::AlignVCenter, "PS2 PAD");
        p.drawText(QRectF(x0 + keyX, 0, 110, headH - 8), Qt::AlignLeft | Qt::AlignVCenter, "KEYBOARD");
        p.drawText(QRectF(x0 + padX, 0, 110, headH - 8), Qt::AlignLeft | Qt::AlignVCenter, "GAMEPAD");
        p.setPen(QPen(alpha(chalk, 0.12), 1));
        p.drawLine(QPointF(x0, headH - 3.5), QPointF(x0 + blockW, headH - 3.5));
        for (int r = 0; r < 7; ++r) {
            const Row &row = rows[c][r];
            if (!*row.ps2)
                continue;
            const qreal y = headH + r * rowH;
            p.setPen(chalkDim);
            p.setFont(nameFont);
            p.drawText(QRectF(x0, y, keyX - 6, rowH), Qt::AlignVCenter | Qt::AlignLeft, row.ps2);
            caps(x0 + keyX, y, row.keys, false);
            caps(x0 + padX, y, m_ps ? row.ps : row.xbox, true);
        }
    }
    p.setPen(chalkDim);
    p.setFont(serif(15, Regular, 0.2));
    p.drawText(QRectF(0, headH + 7 * rowH + 8, width(), 22), Qt::AlignLeft | Qt::AlignVCenter,
               m_ps ? "DualShock 4 / DualSense names; PS button and touchpad are not used. "
                      "Keyboard and gamepad work together."
                    : "Xbox-style names. Keyboard and gamepad work together.");
}

// -------------------------------------------------------------- overlay --
Overlay::Overlay(const QString &title, QWidget *content, QWidget *parent) : QWidget(parent)
{
    setGeometry(parent->rect());
    hide();

    const int pad = 40, head = 78;
    m_card = new Card(this);
    m_card->resize(content->width() + 2 * pad, head + content->height() + pad);
    m_home = QPoint((width() - m_card->width()) / 2, (height() - m_card->height()) / 2 - 8);
    m_card->move(m_home);

    auto *t = new QLabel(title.toUpper(), m_card);
    t->setFont(serif(14, SemiBold, 5));
    QPalette pal = t->palette();
    pal.setColor(QPalette::WindowText, chalk);
    t->setPalette(pal);
    t->adjustSize();
    t->move(pad, 30);

    auto *x = new GlyphButton(GlyphButton::Close, m_card);
    x->move(m_card->width() - x->width() - 1, 1);
    connect(x, &QAbstractButton::clicked, this, &Overlay::close);

    content->setParent(m_card);
    content->move(pad, head);

    m_fade = new QGraphicsOpacityEffect(m_card);
    m_fade->setOpacity(0);
    m_card->setGraphicsEffect(m_fade);

    m_anim = makeAnim(this, 220);
    connect(m_anim, &QVariantAnimation::valueChanged, this,
            [this](const QVariant &v) { setProgress(v.toReal()); });
    connect(m_anim, &QVariantAnimation::finished, this, [this]() {
        if (!m_open)
            hide();
    });
}

void Overlay::setProgress(qreal t)
{
    m_t = t;
    m_fade->setOpacity(t);
    m_card->move(m_home + QPoint(0, qRound(16 * (1 - t))));
    update();
}

void Overlay::open()
{
    if (m_open)
        return;
    m_open = true;
    setGeometry(parentWidget()->rect());
    show();
    raise();
    m_anim->stop();
    m_anim->setStartValue(m_t);
    m_anim->setEndValue(1.0);
    m_anim->start();
}

void Overlay::close()
{
    if (!m_open)
        return;
    m_open = false;
    parentWidget()->setFocus();   // keys go back to the window (Enter = Play)
    m_anim->stop();
    m_anim->setStartValue(m_t);
    m_anim->setEndValue(0.0);
    m_anim->start();
}

void Overlay::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), alpha(QColor(0x04, 0x06, 0x05), 0.72 * m_t));
}

void Overlay::mousePressEvent(QMouseEvent *e)
{
    if (!m_card->geometry().contains(e->pos()))
        close();
}

// ----------------------------------------------------------------- veil --
Veil::Veil(QWidget *parent) : QWidget(parent)
{
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setGeometry(parent->rect());
}

void Veil::lift()
{
    auto *a = new QVariantAnimation(this);
    a->setDuration(520);
    a->setEasingCurve(QEasingCurve::OutQuad);
    a->setStartValue(0.0);
    a->setEndValue(1.0);
    connect(a, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
        m_t = v.toReal();
        update();
    });
    connect(a, &QVariantAnimation::finished, this, &QWidget::hide);
    a->start(QAbstractAnimation::DeleteWhenStopped);
}

void Veil::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), alpha(ink, 1.0 - m_t));
}
