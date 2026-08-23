# ads_dt_probe — a read-only console tool that dumps a PLC's symbol and
# data-type tables to files, so the type-table parser can be developed against
# real bytes rather than against an assumed wire layout. Built alongside the
# app so the Windows CI artifact carries it; it links the same RouterAdsClient
# the Recorder connects with.
add_executable(ads_dt_probe ads_dt_probe/main.cpp ${SCOPE_APP_ICON_RC})
target_link_libraries(ads_dt_probe PRIVATE
    scope::scope_core
    scope::scope_ads
    Qt6::Core
)
set_target_properties(ads_dt_probe PROPERTIES FOLDER scope/tools)
