#pragma once
#include <QString>

// What we know about a disc image the user pointed at.
struct DiscInfo {
    enum State {
        Missing,     // no path, or the file does not exist / cannot be read
        NotIso,      // not an ISO9660 image (2048-byte sectors)
        WrongDisc,   // ISO9660, but not a Rule of Rose disc this port knows
        Ok,
    };
    State state = Missing;
    QString path;      // absolute path that was checked
    QString serial;    // boot file found on the disc, e.g. "SLES-54218" (may be empty)
    QString region;    // Ok: "Europe" or "USA"
    int hz = 50;       // Ok: the game's video rate, 50 (European disc) or 60 (USA disc)
    bool european() const { return hz == 50; }
    QString message;   // one-line description for the UI

    bool ok() const { return state == Ok; }
};

// The discs the port can be prepared from, for messages:
// "the European (SLES-54218) or USA (SLUS-21448) release".
QString supportedDiscsText();

// Look for a default-named image ("Rule of Rose (Europe) (En,Fr,De,Es,It).iso",
// "Rule of Rose (USA).iso" or "RuleOfRose.iso") beside the launcher and up to four folders above it,
// then in the current directory. Empty string if none.
QString findDiscIso();

// Check that `path` is one of the supported discs: an ISO9660 image whose root
// directory holds the boot file SLES_542.18 (Europe) or SLUS_214.48 (USA).
DiscInfo checkDisc(const QString &path);
