#pragma once
#include <QString>

// What we know about a disc image the user pointed at.
struct DiscInfo {
    enum State {
        Missing,     // no path, or the file does not exist / cannot be read
        NotIso,      // not an ISO9660 image (2048-byte sectors)
        WrongDisc,   // ISO9660, but not Rule of Rose SLES-54218
        Ok,
    };
    State state = Missing;
    QString path;      // absolute path that was checked
    QString serial;    // boot file found on the disc, e.g. "SLES-54218" (may be empty)
    QString message;   // one-line description for the UI

    bool ok() const { return state == Ok; }
};

// The serial the port is built from (European release).
extern const char *const kDiscSerial;        // "SLES-54218"

// Look for a default-named image ("Rule of Rose (Europe) (En,Fr,De,Es,It).iso"
// or "RuleOfRose.iso") beside the launcher and up to four folders above it,
// then in the current directory. Empty string if none.
QString findDiscIso();

// Check that `path` is the right disc: an ISO9660 image whose root directory
// holds the boot file SLES_542.18.
DiscInfo checkDisc(const QString &path);
