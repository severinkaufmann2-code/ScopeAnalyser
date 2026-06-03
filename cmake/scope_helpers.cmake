function(scope_add_library target)
    set(options STATIC SHARED)
    set(oneValueArgs FOLDER)
    set(multiValueArgs SOURCES HEADERS PUBLIC_LIBS PRIVATE_LIBS PUBLIC_INCLUDES PRIVATE_INCLUDES)
    cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(ARG_SHARED)
        add_library(${target} SHARED ${ARG_SOURCES} ${ARG_HEADERS})
    else()
        add_library(${target} STATIC ${ARG_SOURCES} ${ARG_HEADERS})
    endif()

    add_library(scope::${target} ALIAS ${target})

    target_include_directories(${target}
        PUBLIC  ${ARG_PUBLIC_INCLUDES}
        PRIVATE ${ARG_PRIVATE_INCLUDES})

    if(ARG_PUBLIC_LIBS)
        target_link_libraries(${target} PUBLIC ${ARG_PUBLIC_LIBS})
    endif()
    if(ARG_PRIVATE_LIBS)
        target_link_libraries(${target} PRIVATE ${ARG_PRIVATE_LIBS})
    endif()

    if(ARG_FOLDER)
        set_target_properties(${target} PROPERTIES FOLDER ${ARG_FOLDER})
    endif()
endfunction()
