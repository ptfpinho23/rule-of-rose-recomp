#pragma once
// The launcher's own controls. Everything is painted here (no stock Qt
// widget looks), in the colours and font of style.h.
#include <QAbstractButton>
#include <QStringList>
#include <QWidget>

class QGraphicsOpacityEffect;
class QVariantAnimation;

// A button that knows how far the pointer-hover fade has progressed (0..1).
class HoverButton : public QAbstractButton {
    Q_OBJECT
public:
    explicit HoverButton(QWidget *parent = nullptr);
    qreal hover() const { return m_forced ? 1.0 : m_hover; }
    void forceHover(bool on) { m_forced = on; update(); }   // for --grab pictures

protected:
    void enterEvent(QEnterEvent *e) override;
    void leaveEvent(QEvent *e) override;
    void animateTo(qreal target);

private:
    QVariantAnimation *m_anim;
    qreal m_hover = 0;
    bool m_forced = false;
};

// The one big action: thin outline, letter-spaced, fills with the accent on hover.
class PlayButton : public HoverButton {
    Q_OBJECT
public:
    explicit PlayButton(const QString &text, QWidget *parent = nullptr);
    QSize sizeHint() const override { return {208, 54}; }

protected:
    void paintEvent(QPaintEvent *) override;
};

// A text link. SmallCaps: upper-case, letter-spaced (navigation);
// Inline: sentence text with a hairline underline (calls to action).
class LinkButton : public HoverButton {
    Q_OBJECT
public:
    enum Kind { SmallCaps, Inline, InlineLarge };
    explicit LinkButton(const QString &text, Kind kind, QWidget *parent = nullptr);
    void setLabel(const QString &text);
    void setActive(bool on) { m_active = on; update(); }   // small caps: the chosen tab
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *) override;

private:
    QFont linkFont() const;
    Kind m_kind;
    bool m_active = false;
};

// Minimise / close glyphs in the title strip, and the overlay's close mark.
class GlyphButton : public HoverButton {
    Q_OBJECT
public:
    enum Glyph { Minimise, Close };
    explicit GlyphButton(Glyph g, QWidget *parent = nullptr);
    QSize sizeHint() const override { return {44, 34}; }

protected:
    void paintEvent(QPaintEvent *) override;

private:
    Glyph m_glyph;
};

// On/off switch.
class Toggle : public HoverButton {
    Q_OBJECT
public:
    explicit Toggle(QWidget *parent = nullptr);
    QSize sizeHint() const override { return {46, 24}; }

protected:
    void paintEvent(QPaintEvent *) override;

private:
    QVariantAnimation *m_slide;
    qreal m_pos = 0;
};

// One-of-N choice drawn as a row of cells ("1×  2×  3×  4×").
class Segmented : public QWidget {
    Q_OBJECT
public:
    explicit Segmented(const QStringList &items, QWidget *parent = nullptr);
    int current() const { return m_current; }
    void setCurrent(int i);
    QSize sizeHint() const override;

signals:
    void changed(int index);

protected:
    void paintEvent(QPaintEvent *) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void leaveEvent(QEvent *e) override;

private:
    void changeEvent(QEvent *e) override;

private:
    int cellAt(const QPoint &p) const;
    QStringList m_items;
    int m_cell = 56;      // width of one cell
    int m_current = 0;
    int m_hot = -1;
};

// A value on a line: thin track, accent fill, small handle. Arrow keys,
// Page Up/Down, Home/End when focused; double-click restores the default.
class Slider : public QWidget {
    Q_OBJECT
public:
    Slider(int min, int max, int def, QWidget *parent = nullptr);
    int value() const { return m_value; }
    int defaultValue() const { return m_def; }
    void setValue(int v);
    QSize sizeHint() const override { return {230, 26}; }

signals:
    void changed(int value);

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseDoubleClickEvent(QMouseEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void focusInEvent(QFocusEvent *) override { update(); }
    void focusOutEvent(QFocusEvent *) override { update(); }
    void changeEvent(QEvent *e) override;

private:
    void setFromX(qreal x);
    int m_min, m_max, m_def, m_value;
};

// A drop-down chooser: a framed field with the current choice; a click (or
// Space/Enter) unrolls the list just below it, inside the launcher window.
// Each entry has a label, an optional dim note on the right, and a value.
class Dropdown : public QWidget {
    Q_OBJECT
public:
    struct Item { QString label, note, value; };
    explicit Dropdown(int width, QWidget *parent = nullptr);
    void setItems(const QList<Item> &items);
    int current() const { return m_current; }
    QString currentValue() const { return m_items.value(m_current).value; }
    void setCurrent(int i);
    void setCurrentValue(const QString &value);
    // Text shown instead of the choice while the control is disabled.
    void setDisabledText(const QString &t) { m_disabledText = t; update(); }
    void openList();
    void closeList();
    QSize sizeHint() const override { return {m_width, 34}; }

signals:
    void changed(int index);

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void enterEvent(QEnterEvent *) override { m_hot = true; update(); }
    void leaveEvent(QEvent *) override { m_hot = false; update(); }
    void focusInEvent(QFocusEvent *) override { update(); }
    void focusOutEvent(QFocusEvent *) override { update(); }
    void changeEvent(QEvent *e) override;
    void hideEvent(QHideEvent *) override { closeList(); }
    bool eventFilter(QObject *o, QEvent *e) override;

private:
    friend class DropList;
    QList<Item> m_items;
    QString m_disabledText;
    QWidget *m_list = nullptr;
    int m_width;
    int m_current = 0;
    bool m_hot = false;
};

// The controls table: for each PS2 button, its key and its gamepad button,
// named the Xbox way or the PlayStation way. Two blocks side by side.
class KeyMap : public QWidget {
    Q_OBJECT
public:
    explicit KeyMap(QWidget *parent = nullptr);
    void setPlayStationNames(bool on) { m_ps = on; update(); }
    QSize sizeHint() const override { return {700, 26 + 7 * 36 + 34}; }

protected:
    void paintEvent(QPaintEvent *) override;

private:
    bool m_ps = false;
};

// A thin progress line: track, accent fill up to value (0..1).
class ProgressLine : public QWidget {
    Q_OBJECT
public:
    explicit ProgressLine(QWidget *parent = nullptr) : QWidget(parent) {}
    void setValue(qreal v) { m_value = qBound(0.0, v, 1.0); update(); }
    void setFailed(bool on) { m_failed = on; update(); }

protected:
    void paintEvent(QPaintEvent *) override;

private:
    qreal m_value = 0;
    bool m_failed = false;
};

// A panel that fades and slides in over the dimmed window. Click outside
// it or press Esc to close. The content widget is owned by the overlay.
class Overlay : public QWidget {
    Q_OBJECT
public:
    Overlay(const QString &title, QWidget *content, QWidget *parent);
    void open();
    void close();
    bool isOpen() const { return m_open; }

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *e) override;

private:
    void setProgress(qreal t);
    QWidget *m_card;
    QGraphicsOpacityEffect *m_fade;
    QVariantAnimation *m_anim;
    QPoint m_home;
    qreal m_t = 0;
    bool m_open = false;
};

// A sheet of black over the whole window that fades away on start.
class Veil : public QWidget {
    Q_OBJECT
public:
    explicit Veil(QWidget *parent);
    void lift();

protected:
    void paintEvent(QPaintEvent *) override;

private:
    qreal m_t = 0;
};
