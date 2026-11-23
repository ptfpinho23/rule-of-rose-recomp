#include "mainwindow.h"
#include "style.h"
#include "widgets.h"

#include <QCloseEvent>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontMetricsF>
#include <QKeyEvent>
#include <QLabel>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QProcessEnvironment>
#include <QScreen>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QWindow>
#include <algorithm>
#include <cstdio>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

using namespace theme;

namespace {

#ifdef Q_OS_WIN
const char *const kPackagedGame = "ror-game.exe";
const char *const kDevGame = "build/recomp/win/ror.exe";
#else
const char *const kPackagedGame = "ror-game";
const char *const kDevGame = "build/recomp/ror";
#endif

// Window metrics (logical pixels). The window does not resize.
const int W = 1100, H = 640;
const int M = 56;                 // side margin of the bottom band
const int kRuleY = 548;           // hairline above the bottom band
const int kStripH = 40;           // title strip (drag area, window buttons)

// One line on stderr per state change, so scripts (and curious users) can
// follow what the launcher did.
void note(const QString &s)
{
    fprintf(stderr, "[launcher] %s\n", s.toLocal8Bit().constData());
    fflush(stderr);
}

QString appDir() { return QCoreApplication::applicationDirPath(); }

QLabel *makeLabel(QWidget *parent, const QFont &f, const QColor &c)
{
    auto *l = new QLabel(parent);
    l->setFont(f);
    QPalette p = l->palette();
    p.setColor(QPalette::WindowText, c);
    l->setPalette(p);
    l->setAttribute(Qt::WA_TransparentForMouseEvents, false);
    return l;
}

void setColor(QLabel *l, const QColor &c)
{
    QPalette p = l->palette();
    p.setColor(QPalette::WindowText, c);
    l->setPalette(p);
}

// Put `text` in a fixed-width label, shortened with an ellipsis when it does
// not fit; the full text is then the tool tip.
void setElided(QLabel *l, const QString &text, Qt::TextElideMode mode = Qt::ElideRight)
{
    const QFontMetricsF fm(l->font());
    const QString shown = fm.elidedText(text, mode, l->width() - 2);
    l->setText(shown);
    l->setToolTip(shown == text ? QString() : text);
}

} // namespace

MainWindow::MainWindow(const QString &isoOverride, QWidget *parent) : QWidget(parent)
{
    setWindowTitle("Rule of Rose");
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    setFixedSize(W, H);
    setFocusPolicy(Qt::StrongFocus);
    setAcceptDrops(true);
    setWindowIcon(theme::icon());
    m_bg = theme::background(QSize(W, H));

    // --- title strip ------------------------------------------------------
    auto *caption = makeLabel(this, serif(11, SemiBold, 3.6), mix(ink, chalk, 0.5));
    caption->setText("NATIVE PC PORT");
    caption->adjustSize();
    caption->move(24, (kStripH - caption->height()) / 2 + 1);
    caption->setAttribute(Qt::WA_TransparentForMouseEvents);

    auto *closeBtn = new GlyphButton(GlyphButton::Close, this);
    closeBtn->move(W - closeBtn->width() - 1, 1);
    closeBtn->setToolTip("Close");
    connect(closeBtn, &QAbstractButton::clicked, this, &QWidget::close);
    auto *minBtn = new GlyphButton(GlyphButton::Minimise, this);
    minBtn->move(closeBtn->x() - minBtn->width(), 1);
    minBtn->setToolTip("Minimise");
    connect(minBtn, &QAbstractButton::clicked, this, &QWidget::showMinimized);

    // --- status (one line + a quiet detail line), above the band -----------
    m_status = makeLabel(this, serif(18, Regular, 0.2), chalk);
    m_status->setGeometry(M, kRuleY - 62, W - 2 * M, 26);
    m_statusDetail = makeLabel(this, serif(15, Regular, 0.2), chalkDim);
    m_statusDetail->setGeometry(M, kRuleY - 36, W - 2 * M, 22);

    // --- bottom band: disc on the left, PLAY on the right -------------------
    m_discMain = makeLabel(this, serif(23, Regular, 0.3), chalk);
    m_discSub = makeLabel(this, serif(15, Regular, 0.2), chalkDim);
    m_discSub->setGeometry(M, kRuleY + 51, 520, 22);
    m_discLink = new LinkButton("Change…", LinkButton::Inline, this);
    connect(m_discLink, &QAbstractButton::clicked, this, &MainWindow::browseDisc);
    m_discCall = new LinkButton("Select disc image…", LinkButton::InlineLarge, this);
    m_discCall->move(M, kRuleY + 17);
    connect(m_discCall, &QAbstractButton::clicked, this, &MainWindow::browseDisc);

    m_play = new PlayButton("PLAY", this);
    m_play->move(W - M - m_play->width(), kRuleY + 20);
    connect(m_play, &QAbstractButton::clicked, this, [this]() {
        if (!m_testMode)
            play();
    });

    auto *controlsLink = new LinkButton("Controls", LinkButton::SmallCaps, this);
    auto *optionsLink = new LinkButton("Options", LinkButton::SmallCaps, this);
    const int linkY = m_play->y() + (m_play->height() - controlsLink->height()) / 2 - 1;
    controlsLink->move(m_play->x() - 48 - controlsLink->width(), linkY);
    optionsLink->move(controlsLink->x() - 36 - optionsLink->width(), linkY);

    // --- overlays -------------------------------------------------------------
    m_options = new Overlay("Options", buildOptions(), this);
    m_controls = new Overlay("Controls", buildControls(), this);
    m_prepare = new Overlay("Preparing the game", buildPrepare(), this);
    connect(optionsLink, &QAbstractButton::clicked, m_options, &Overlay::open);
    connect(controlsLink, &QAbstractButton::clicked, m_controls, &Overlay::open);

    m_veil = new Veil(this);
    m_veil->raise();

    QSettings s;
    {
        // An earlier version cached a captured title picture in art/ beside
        // the settings file. It is no longer used: remove those files.
        QDir art(QFileInfo(s.fileName()).absolutePath() + "/art");
        if (art.exists()) {
            for (const QString &f : art.entryList({"*.png"}, QDir::Files))
                art.remove(f);
            QDir().rmdir(art.absolutePath());   // only goes if nothing else is in it
        }
    }
    // Disc: command line, else the remembered path, else a default-named
    // image near the launcher.
    QString iso = isoOverride;
    if (iso.isEmpty()) {
        iso = s.value("disc/path").toString();
        if (iso.isEmpty() || !QFileInfo(iso).isFile()) {
            const QString found = findDiscIso();
            if (!found.isEmpty())
                iso = found;
        }
    }
    setDisc(iso, false);
}

