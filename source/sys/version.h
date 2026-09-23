#ifndef VERSION_H
#define VERSION_H

// The Makefile sets APP_VERSION from `git describe` (a release build gets
// the tag). A build that somehow bypasses it still says something.
#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif

#endif
