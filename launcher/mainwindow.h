#pragma once
#include <QElapsedTimer>
#include <QPixmap>
#include <QProcess>
#include <QWidget>

#include "discinfo.h"

class Dropdown;
class GlyphButton;
class LinkButton;
class Overlay;
class PlayButton;
class QLabel;
class QTimer;
class Segmented;
class Slider;
class Toggle;
class Veil;

// The launcher window: the title card, the disc line, PLAY, and two overlay
// panels (Options, Controls). Frameless, fixed size.
class MainWindow : public QWidget {
    Q_OBJECT
public:
    // isoOverride: disc image given on the command line (not remembered).
    explicit MainWindow(const QString &isoOverride = {}, QWidget *parent = nullptr);

    // Where the game executable is, or empty: "ror-game" beside the launcher,
    // then build/recomp/ror in a folder above it, then $ROR_GAME. workDir
    // receives the directory to start it in.
    static QString findGameBinary(QString *workDir = nullptr);

    // For --show / --hover on the command line (pictures of each state).
    void showPanel(const QString &name);
    void forcePlayHover();
    // --dry-run: PLAY prints the command line and environment instead of
    // starting the game.
    void setDryRun(bool on) { m_dryRun = on; }
    // Test modes (--grab, --dry-run): the window is on a live desktop, so it
    // must not take the keyboard focus, and PLAY by key or click does nothing
    // (a stray Enter once started a real game); only --autoplay plays.
    void setTestMode(bool on);
    void testSet(const QString &name, const QString &value);   // --set name=value
    // --extra-arg X (test hook): appended to the game's command line.
    void addExtraArg(const QString &a) { m_extraArgs << a; }

public slots:
    void play();

signals:
    // The game process ended (ok = clean exit) or could not be started.
    void gameFinished(bool ok);

protected:
    void paintEvent(QPaintEvent *) override;
    void showEvent(QShowEvent *e) override;
    void closeEvent(QCloseEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dropEvent(QDropEvent *e) override;

private slots:
    void browseDisc();
    void onGameError(QProcess::ProcessError err);
    void onGameFinished(int exitCode, QProcess::ExitStatus status);

private:
    enum Level { Info, Bad };
    QWidget *buildOptions();
    QWidget *buildControls();
    void selectOptionsTab(int index);
    void updateOptionStates();
    QStringList gameArguments() const;
    void setDisc(const QString &path, bool remember);
    void layoutDisc();
    void setStatus(Level level, const QString &text, const QString &detail = {});
    void gameEnded(bool ok, const QString &text, const QString &detail = {});
    QString lastLogLine() const;
    Overlay *openOverlay() const;

    DiscInfo m_disc;
    QString m_logPath;
    QProcess *m_game = nullptr;
    QElapsedTimer m_gameClock;
    QPixmap m_bg;
    bool m_shown = false;
    bool m_dryRun = false;
    bool m_testMode = false;
    QStringList m_extraArgs;

    QLabel *m_discMain = nullptr;
    QLabel *m_discSub = nullptr;
    LinkButton *m_discLink = nullptr;     // "Change…" beside the disc name
    LinkButton *m_discCall = nullptr;     // "Select disc image…" when there is none
    QLabel *m_status = nullptr;
    QLabel *m_statusDetail = nullptr;
    PlayButton *m_play = nullptr;
    // Options panel
    enum { kTabs = 4 };              // Display, Quality, Image, Game
    LinkButton *m_optTabs[kTabs] = {};
    QWidget *m_optPages[kTabs] = {};
    Segmented *m_renderer = nullptr, *m_mode = nullptr, *m_wide = nullptr, *m_filter = nullptr;
    Dropdown *m_res = nullptr, *m_fps = nullptr, *m_internal = nullptr;
    Toggle *m_vsync = nullptr, *m_stretch = nullptr;
    Segmented *m_pad = nullptr;
    Slider *m_deadzone = nullptr;
    QLabel *m_deadzoneLabel = nullptr;
    QLabel *m_displayNote = nullptr;
    QLabel *m_fsrNote = nullptr, *m_fsrNote2 = nullptr;
    Slider *m_grain = nullptr;
    QLabel *m_fpsLabel = nullptr, *m_vsyncLabel = nullptr, *m_internalLabel = nullptr,
           *m_qualityNote = nullptr;
    QString m_desktopRes;            // primary screen in real pixels, "WxH"
    Slider *m_sharp = nullptr, *m_bright = nullptr, *m_contrast = nullptr, *m_gamma = nullptr,
           *m_saturation = nullptr, *m_bloom = nullptr;
    QLabel *m_resLabel = nullptr, *m_sharpLabel = nullptr;
    Toggle *m_sound = nullptr;
    Toggle *m_memcard = nullptr;
    Overlay *m_options = nullptr;
    Overlay *m_controls = nullptr;
    Veil *m_veil = nullptr;
};