namespace {

enum Readout { Plain, Signed, Hundredths, Percent };

QString readout(Readout kind, int v)
{
    switch (kind) {
    case Signed:     return v > 0 ? QString("+%1").arg(v) : QString::number(v);
    case Hundredths: return QString::number(v / 100.0, 'f', 2);
    case Percent:    return QString("%1%").arg(v);
    default:         return QString::number(v);
    }
}

const char *const kFilters[] = {"nearest", "bilinear", "fsr", "fsr3"};
// FSR 3.1 exists only in the Windows game (GPU renderer + AMD's DLLs); the
// choice is not offered elsewhere, where the game would just use FSR 1.0.
#ifdef Q_OS_WIN
const int kFilterCount = 4;
#else
const int kFilterCount = 3;
#endif

// --pad values, in the order of the Gamepad cells.
const char *const kPadKinds[4] = {"auto", "xinput", "playstation", "off"};

// --wide values, in the order of the Picture shape cells.
const char *const kWide[4] = {"off", "16:9", "21:9", "32:9"};

// "16:9" etc. for a window size, to label the resolution list; empty if it
// is none of the usual shapes.
QString shapeName(const QSize &z)
{
    const double r = double(z.width()) / z.height();
    struct { double r; const char *name; } const shapes[] = {
        {4.0 / 3, "4:3"}, {16.0 / 9, "16:9"}, {16.0 / 10, "16:10"}, {64.0 / 27, "21:9"},
        {43.0 / 18, "21:9"}, {32.0 / 9, "32:9"}};
    for (const auto &sh : shapes)
        if (qAbs(r - sh.r) < 0.02)
            return sh.name;
    return {};
}

// The renderer a fresh install starts with, and how the GPU choice is named.
// (Windows machines nearly always have a Vulkan GPU driver; under Linux -
// WSL in particular - Vulkan is often a slow CPU implementation. Without
// Vulkan the game uses its software renderer by itself.)
#ifdef Q_OS_WIN
const char *const kDefaultRenderer = "gpu";
#else
const char *const kDefaultRenderer = "software";
#endif
const char *const kGpuLabel = "GPU (Vulkan)";

} // namespace

// The Controls panel: the table, and a switch for how the gamepad buttons
// are named in it (cosmetic; remembered).
QWidget *MainWindow::buildControls()
{
    auto *map = new KeyMap;
    auto *root = new QWidget;
    const int top = 44;
    root->resize(map->width(), top + map->height());
    map->setParent(root);
    map->move(0, top);
    auto *names = new Segmented({"Xbox", "PlayStation"}, root);
    names->move(root->width() - names->width(), 0);
    auto *l = makeLabel(root, serif(15, Regular, 0.2), chalkDim);
    l->setText("Gamepad names");
    l->adjustSize();
    l->move(names->x() - l->width() - 16, (names->height() - l->height()) / 2 - 1);
    const bool ps = QSettings().value("ui/pad_names", "xbox").toString() == "playstation";
    names->setCurrent(ps ? 1 : 0);
    map->setPlayStationNames(ps);
    connect(names, &Segmented::changed, this, [map](int i) {
        map->setPlayStationNames(i == 1);
        QSettings().setValue("ui/pad_names", i ? "playstation" : "xbox");
    });
    return root;
}

