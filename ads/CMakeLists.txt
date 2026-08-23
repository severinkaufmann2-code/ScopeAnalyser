set(ADS_HEADERS
    include/scope/ads/AdsTypeNames.h
    include/scope/ads/BeckhoffOpenAdsClient.h
    include/scope/ads/RouterAdsClient.h
    include/scope/ads/MockAdsClient.h
)

set(ADS_SOURCES
    src/AdsTypeNames.cpp
    src/BeckhoffOpenAdsClient.cpp
    src/RouterAdsClient.cpp
    src/MockAdsClient.cpp
)

scope_add_library(scope_ads
    SOURCES ${ADS_SOURCES}
    HEADERS ${ADS_HEADERS}
    PUBLIC_INCLUDES
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PUBLIC_LIBS
        scope::scope_core
        ads
    PRIVATE_LIBS
        spdlog::spdlog
    FOLDER scope/ads
)
