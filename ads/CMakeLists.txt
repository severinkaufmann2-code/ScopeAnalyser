set(ADS_HEADERS
    include/scope/ads/BeckhoffOpenAdsClient.h
)

set(ADS_SOURCES
    src/BeckhoffOpenAdsClient.cpp
)

scope_add_library(scope_ads
    SOURCES ${ADS_SOURCES}
    HEADERS ${ADS_HEADERS}
    PUBLIC_INCLUDES
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PUBLIC_LIBS
        scope::scope_core
    PRIVATE_LIBS
        spdlog::spdlog
    FOLDER scope/ads
)

# The Beckhoff/ADS open-source library is fetched + linked here.
# vcpkg port name pending; provided as FetchContent until added to the manifest.
include(FetchContent)
FetchContent_Declare(
    beckhoff_ads
    GIT_REPOSITORY https://github.com/Beckhoff/ADS.git
    GIT_TAG        main
)
FetchContent_MakeAvailable(beckhoff_ads)
target_link_libraries(scope_ads PUBLIC ads)