// The Options panel: three tabs (Display, Image, Game) of rows, each a label
// on the left and one control on the right. Every control is loaded from and
// saved to QSettings here; play() turns them into game switches.
QWidget *MainWindow::buildOptions()
{
    const int w = 560, rowH = 46, tabH = 50, pageH = 6 * rowH + 52;
    auto *root = new QWidget;
    root->resize(w, tabH + pageH);

    int tabX = 0;
    const char *const tabNames[kTabs] = {"Display", "Quality", "Image", "Game"};
    for (int i = 0; i < kTabs; ++i) {
        m_optTabs[i] = new LinkButton(tabNames[i], LinkButton::SmallCaps, root);
        m_optTabs[i]->move(tabX, 0);
        tabX += m_optTabs[i]->width() + 34;
        connect(m_optTabs[i], &QAbstractButton::clicked, this, [this, i]() { selectOptionsTab(i); });
        m_optPages[i] = new QWidget(root);
        m_optPages[i]->setGeometry(0, tabH, w, pageH);
    }

    const QFont labelFont = serif(19, Regular, 0.2);
    auto row = [&](QWidget *page, int i, const QString &text, QWidget *control) {
        auto *l = makeLabel(page, labelFont, chalk);
        l->setText(text);
        l->adjustSize();
        l->move(0, i * rowH + (rowH - l->height()) / 2 - 1);
        control->setParent(page);
        control->move(w - control->width(), i * rowH + (rowH - control->height()) / 2);
        return l;
    };
    // A slider row: label, slider, and the value in figures at the far right.
    auto sliderRow = [&](QWidget *page, int i, const QString &text, const char *key, int min,
                         int max, int def, Readout kind, QLabel **labelOut = nullptr) {
        auto *sl = new Slider(min, max, def);
        QLabel *l = row(page, i, text, sl);
        sl->move(w - 64 - sl->width(), sl->y());
        auto *val = makeLabel(page, serif(17, SemiBold, 0.3), chalkDim);
        val->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        val->setGeometry(w - 60, i * rowH, 60, rowH - 2);
        sl->setValue(QSettings().value(key, def).toInt());
        val->setText(readout(kind, sl->value()));
        const QString k = key;
        connect(sl, &Slider::changed, this, [val, kind, k](int v) {
            val->setText(readout(kind, v));
            QSettings().setValue(k, v);
        });
        if (labelOut)
            *labelOut = l;
        return sl;
    };
    auto helper = [&](QWidget *page, int y, const QString &text) {
        auto *l = makeLabel(page, serif(15, Regular, 0.2), chalkDim);
        l->setGeometry(0, y, w, 22);
        setElided(l, text, Qt::ElideMiddle);
        return l;
    };
    QSettings s;

    // --- Display ------------------------------------------------------------
    QWidget *d = m_optPages[0];
    m_renderer = new Segmented({"Software", kGpuLabel});
    m_mode = new Segmented({"Window", "Fullscreen"});
    m_res = new Dropdown(236);
    m_wide = new Segmented({"4:3", "16:9", "21:9", "32:9"});
    m_stretch = new Toggle;
    m_fps = new Dropdown(236);
    m_vsync = new Toggle;
    row(d, 0, "Mode", m_mode);
    m_resLabel = row(d, 1, "Resolution", m_res);
    row(d, 2, "Picture shape", m_wide);
    row(d, 3, "Stretch to fill the window", m_stretch);
    m_fpsLabel = row(d, 4, "Frame rate", m_fps);
    m_vsyncLabel = row(d, 5, "V-Sync", m_vsync);
    m_displayNote = helper(d, 6 * rowH + 4, {});
    helper(d, 6 * rowH + 27,
           "4:3 is the original. Wider (experimental) widens 3D scenes; menus, movies, text stay 4:3.");

    // Renderer, Frame rate and V-Sync hang together: a frame rate above the
    // original, or touching V-Sync, needs the GPU renderer, so choosing one
    // selects it; choosing Software puts the frame rate back to Original.
    // Nothing here is ever greyed out because of the other tab, and no
    // contradictory combination is saved.
    bool gpuSaved = s.value("video/renderer", kDefaultRenderer).toString() == "gpu";
    QString fpsSaved = s.value("video/fps", "0").toString();
    if (!gpuSaved && fpsSaved != "0") {
        fpsSaved = "0";
        s.setValue("video/fps", fpsSaved);
    }
    m_renderer->setCurrent(gpuSaved ? 1 : 0);
    m_fps->setItems({{"Original (50 Hz)", {}, "0"}, {"Match display", {}, "display"},
                     {"60 fps", {}, "60"},      {"120 fps", {}, "120"},
                     {"144 fps", {}, "144"},    {"240 fps", {}, "240"}});
    m_fps->setCurrentValue(fpsSaved);
    m_vsync->setChecked(s.value("video/vsync", true).toBool());
    connect(m_renderer, &Segmented::changed, this, [this](int i) {
        QSettings().setValue("video/renderer", i ? "gpu" : "software");
        if (i == 0) {
            m_fps->setCurrentValue("0");
            if (m_filter->current() == 3)
                m_filter->setCurrent(2);
        }
        updateOptionStates();
    });
    connect(m_fps, &Dropdown::changed, this, [this](int) {
        QSettings().setValue("video/fps", m_fps->currentValue());
        if (m_fps->currentValue() != "0")
            m_renderer->setCurrent(1);
    });
    connect(m_vsync, &QAbstractButton::toggled, this, [this](bool on) {
        QSettings().setValue("video/vsync", on);
        m_renderer->setCurrent(1);
    });

    // --- Quality ------------------------------------------------------------
    QWidget *q = m_optPages[1];
    m_internal = new Dropdown(236);
    m_filter = new Segmented(QStringList({"Nearest", "Bilinear", "FSR 1.0", "FSR 3.1"}).mid(0, kFilterCount));
    row(q, 0, "Renderer", m_renderer);
    m_internalLabel = row(q, 1, "Internal resolution", m_internal);
    row(q, 2, "Upscaling", m_filter);
    m_sharp = sliderRow(q, 3, "Sharpness", "video/sharpness", 0, 100, 60, Plain, &m_sharpLabel);
    m_qualityNote = helper(q, 4 * rowH + 10, {});
    m_fsrNote = helper(q, 4 * rowH + 32, {});
    m_fsrNote2 = helper(q, 4 * rowH + 54, {});
    helper(q, 4 * rowH + 76, "Internal resolution sizes are for the game's 512 × 448 picture.");
    {
        // The GPU renderer draws the PS2 picture (512 x 448 in this game) N times larger.
        QList<Dropdown::Item> items;
        for (int n : {1, 2, 3, 4, 6, 8})
            items << Dropdown::Item{
                n == 1 ? QString("1× native  ·  512 × 448")
                       : QString("%1×  ·  %2 × %3").arg(n).arg(512 * n).arg(448 * n),
                {}, QString::number(n)};
        m_internal->setItems(items);
        m_internal->setDisabledText("GPU renderer only");
        m_internal->setCurrentValue("4");
        m_internal->setCurrentValue(s.value("video/internal", 4).toString());
        connect(m_internal, &Dropdown::changed, this, [this](int) {
            QSettings().setValue("video/internal", m_internal->currentValue().toInt());
            updateOptionStates();
        });
    }

    // Resolutions: the fixed list, the desktop's own size, and - if the saved
    // one is none of those (e.g. migrated from the old 3x/4x) - that one too.
    QSize desk(0, 0);
    if (const QScreen *scr = QGuiApplication::primaryScreen())
        desk = (QSizeF(scr->size()) * scr->devicePixelRatio()).toSize();
    QString saved = s.value("video/res").toString();
    if (saved.isEmpty() && s.contains("game/scale")) {       // one-time migration
        const int n = qBound(1, s.value("game/scale").toInt(), 4);
        saved = QString("%1x%2").arg(640 * n).arg(480 * n);
        s.setValue("video/res", saved);
    }
    s.remove("game/scale");
    if (saved.isEmpty())
        saved = "1280x960";
    QList<QSize> sizes = {{640, 480},   {1280, 720},  {1280, 960},  {1600, 1200},
                          {1920, 1080}, {2560, 1080}, {2560, 1440}, {3440, 1440},
                          {3840, 1080}, {3840, 2160}, {5120, 1440}};
    const QStringList wh = saved.split('x');
    const QSize savedSize(wh.value(0).toInt(), wh.value(1).toInt());
    if (saved != "desktop" && savedSize.width() >= 320 && savedSize.height() >= 240 &&
        !sizes.contains(savedSize)) {
        sizes << savedSize;
        std::sort(sizes.begin(), sizes.end(), [](const QSize &a, const QSize &b) {
            return a.width() != b.width() ? a.width() < b.width() : a.height() < b.height();
        });
    }
    QList<Dropdown::Item> items;
    for (const QSize &z : sizes) {
        const bool big = !desk.isEmpty() && (z.width() > desk.width() || z.height() > desk.height());
        items << Dropdown::Item{QString("%1 × %2").arg(z.width()).arg(z.height()),
                                big ? QString("larger than screen") : shapeName(z),
                                QString("%1x%2").arg(z.width()).arg(z.height())};
    }
    m_desktopRes = desk.isEmpty() ? QString("1280x960")
                                  : QString("%1x%2").arg(desk.width()).arg(desk.height());
    const QString deskLabel = desk.isEmpty()
                                  ? QString("Desktop")
                                  : QString("Desktop (%1 × %2)").arg(desk.width()).arg(desk.height());
    items << Dropdown::Item{deskLabel, {}, "desktop"};
    m_res->setItems(items);
    m_res->setDisabledText(deskLabel);
    m_res->setCurrentValue("1280x960");
    m_res->setCurrentValue(saved);
    connect(m_res, &Dropdown::changed, this,
            [this](int) { QSettings().setValue("video/res", m_res->currentValue()); });

    m_mode->setCurrent(s.value("video/fullscreen", false).toBool() ? 1 : 0);
    m_stretch->setChecked(s.value("video/aspect", "4:3").toString() == "stretch");
    m_wide->setCurrent(qMax(0, int(QStringList(kWide, kWide + 4)
                                       .indexOf(s.value("video/wide", "off").toString()))));
    // FSR 3.1 belongs to the GPU renderer: choosing it selects GPU, choosing
    // Software goes back to FSR 1.0, and a saved Software + FSR 3.1 is put
    // right here. Where FSR 3.1 is not offered, a saved one shows as FSR 1.0.
    int filter = 2;
    for (int i = 0; i < kFilterCount; ++i)
        if (s.value("video/filter", "fsr").toString() == kFilters[i])
            filter = i;
    if (filter == 3 && m_renderer->current() == 0) {
        filter = 2;
        s.setValue("video/filter", kFilters[filter]);
    }
    m_filter->setCurrent(filter);
    connect(m_mode, &Segmented::changed, this, [this](int i) {
        QSettings().setValue("video/fullscreen", i == 1);
        updateOptionStates();
    });
    connect(m_stretch, &QAbstractButton::toggled, this,
            [](bool on) { QSettings().setValue("video/aspect", on ? "stretch" : "4:3"); });
    connect(m_wide, &Segmented::changed, this,
            [](int i) { QSettings().setValue("video/wide", kWide[i]); });
    connect(m_filter, &Segmented::changed, this, [this](int i) {
        QSettings().setValue("video/filter", kFilters[i]);
        if (i == 3)
            m_renderer->setCurrent(1);
        updateOptionStates();
    });

    // --- Image ----------------------------------------------------------------
    QWidget *im = m_optPages[2];
    m_bright = sliderRow(im, 0, "Brightness", "video/brightness", -100, 100, 0, Signed);
    m_contrast = sliderRow(im, 1, "Contrast", "video/contrast", -100, 100, 0, Signed);
    m_gamma = sliderRow(im, 2, "Gamma", "video/gamma", 50, 200, 100, Hundredths);
    m_saturation = sliderRow(im, 3, "Saturation", "video/saturation", 0, 200, 100, Percent);
    m_bloom = sliderRow(im, 4, "Bloom", "video/bloom", 0, 100, 0, Plain);
    m_grain = sliderRow(im, 5, "Film grain", "video/film_grain", 0, 100, 100, Percent);
    auto *reset = new LinkButton("Reset", LinkButton::Inline, im);
    reset->move(w - reset->width(), 6 * rowH + 8);
    connect(reset, &QAbstractButton::clicked, this, [this]() {
        for (Slider *sl : {m_bright, m_contrast, m_gamma, m_saturation, m_bloom, m_grain})
            sl->setValue(sl->defaultValue());
    });
    helper(im, 6 * rowH + 10, "Film grain is the game's own; 100% is the original, 0% removes it.")
        ->resize(w - 80, 22);

    // --- Game -------------------------------------------------------------------
    QWidget *g = m_optPages[3];
    m_sound = new Toggle;
    m_memcard = new Toggle;
    m_pad = new Segmented({"Auto", "Xbox", "PlayStation", "Off"});
    row(g, 0, "Sound", m_sound);
    row(g, 1, "Memory card inserted", m_memcard);
    row(g, 2, "Gamepad", m_pad);
    m_deadzone = sliderRow(g, 3, "Stick dead zone", "game/pad_deadzone", 0, 90, 24, Percent,
                           &m_deadzoneLabel);
    helper(g, 4 * rowH + 8, "Auto: an Xbox-style pad if there is one, else a PlayStation pad. The keyboard always works.");
    helper(g, 4 * rowH + 34, "Without a card the game cannot save or load. Saves are kept in");
    auto *path = makeLabel(g, serif(15, Italic, 0.2), chalkDim);
    path->setGeometry(0, 4 * rowH + 56, w, 22);
    setElided(path, QDir::toNativeSeparators(QDir(appDir()).filePath("saves/mc0")),
              Qt::ElideMiddle);
    m_sound->setChecked(s.value("game/sound", true).toBool());
    m_memcard->setChecked(s.value("game/memcard", true).toBool());
    connect(m_sound, &QAbstractButton::toggled, this,
            [](bool on) { QSettings().setValue("game/sound", on); });
    connect(m_memcard, &QAbstractButton::toggled, this,
            [](bool on) { QSettings().setValue("game/memcard", on); });
    // Which gamepad: the old on/off setting becomes Auto / Off, once.
    QString kind = s.value("game/pad_kind").toString();
    if (kind.isEmpty() && s.contains("game/pad") && !s.value("game/pad").toBool()) {
        kind = "off";
        s.setValue("game/pad_kind", kind);
    }
    s.remove("game/pad");
    m_pad->setCurrent(qMax(0, int(QStringList(kPadKinds, kPadKinds + 4).indexOf(kind))));
    connect(m_pad, &Segmented::changed, this, [this](int i) {
        QSettings().setValue("game/pad_kind", kPadKinds[i]);
        updateOptionStates();
    });

    updateOptionStates();
    selectOptionsTab(0);
    return root;
}

