list(APPEND WTF_SOURCES
    android/LoggingAndroid.cpp
    android/RefPtrAndroid.cpp

    generic/MainThreadGeneric.cpp
    generic/WorkQueueGeneric.cpp

    glib/Application.cpp
    glib/FilePathWatcher.cpp
    glib/FileSystemGlib.cpp
    glib/GMallocString.cpp
    glib/GRefPtr.cpp
    glib/GResources.cpp
    glib/GSocketMonitor.cpp
    glib/GSpanExtras.cpp
    glib/RunLoopGLib.cpp
    glib/Sandbox.cpp
    glib/SocketConnection.cpp
    glib/TimeZoneGLib.cpp
    glib/URLGLib.cpp
)

if (WIN32)
    # The operating system side is the Win port's; the run loop and the
    # main thread stay GLib's.
    list(REMOVE_ITEM WTF_SOURCES
        generic/MainThreadGeneric.cpp
    )
    list(APPEND WTF_SOURCES
        text/win/StringWin.cpp
        text/win/TextBreakIteratorInternalICUWin.cpp

        win/CPUTimeWin.cpp
        win/DbgHelperWin.cpp
        win/FileHandleWin.cpp
        win/FileSystemWin.cpp
        win/LanguageWin.cpp
        win/LoggingWin.cpp
        win/MainThreadWin.cpp
        win/MappedFileDataWin.cpp
        win/MemoryFootprintWin.cpp
        win/MemoryPressureHandlerWin.cpp
        win/OSAllocatorWin.cpp
        win/PathWalker.cpp
        win/SignalsWin.cpp
        win/ThreadingWin.cpp
        win/WTFCRTDebug.cpp
        win/Win32Handle.cpp
    )
    list(APPEND WTF_PUBLIC_HEADERS
        text/win/WCharStringExtras.h

        win/DbgHelperWin.h
        win/GDIObject.h
        win/PathWalker.h
        win/SoftLinking.h
        win/WTFCRTDebug.h
        win/Win32Handle.h
    )
    # GLib's translation macros call libintl, a library of its own there.
    find_package(Intl REQUIRED)
    list(APPEND WTF_LIBRARIES
        dbghelp
        Intl::Intl
        shlwapi
        synchronization
        winmm
        ws2_32
    )
else ()
    list(APPEND WTF_SOURCES
        posix/CPUTimePOSIX.cpp
        posix/FileHandlePOSIX.cpp
        posix/FileSystemPOSIX.cpp
        posix/MappedFileDataPOSIX.cpp
        posix/OSAllocatorPOSIX.cpp
        posix/ThreadingPOSIX.cpp

        text/unix/TextBreakIteratorInternalICUUnix.cpp

        unix/LanguageUnix.cpp
        unix/LoggingUnix.cpp
        unix/UniStdExtrasUnix.cpp
    )
endif ()

if (ANDROID)
    list(APPEND WTF_SOURCES
        generic/MemoryFootprintGeneric.cpp
        generic/MemoryPressureHandlerGeneric.cpp

        linux/CurrentProcessMemoryStatus.cpp
        linux/HighPriorityThreads.cpp
    )
