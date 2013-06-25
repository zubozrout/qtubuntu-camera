include(../../coverage.pri)

CONFIG += no_private_qt_headers_warning
TARGET = tst_aalcamerazoomcontrol

QT += testlib multimedia-private opengl

LIBS += -L../mocks/aal -laal
INCLUDEPATH += ../../src
INCLUDEPATH += ../mocks/aal

HEADERS += ../../src/aalcamerazoomcontrol.h \
    ../../src/aalcameraservice.h

SOURCES += tst_aalcamerazoomcontrol.cpp \
    ../../src/aalcamerazoomcontrol.cpp \
    aalcameraservice.cpp

check.depends = $${TARGET}
check.commands = ./$${TARGET}
QMAKE_EXTRA_TARGETS += check