void MainWindow::selectOptionsTab(int index)
{
    for (int i = 0; i < kTabs; ++i) {
        m_optTabs[i]->setActive(i == index);
        m_optPages[i]->setVisible(i == index);
    }
}

// What applies depends on the renderer and the mode: fullscreen uses the
// desktop's size; internal resolution, frame rate and V-Sync are the GPU
// renderer's; sharpness is FSR's.
void MainWindow::updateOptionStates()
{
    const bool windowed = m_mode->current() == 0, fsr = m_filter->current() >= 2;
    const bool fsr3 = m_filter->current() == 3;
    const bool gpu = m_renderer->current() == 1;
    const QColor on = chalk, off = mix(panel, chalk, 0.36);
    m_res->setEnabled(windowed);
    setColor(m_resLabel, windowed ? on : off);
    m_internal->setEnabled(gpu);
    setColor(m_internalLabel, gpu ? on : off);
    m_sharp->setEnabled(fsr);
    setColor(m_sharpLabel, fsr ? on : off);
    const bool pad = m_pad->current() != 3;
    m_deadzone->setEnabled(pad);
    setColor(m_deadzoneLabel, pad ? on : off);
    setElided(m_displayNote,
              gpu ? QString("Frame rate and V-Sync use the GPU renderer.")
                  : QString("Choosing a frame rate, or changing V-Sync, switches to the GPU renderer."));
    setElided(m_fsrNote, fsr3 ? QString("FSR 3.1 renders at a low internal resolution and reconstructs detail over time.")
                              : QString("FSR 1.0: sharper upscaling of the picture to the chosen resolution."));
    setElided(m_fsrNote2,
              !fsr3 ? QString()
              : m_internal->currentValue().toInt() > 2
                    ? QString("FSR 3.1 is meant for 1× or 2× internal resolution; above that, FSR 1.0 is sharper.")
                    : QString("Best at 1× or 2× internal resolution. Windows and the GPU renderer only."));
    setElided(m_qualityNote,
              gpu ? QString("GPU needs Vulkan 1.3 (else the game uses Software). Above 50 Hz, frames are interpolated.")
                  : QString("Software runs at the original 50 Hz; higher frame rates use the GPU renderer."));
}