elseif (APPLE)
    # The signal handlers of an Apple OS are Mach exception handlers, whatever
    # the port (as in PlatformJSCOnly.cmake).
    file(COPY mac/MachExceptions.defs DESTINATION ${WTF_DERIVED_SOURCES_DIR})
    add_custom_command(
        OUTPUT
            ${WTF_DERIVED_SOURCES_DIR}/MachExceptionsServer.h
            ${WTF_DERIVED_SOURCES_DIR}/mach_exc.h
            ${WTF_DERIVED_SOURCES_DIR}/mach_excServer.c
            ${WTF_DERIVED_SOURCES_DIR}/mach_excUser.c
        MAIN_DEPENDENCY mac/MachExceptions.defs
        WORKING_DIRECTORY ${WTF_DERIVED_SOURCES_DIR}
        COMMAND ${Mig_EXECUTABLE} -header mach_exc.h -user mach_excUser.c
            -sheader MachExceptionsServer.h -server mach_excServer.c
            -DMACH_EXC_SERVER_TASKIDTOKEN_STATE -isysroot ${CMAKE_OSX_SYSROOT}
            MachExceptions.defs
        VERBATIM)
    list(APPEND WTF_SOURCES
        cocoa/MemoryFootprintCocoa.cpp

        generic/MemoryPressureHandlerGeneric.cpp

        ${WTF_DERIVED_SOURCES_DIR}/mach_excServer.c
        ${WTF_DERIVED_SOURCES_DIR}/mach_excUser.c
    )
    set_source_files_properties(darwin/OSLogPrintStream.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
    # USE(DARWIN_SYSTEM_TYPES): what the Cocoa ports' source list has of it.
    list(APPEND WTF_SOURCES
        cocoa/MachSendRight.cpp

        text/cf/StringCF.cpp
        text/cf/StringImplCF.cpp

        text/cocoa/StringCocoa.mm
        text/cocoa/StringImplCocoa.mm

        BlockObjCExceptions.mm
    )
    set_source_files_properties(
        text/cocoa/StringCocoa.mm
        text/cocoa/StringImplCocoa.mm
        BlockObjCExceptions.mm
        PROPERTIES COMPILE_OPTIONS -fobjc-arc
    )
    list(APPEND WTF_PUBLIC_HEADERS
        cf/CFTypeTraits.h

        cocoa/NSTypeTraits.h
    )
    # GLib's translation macros call libintl, which is no part of the C
    # library there.
    find_package(Intl REQUIRED)
    list(APPEND WTF_LIBRARIES
        Intl::Intl
        objc
        "-framework CoreFoundation"
        "-framework Foundation"
    )
elseif (NOT WIN32)
    list(APPEND WTF_SOURCES
        linux/CurrentProcessMemoryStatus.cpp
        linux/HighPriorityThreads.cpp
        linux/MemoryFootprintLinux.cpp

        unix/MemoryPressureHandlerUnix.cpp
    )
endif ()

list(APPEND WTF_PUBLIC_HEADERS
    PlatformEnableGlib.h

    android/RefPtrAndroid.h

    glib/ActivityObserver.h
    glib/Application.h
    glib/FilePathWatcher.h
    glib/GLibExtras.h
    glib/GMallocString.h
    glib/GRefPtr.h
    glib/GResources.h
    glib/GSocketMonitor.h
    glib/GSpanExtras.h
    glib/GThreadSafeWeakPtr.h
    glib/GTypedefs.h
    glib/GUniquePtr.h
    glib/GWeakPtr.h
    glib/RunLoopSourcePriority.h
    glib/Sandbox.h
    glib/SocketConnection.h
    glib/SysprofAnnotator.h
    glib/WTFGType.h

    linux/CurrentProcessMemoryStatus.h
    linux/HighPriorityThreads.h
    linux/ProcessMemoryFootprint.h

    posix/SocketPOSIX.h

    unix/UnixFileDescriptor.h
)

list(APPEND WTF_LIBRARIES
    GLib::Gio
    Threads::Threads
    ZLIB::ZLIB
)

list(APPEND WTF_PRIVATE_DEFINITIONS
    PKGDATADIR="${CMAKE_INSTALL_FULL_DATADIR}/wpe-webkit-${WPE_API_VERSION}"
)

if (ENABLE_JOURNALD_LOG)
    list(APPEND WTF_LIBRARIES Journald::Journald)
endif ()

if (ANDROID)
    list(APPEND WTF_LIBRARIES Android::Android Android::Log)
endif ()

if (USE_LIBBACKTRACE)
    list(APPEND WTF_LIBRARIES
        LIBBACKTRACE::LIBBACKTRACE
    )
endif ()

if (USE_SYSPROF_CAPTURE)
    list(APPEND WTF_LIBRARIES
        SysProfCapture::SysProfCapture
    )
endif ()
