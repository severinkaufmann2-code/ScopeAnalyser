set(CORE_HEADERS
    include/scope/core/Types.h
    include/scope/core/Signal.h
    include/scope/core/SignalStore.h
    include/scope/core/Hdf5Session.h
    include/scope/core/Mdf4Session.h
    include/scope/core/SvdxReader.h
    include/scope/core/IAdsClient.h
)

set(CORE_SOURCES
    src/Signal.cpp
    src/SignalStore.cpp
    src/Hdf5Session.cpp
    src/Mdf4Session.cpp
    src/SvdxReader.cpp
)

scope_add_library(scope_core
    SOURCES ${CORE_SOURCES}
    HEADERS ${CORE_HEADERS}
    PUBLIC_INCLUDES
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PUBLIC_LIBS
        Qt6::Core
        spdlog::spdlog
        nlohmann_json::nlohmann_json
        HighFive
        concurrentqueue
        mdf
    FOLDER scope/core
)