// The game's command line for the current options. A switch is passed only
// when it differs from the game's default, except --res, --renderer and
// --filter (always).
QStringList MainWindow::gameArguments() const
{
    QStringList args = {"--iso", QDir::toNativeSeparators(m_disc.path)};
    auto opt = [&args](const char *name, int value, int def) {
        if (value != def)
            args << name << QString::number(value);
    };
    // Always the window size in real pixels (in fullscreen it is the size
    // Alt+Enter returns to).
    const QString res = m_res->currentValue();
    args << "--res" << (res == "desktop" ? m_desktopRes : res);
    if (m_mode->current() == 1)
        args << "--fullscreen";
    if (m_wide->current() > 0)
        args << "--wide" << kWide[m_wide->current()];
    if (m_stretch->isChecked())
        args << "--aspect" << "stretch";
    const bool gpu = m_renderer->current() == 1;
    args << "--renderer" << (gpu ? "gpu" : "software");
    if (gpu) {
        opt("--internal", m_internal->currentValue().toInt(), 4);
        if (!m_vsync->isChecked())
            args << "--vsync" << "0";
        if (m_fps->currentValue() != "0")
            args << "--fps" << m_fps->currentValue();
    }
    args << "--filter" << kFilters[m_filter->current()];
    if (m_filter->current() >= 2)               // FSR 1.0 and FSR 3.1
        opt("--sharpness", m_sharp->value(), 60);
    opt("--brightness", m_bright->value(), 0);
    opt("--contrast", m_contrast->value(), 0);
    opt("--gamma", m_gamma->value(), 100);
    opt("--saturation", m_saturation->value(), 100);
    opt("--bloom", m_bloom->value(), 0);
    opt("--film-grain", m_grain->value(), 100);
    if (m_pad->current() > 0)                // Auto is the game's default
        args << "--pad" << kPadKinds[m_pad->current()];
    if (m_pad->current() != 3)
        opt("--pad-deadzone", m_deadzone->value(), 24);
    return args;
}

void MainWindow::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.drawPixmap(0, 0, m_bg);
    // Hairline above the bottom band, and a faint frame: the window has no
    // native border.
    p.setPen(QPen(alpha(chalk, 0.15), 1));
    p.drawLine(QPointF(M, kRuleY + 0.5), QPointF(W - M, kRuleY + 0.5));
    p.setPen(QPen(alpha(chalk, 0.10), 1));
    p.drawRect(QRectF(0.5, 0.5, W - 1, H - 1));
}

void MainWindow::showEvent(QShowEvent *e)
{
    QWidget::showEvent(e);
    if (!m_shown) {
        m_shown = true;
        m_veil->lift();
    }
}

Overlay *MainWindow::openOverlay() const
{
    return m_options->isOpen() ? m_options : m_controls->isOpen() ? m_controls
           : m_prepare->isOpen() ? m_prepare : nullptr;
}

void MainWindow::showPanel(const QString &name)
{
    // "options", "options:res", "options:quality", "options:image", "options:game", "controls"
    if (name.startsWith("options")) {
        selectOptionsTab(name.endsWith(":quality") ? 1 : name.endsWith(":image") ? 2
                         : name.endsWith(":game") ? 3 : 0);
        m_options->open();
        if (name.endsWith(":res")) {    // Display tab with the resolution list unrolled,
            auto *t = new QTimer(this);  // once the panel has finished sliding in
            t->setSingleShot(true);
            connect(t, &QTimer::timeout, m_res, &Dropdown::openList);
            t->start(400);
        }
    } else if (name == "controls") {
        m_controls->open();
    }
}

// --set name=value (test hook): operate a control the way a click would.
void MainWindow::testSet(const QString &name, const QString &value)
{
    if (name == "renderer")
        m_renderer->setCurrent(value == "gpu" ? 1 : 0);
    else if (name == "fps")
        m_fps->setCurrentValue(value);
    else if (name == "filter") {
        const int i = int(QStringList(kFilters, kFilters + kFilterCount).indexOf(value));
        if (i >= 0)                 // a choice this platform does not offer cannot be clicked
            m_filter->setCurrent(i);
    }
    else if (name == "internal")
        m_internal->setCurrentValue(value);
    else if (name == "vsync")
        m_vsync->setChecked(value == "1" || value == "on");
}

void MainWindow::setTestMode(bool on)
{
    m_testMode = on;
    setAttribute(Qt::WA_ShowWithoutActivating, on);
    setWindowFlag(Qt::WindowDoesNotAcceptFocus, on);
}

void MainWindow::forcePlayHover() { m_play->forceHover(true); }

void MainWindow::keyPressEvent(QKeyEvent *e)
{
    Overlay *o = openOverlay();
    if (e->key() == Qt::Key_Escape && o) {
        o->close();
    } else if ((e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) && !o) {
        if (m_play->isEnabled() && !m_testMode)
            play();
    } else {
        QWidget::keyPressEvent(e);
    }
}

void MainWindow::mousePressEvent(QMouseEvent *e)
{
    // Drag the window by the picture (anything above the bottom band).
    if (e->button() == Qt::LeftButton && e->position().y() < kRuleY - 70 && windowHandle())
        windowHandle()->startSystemMove();
}

void MainWindow::dragEnterEvent(QDragEnterEvent *e)
{
    if (e->mimeData()->hasUrls() && e->mimeData()->urls().first().isLocalFile())
        e->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *e)
{
    const QList<QUrl> urls = e->mimeData()->urls();
    if (!urls.isEmpty() && urls.first().isLocalFile())
        setDisc(urls.first().toLocalFile(), true);
}

void MainWindow::setStatus(Level level, const QString &text, const QString &detail)
{
    setColor(m_status, level == Bad ? roseLight : chalk);
    setElided(m_status, text);
    setElided(m_statusDetail, detail, Qt::ElideMiddle);
    if (!text.isEmpty())
        note(detail.isEmpty() ? text : text + "  " + detail);
}

void MainWindow::layoutDisc()
{
    // "Change…" sits on the baseline of the disc name, a little to its right.
    m_discMain->adjustSize();
    m_discMain->move(M, kRuleY + 17);
    const QFontMetrics big(m_discMain->font());
    m_discLink->move(m_discMain->x() + m_discMain->width() + 22,
                     m_discMain->y() + big.ascent() - QFontMetrics(serif(17, Italic)).ascent() - 3);
}

void MainWindow::setDisc(const QString &path, bool remember)
{
    m_disc = checkDisc(path);
    const QString file = QFileInfo(m_disc.path).fileName();
    const QString native = QDir::toNativeSeparators(m_disc.path);
    const bool none = path.isEmpty();

    m_discCall->setVisible(none);
    m_discMain->setVisible(!none);
    m_discLink->setVisible(!none);
    setColor(m_discMain, m_disc.ok() ? chalk : roseLight);
    QString sub;
    switch (m_disc.state) {
    case DiscInfo::Ok:
        m_discMain->setText(QString("Rule of Rose (Europe)  ·  %1").arg(kDiscSerial));
        m_discLink->setLabel("Change…");
        sub = file;
        break;
    case DiscInfo::WrongDisc:
        m_discMain->setText(m_disc.serial.isEmpty()
                                ? QString("Wrong disc")
                                : QString("Wrong disc  ·  %1").arg(m_disc.serial));
        m_discLink->setLabel("Select another…");
        sub = QString("This port needs the European release, %1.").arg(kDiscSerial);
        break;
    case DiscInfo::NotIso:
        m_discMain->setText("Not a disc image");
        m_discLink->setLabel("Select another…");
        sub = file + "  ·  expected a plain .iso file";
        break;
    case DiscInfo::Missing:
        m_discMain->setText("Disc image not found");
        m_discLink->setLabel("Select another…");
        sub = none ? QString("Your European disc (%1) as an .iso file. Browse, or drop it here.")
                         .arg(kDiscSerial)
                   : native;
        break;
    }
    setElided(m_discSub, sub, m_disc.state == DiscInfo::Missing && !none ? Qt::ElideMiddle
                                                                         : Qt::ElideRight);
    if (m_disc.ok())
        m_discSub->setToolTip(native);
    layoutDisc();

    if (remember && m_disc.ok())
        QSettings().setValue("disc/path", m_disc.path);
    note(QString("disc %1: %2 (%3)").arg(m_disc.ok() ? "ok" : "rejected", m_disc.path, m_disc.message));

    if (m_game)
        return; // keep the "running" status
    if (m_build)                 // another disc was chosen mid-way: that build is void
        cancelPrepare();
    refreshGameState();
}

