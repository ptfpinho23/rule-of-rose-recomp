#pragma once
#include <QElapsedTimer>
#include <QPixmap>
#include <QProcess>
#include <QWidget>

#include "discinfo.h"
#include "gpuprobe.h"

class Dropdown;
class GlyphButton;
class LinkButton;
class Overlay;
class PlayButton;
class QLabel;
class ProgressLine;
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
    ~MainWindow() override;

    // Where the game executable is, or empty: "ror-game" beside the launcher,
    // then build/recomp/ror in a folder above it, then $ROR_GAME. workDir
    // receives the directory to start it in.
    QString findGameBinary(QString *workDir = nullptr) const;

    // Where a game built on this PC lives (see the "prepare" step in the
    // .cpp). Default: <per-user application data>/game.
    // The graphics hardware found at start (main() sets it before the window
    // is made), and the number of processor threads to assume (0 = ask the
    // system; a test override otherwise).
    static void setGraphics(const GpuInfo &gpu, int threads = 0);

    static void setGameDir(const QString &dir);
    static QString gameDir();

    // Test hooks: start the prepare step as a press on PREPARE GAME would
    // (prepareFinished reports how it ended), and cancel it.
    void startPrepare();
    void cancelPrepare();

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
    // The prepare step ended: the game is ready (ok), or it failed / was cancelled.
    void prepareFinished(bool ok);

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

    // The game on this PC. With a game program beside the launcher (the full
    // local package, developer builds) nothing here applies. Otherwise the
    // game is made from the player's disc by the builder program beside the
    // launcher, into gameDir(), once - and again when the disc image or the
    // package changes.
    enum GameState { Bundled, NoBuilder, NeedsBuild, Ready };
    QWidget *buildPrepare();
    QString builderPath() const;
    QString builtGamePath() const;
    QString discGameDir() const;      // gameDir()/<serial of the chosen disc>
    QString saveDir() const;          // the memory-card folder of the chosen disc
    void refreshGameState();
    void updatePlayButton();
    void showPrepareState(int state);          // 0 working, 1 failed
    void onBuilderOutput();
    void onBuilderFinished(int code, QProcess::ExitStatus st);
    void endBuild();
    void stopBuilder();

    DiscInfo m_disc;
    QString m_logPath;
    QProcess *m_game = nullptr;
    QElapsedTimer m_gameClock;
    QPixmap m_bg;
    bool m_shown = false;
    bool m_dryRun = false;
    bool m_testMode = false;

    GameState m_gameState = Bundled;
    QProcess *m_build = nullptr;
    QByteArray m_buildOut;
    QString m_buildError, m_buildDone;
    int m_buildPercent = 0;
    int m_buildTotal = 0, m_buildBaseDone = 0;      // see onBuilderOutput
    qreal m_buildBaseShown = 0, m_buildShown = 0;
    bool m_playAfterBuild = false;
    QElapsedTimer m_buildClock;
    QTimer *m_buildTick = nullptr;
    Overlay *m_prepare = nullptr;
    QLabel *m_prepText = nullptr, *m_prepStep = nullptr, *m_prepTime = nullptr;
    ProgressLine *m_prepBar = nullptr;
    LinkButton *m_prepCancel = nullptr, *m_prepLog = nullptr, *m_prepRetry = nullptr;
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
    QLabel *m_fsrNote = nullptr, *m_fsrNote2 = nullptr, *m_gpuLine = nullptr;
    QLabel *m_texLabel = nullptr;
    Toggle *m_texSmooth = nullptr;   // "Smooth distant textures"
    // Settings the player has not chosen follow the renderer and the
    // internal resolution (see applyFollowingDefaults in the .cpp).
    void applyFollowingDefaults();
    int defaultFilterIndex() const;
    QString settingsLine() const;    // for the log and --dry-run
    QSize outputSize() const;        // the size the picture is upscaled to with the current choices
    Slider *m_grain = nullptr;
    QLabel *m_fpsLabel = nullptr, *m_vsyncLabel = nullptr, *m_internalLabel = nullptr,
           *m_qualityNote = nullptr, *m_savePath = nullptr;
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