void MainWindow::browseDisc()
{
    const QString start = m_disc.path.isEmpty() ? QDir::homePath()
                                                 : QFileInfo(m_disc.path).absolutePath();
    const QString f = QFileDialog::getOpenFileName(
        this, "Choose the Rule of Rose disc image", start,
        "Disc images (*.iso *.ISO);;All files (*)");
    if (!f.isEmpty())
        setDisc(f, true);
}

// ------------------------------------------------- preparing the game ----
namespace {
QString g_gameDir;
#ifdef Q_OS_WIN
const char *const kBuilder = "ror-build.exe";
#else
const char *const kBuilder = "ror-build";
#endif
}

void MainWindow::setGameDir(const QString &dir) { g_gameDir = dir; }

QString MainWindow::gameDir()
{
    if (g_gameDir.isEmpty())
        g_gameDir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/game";
    return g_gameDir;
}

QString MainWindow::builderPath() const { return QDir(appDir()).filePath(kBuilder); }
QString MainWindow::builtGamePath() const { return QDir(gameDir()).filePath(kPackagedGame); }

// Decide where the game comes from, and whether it has to be prepared.
void MainWindow::refreshGameState()
{
    if (m_build)
        return;
    if (!findGameBinary().isEmpty()) {
        m_gameState = Bundled;
    } else if (!QFileInfo(builderPath()).isFile()) {
        m_gameState = NoBuilder;
    } else if (!m_disc.ok()) {
        m_gameState = NeedsBuild;          // unknown until there is a disc; PLAY is off anyway
    } else {
        // "--check" answers at once: 0 = the game in gameDir() is the one
        // for this disc image and this package, anything else = prepare it.
        QProcess check;
        check.setProgram(builderPath());
        check.setArguments({"--check", "--iso", QDir::toNativeSeparators(m_disc.path), "--out",
                            QDir::toNativeSeparators(gameDir())});
        check.setStandardOutputFile(QProcess::nullDevice());
        check.setStandardErrorFile(QProcess::nullDevice());
#ifdef Q_OS_WIN
        check.setCreateProcessArgumentsModifier(
            [](QProcess::CreateProcessArguments *a) { a->flags |= CREATE_NO_WINDOW; });
#endif
        check.start();
        const bool finished = check.waitForFinished(8000);
        if (!finished)
            check.kill(), check.waitForFinished(2000);
        const bool upToDate = finished && check.exitStatus() == QProcess::NormalExit &&
                              check.exitCode() == 0 && QFileInfo(builtGamePath()).isFile();
        m_gameState = upToDate ? Ready : NeedsBuild;
    }
    updatePlayButton();
    if (m_game)
        return;                             // keep the "running" status
    if (m_gameState == NoBuilder)
        setStatus(Bad, "This package is incomplete: the game builder is missing.",
                  QString("Expected %1 next to the launcher, in %2")
                      .arg(kBuilder, QDir::toNativeSeparators(appDir())));
    else if (m_gameState == NeedsBuild && m_disc.ok())
        setStatus(Info, "The game has to be prepared for this PC once, from your disc.",
                  "Press PREPARE GAME; it takes a few minutes");
    else
        setStatus(Info, {});
}

void MainWindow::updatePlayButton()
{
    if (m_game) {
        m_play->setEnabled(false);
        return;
    }
    QString text = "PLAY";
    bool on = m_disc.ok();
    if (m_build) {
        text = QString("PREPARING  %1%").arg(m_buildPercent);
        on = true;
    } else if (m_gameState == NoBuilder) {
        on = false;
    } else if (m_gameState == NeedsBuild && m_disc.ok()) {
        text = "PREPARE GAME";
    }
    m_play->setText(text);
    m_play->setEnabled(on);
    m_play->update();
}

QWidget *MainWindow::buildPrepare()
{
    const int w = 520;
    auto *root = new QWidget;
    root->resize(w, 168);
    m_prepText = makeLabel(root, serif(18, Regular, 0.2), chalk);
    m_prepText->setWordWrap(true);
    m_prepText->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    m_prepText->setGeometry(0, 0, w, 54);
    m_prepBar = new ProgressLine(root);
    m_prepBar->setGeometry(0, 66, w, 9);
    m_prepStep = makeLabel(root, serif(15, Regular, 0.2), chalkDim);
    m_prepStep->setGeometry(0, 84, w - 70, 22);
    m_prepTime = makeLabel(root, serif(15, SemiBold, 0.3), chalkDim);
    m_prepTime->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_prepTime->setGeometry(w - 70, 84, 70, 22);

    m_prepCancel = new LinkButton("Cancel", LinkButton::Inline, root);
    m_prepCancel->move(w - m_prepCancel->width(), 132);
    connect(m_prepCancel, &QAbstractButton::clicked, this, &MainWindow::cancelPrepare);
    m_prepRetry = new LinkButton("Try again", LinkButton::Inline, root);
    m_prepRetry->move(w - m_prepRetry->width(), 132);
    connect(m_prepRetry, &QAbstractButton::clicked, this, &MainWindow::startPrepare);
    m_prepLog = new LinkButton("Open log", LinkButton::Inline, root);
    m_prepLog->move(m_prepRetry->x() - 26 - m_prepLog->width(), 132);
    connect(m_prepLog, &QAbstractButton::clicked, this, [this]() {
        if (!m_testMode)
            QDesktopServices::openUrl(QUrl::fromLocalFile(QDir(gameDir()).filePath("build.log")));
    });

    m_buildTick = new QTimer(this);
    m_buildTick->setInterval(500);
    connect(m_buildTick, &QTimer::timeout, this, [this]() {
        const qint64 secs = m_buildClock.elapsed() / 1000;
        m_prepTime->setText(QString("%1:%2").arg(secs / 60).arg(secs % 60, 2, 10, QChar('0')));
    });
    showPrepareState(0);
    return root;
}

void MainWindow::showPrepareState(int state)
{
    const bool failed = state == 1;
    m_prepCancel->setVisible(!failed);
    m_prepRetry->setVisible(failed);
    m_prepLog->setVisible(failed);
    m_prepBar->setFailed(failed);
    m_prepTime->setVisible(!failed);
    setColor(m_prepText, failed ? roseLight : chalk);
    if (!failed)
        m_prepText->setText("The game is being built for your PC from your disc. "
                            "This is done once and takes a few minutes.");
}

void MainWindow::startPrepare()
{
    if (m_build || m_game)
        return;
    if (m_gameState != NeedsBuild) {     // (also reached from "Try again" and the test hook)
        refreshGameState();
        if (m_gameState != NeedsBuild) {
            emit prepareFinished(m_gameState == Ready || m_gameState == Bundled);
            return;
        }
    }
    QDir().mkpath(gameDir());
    m_buildOut.clear();
    m_buildError.clear();
    m_buildDone.clear();
    m_buildPercent = 0;
    m_prepBar->setValue(0);
    m_prepTime->setText("0:00");
    setElided(m_prepStep, "Starting…");
    showPrepareState(0);

    m_build = new QProcess(this);
    m_build->setProgram(builderPath());
    m_build->setArguments({"--iso", QDir::toNativeSeparators(m_disc.path), "--out",
                           QDir::toNativeSeparators(gameDir())});
    m_build->setWorkingDirectory(appDir());
    m_build->setStandardErrorFile(QProcess::nullDevice());     // the builder keeps its own log
#ifdef Q_OS_WIN
    m_build->setCreateProcessArgumentsModifier(
        [](QProcess::CreateProcessArguments *a) { a->flags |= CREATE_NO_WINDOW; });
#endif
    connect(m_build, &QProcess::readyReadStandardOutput, this, &MainWindow::onBuilderOutput);
    connect(m_build, &QProcess::finished, this, &MainWindow::onBuilderFinished);
    connect(m_build, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart)
            onBuilderFinished(-1, QProcess::CrashExit);
    });
    m_buildClock.start();
    m_buildTick->start();
    note("prepare: " + builderPath() + " --iso " + m_disc.path + " --out " + gameDir());
    m_build->start();
    updatePlayButton();
    setStatus(Info, "Preparing the game…");
    m_prepare->open();
}

// The builder talks in lines: "PROGRESS <done> <total> <text>", then
// "DONE <game program>" or "ERROR <what went wrong>".
void MainWindow::onBuilderOutput()
{
    if (!m_build)
        return;
    m_buildOut += m_build->readAllStandardOutput();
    int nl;
    while ((nl = m_buildOut.indexOf('\n')) >= 0) {
        const QString line = QString::fromLocal8Bit(m_buildOut.left(nl)).trimmed();
        m_buildOut.remove(0, nl + 1);
        if (line.startsWith("PROGRESS ")) {
            const int done = line.section(' ', 1, 1).toInt(), total = line.section(' ', 2, 2).toInt();
            if (total > 0) {
                m_buildPercent = qBound(0, done * 100 / total, 100);
                m_prepBar->setValue(qreal(done) / total);
            }
            setElided(m_prepStep, line.section(' ', 3));
            updatePlayButton();
        } else if (line.startsWith("DONE ")) {
            m_buildDone = line.mid(5).trimmed();
        } else if (line.startsWith("ERROR ")) {
            m_buildError = line.mid(6).trimmed();
        }
    }
}

void MainWindow::onBuilderFinished(int code, QProcess::ExitStatus st)
{
    if (!m_build)
        return;
    onBuilderOutput();
    const QString failedToStart = code == -1 && st == QProcess::CrashExit ? m_build->errorString() : QString();
    const bool ok = st == QProcess::NormalExit && code == 0 && QFileInfo(builtGamePath()).isFile();
    endBuild();
    if (ok) {
        m_gameState = Ready;
        updatePlayButton();
        m_prepare->close();
        setStatus(Info, "The game is ready.");
        note("prepare: done -> " + builtGamePath());
        emit prepareFinished(true);
        if (m_playAfterBuild) {
            m_playAfterBuild = false;
            play();
        }
        return;
    }
    m_playAfterBuild = false;
    QString why = m_buildError;
    if (why.isEmpty())
        why = !failedToStart.isEmpty() ? failedToStart
              : st != QProcess::NormalExit ? QString("The builder stopped unexpectedly.")
              : QString("The builder ended without a game (code %1).").arg(code);
    m_gameState = NeedsBuild;
    updatePlayButton();
    showPrepareState(1);
    m_prepText->setText("The game could not be prepared.");
    setElided(m_prepStep, why);
    setStatus(Bad, "The game could not be prepared.",
              why + "  ·  Log: " + QDir::toNativeSeparators(QDir(gameDir()).filePath("build.log")));
    m_prepare->open();
    emit prepareFinished(false);
}

void MainWindow::cancelPrepare()
{
    if (!m_build)
        return;
    m_build->disconnect(this);
    m_build->kill();                    // our own child, by its handle
    m_build->waitForFinished(5000);
    endBuild();
    m_playAfterBuild = false;
    m_prepare->close();
    refreshGameState();
    setStatus(Info, "Preparing the game was cancelled.");
    note("prepare: cancelled");
    emit prepareFinished(false);
}

void MainWindow::endBuild()
{
    m_buildTick->stop();
    if (m_build) {
        m_build->disconnect(this);
        m_build->deleteLater();
        m_build = nullptr;
    }
}

QString MainWindow::findGameBinary(QString *workDir)
{
    if (workDir)
        *workDir = appDir();
    const QFileInfo packaged(QDir(appDir()), kPackagedGame);
    if (packaged.isFile())
        return packaged.absoluteFilePath();
    QDir d(appDir());
    for (int up = 0; up < 6; ++up) {
        const QFileInfo dev(d, kDevGame);
        if (dev.isFile()) {
            // Development builds are run from the repository root.
            if (workDir)
                *workDir = d.absolutePath();
            return dev.absoluteFilePath();
        }
        if (!d.cdUp())
            break;
    }
    const QString env = qEnvironmentVariable("ROR_GAME");
    if (!env.isEmpty() && QFileInfo(env).isFile())
        return QFileInfo(env).absoluteFilePath();
    return {};
}

void MainWindow::play()
{
    if (m_game)
        return;
    if (m_build) {               // the button reads "PREPARING": show the panel again
        m_prepare->open();
        return;
    }
    // The image may have been moved or replaced since it was checked.
    m_disc = checkDisc(m_disc.path);
    if (!m_disc.ok()) {
        setDisc(m_disc.path, false);
        emit gameFinished(false);
        return;
    }
    // Never start a game that does not belong to this disc image and this
    // package: ask the builder again (it answers at once).
    const bool saidPlay = m_gameState == Ready;
    refreshGameState();
    if (m_gameState == NoBuilder) {
        emit gameFinished(false);
        return;
    }
    if (m_gameState == NeedsBuild) {
        m_playAfterBuild = saidPlay;      // PLAY was pressed: go on to the game afterwards
        if (m_dryRun) {
            printf("[dry-run] action: prepare the game first\n[dry-run] builder: %s\n",
                   builderPath().toLocal8Bit().constData());
            for (const QString &a : QStringList{"--iso", QDir::toNativeSeparators(m_disc.path),
                                                "--out", QDir::toNativeSeparators(gameDir())})
                printf("[dry-run] builder arg: %s\n", a.toLocal8Bit().constData());
            printf("[dry-run] then play: %s\n", builtGamePath().toLocal8Bit().constData());
            fflush(stdout);
            emit gameFinished(true);
            return;
        }
        startPrepare();
        return;
    }
    QString workDir;
    QString game = findGameBinary(&workDir);
    if (m_gameState == Ready) {
        game = builtGamePath();
        workDir = appDir();
    }
    if (game.isEmpty()) {
        gameEnded(false, "The game program is missing.",
                  QString("Expected %1 next to the launcher, in %2")
                      .arg(kPackagedGame, QDir::toNativeSeparators(appDir())));
        return;
    }

    const QStringList args = gameArguments() + m_extraArgs;

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.remove("ROR_NO_MC");
    env.remove("ROR_NOAUDIO");
    env.remove("ROR_MC_DIR");
    if (m_memcard->isChecked()) {
        const QString mc = QDir(appDir()).filePath("saves/mc0");
        if (!m_dryRun && !QDir().mkpath(mc)) {       // (a dry run leaves no folder behind)
            gameEnded(false, "Cannot create the save folder.",
                      QDir::toNativeSeparators(mc) +
                          "  ·  move the game to a folder you can write to, or switch "
                          "the memory card off in Options");
            return;
        }
        env.insert("ROR_MC_DIR", QDir::toNativeSeparators(mc));
    } else {
        env.insert("ROR_NO_MC", "1");
    }
    if (!m_sound->isChecked())
        env.insert("ROR_NOAUDIO", "1");

    // A game built on this PC lives in the user's data folder; it finds the
    // files that stay with the launcher through this.
    if (m_gameState == Ready)
        env.insert("ROR_APP_DIR", QDir::toNativeSeparators(appDir()));

    if (m_dryRun) {
        // --dry-run: say exactly what would be started, and stop there.
        printf("[dry-run] program: %s\n[dry-run] cwd: %s\n", game.toLocal8Bit().constData(),
               workDir.toLocal8Bit().constData());
        for (const QString &a : args)
            printf("[dry-run] arg: %s\n", a.toLocal8Bit().constData());
        for (const char *k : {"ROR_MC_DIR", "ROR_NO_MC", "ROR_NOAUDIO"})
            printf("[dry-run] env: %s=%s\n", k,
                   env.contains(k) ? env.value(k).toLocal8Bit().constData() : "(unset)");
        if (env.contains("ROR_APP_DIR"))
            printf("[dry-run] env: ROR_APP_DIR=%s\n", env.value("ROR_APP_DIR").toLocal8Bit().constData());
        fflush(stdout);
        gameEnded(true, {});
        return;
    }

    m_game = new QProcess(this);
    m_game->setProgram(game);
    m_game->setArguments(args);
    m_game->setWorkingDirectory(workDir);
    m_game->setProcessEnvironment(env);
#ifdef Q_OS_WIN
    // The game is a console program: start it without a console window.
    m_game->setCreateProcessArgumentsModifier(
        [](QProcess::CreateProcessArguments *a) { a->flags |= CREATE_NO_WINDOW; });
#endif
    // The game's console output goes to game.log beside the launcher when
    // that is writable; the last line is shown if the game fails.
    m_logPath = QDir(appDir()).filePath("game.log");
    QFile probe(m_logPath);
    if (probe.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        probe.close();
        m_game->setProcessChannelMode(QProcess::MergedChannels);
        m_game->setStandardOutputFile(m_logPath);
    } else {
        m_logPath.clear();
        m_game->setProcessChannelMode(QProcess::ForwardedChannels);
    }
    connect(m_game, &QProcess::errorOccurred, this, &MainWindow::onGameError);
    connect(m_game, &QProcess::finished, this, &MainWindow::onGameFinished);
    connect(m_game, &QProcess::started, this, [this]() {
        setStatus(Info, "The game is running.", "Esc in the game window quits");
    });

    m_play->setEnabled(false);
    setStatus(Info, "Starting the game…");
    note("exec: " + game + " " + args.join(' '));
    m_gameClock.start();
    m_game->start();
}

QString MainWindow::lastLogLine() const
{
    QFile f(m_logPath);
    if (m_logPath.isEmpty() || !f.open(QIODevice::ReadOnly))
        return {};
    if (f.size() > 4096)
        f.seek(f.size() - 4096);
    const QList<QByteArray> lines = f.readAll().split('\n');
    for (auto it = lines.crbegin(); it != lines.crend(); ++it) {
        const QString s = QString::fromLocal8Bit(*it).trimmed();
        if (!s.isEmpty())
            return s.left(160);
    }
    return {};
}

void MainWindow::onGameError(QProcess::ProcessError err)
{
    if (err != QProcess::FailedToStart || !m_game)
        return; // crashes are reported by finished()
    gameEnded(false, "The game could not be started.",
              QDir::toNativeSeparators(m_game->program()) + "  ·  " + m_game->errorString());
}

void MainWindow::onGameFinished(int exitCode, QProcess::ExitStatus status)
{
    if (!m_game)
        return;
    if (status == QProcess::NormalExit && exitCode == 0) {
        gameEnded(true, {});
        return;
    }
    const qint64 secs = m_gameClock.elapsed() / 1000;
    QString msg = status == QProcess::CrashExit
                      ? QString("The game crashed or was closed by the system")
                      : QString("The game stopped with an error (code %1)").arg(exitCode);
    msg += secs < 3 ? QString(" as soon as it started.") : QString(" after %1 s.").arg(secs);
    QStringList detail;
    const QString last = lastLogLine();
    if (!last.isEmpty())
        detail << last;
    if (!m_logPath.isEmpty())
        detail << "Log: " + QDir::toNativeSeparators(m_logPath);
    gameEnded(false, msg, detail.join("  ·  "));
}

void MainWindow::gameEnded(bool ok, const QString &text, const QString &detail)
{
    if (m_game) {
        m_game->disconnect(this);
        m_game->deleteLater();
        m_game = nullptr;
    }
    updatePlayButton();
    setStatus(ok ? Info : Bad, text, detail);
    emit gameFinished(ok);
    // The launcher window was closed while the game ran: nothing left to do.
    if (m_shown && !isVisible())
        QCoreApplication::quit();
}

void MainWindow::closeEvent(QCloseEvent *e)
{
    // Closing while the game is being prepared cancels that (the builder
    // leaves nothing half-made behind).
    if (m_build) {
        m_build->disconnect(this);
        m_build->kill();
        m_build->waitForFinished(5000);
        endBuild();
    }
    // Closing the launcher must not take the running game down with it:
    // hide, and quit once the game ends.
    if (m_game) {
        hide();
        e->ignore();
        return;
    }
    QWidget::closeEvent(e);
}
