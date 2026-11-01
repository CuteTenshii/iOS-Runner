cmake_minimum_required(VERSION 3.22.1)
project(iosrunner CXX)

add_library(iosrunner SHARED jni_bridge.cpp engine.cpp macho.cpp symbols.cpp plist.cpp uikit_assets.cpp uikit_collection.cpp uuid.cpp base64.cpp predicate.cpp device_shim.cpp security_shim.cpp uikit_app.cpp uikit_nib.cpp uikit_view.cpp uikit_render.cpp uikit_layout.cpp uikit_constants.cpp uikit_properties.cpp darwin_libc.cpp darwin_io.cpp darwin_thread.cpp darwin_system.cpp swift_runtime.cpp type_system.cpp type_witnesses.cpp type_builders.cpp type_mangling.cpp type_metadata.cpp swiftui_shim.cpp swiftui_views.cpp swiftui_modifiers.cpp swiftui_state.cpp swiftui_layout.cpp swiftui_render.cpp objc_runtime.cpp foundation_shim.cpp foundation_objects.cpp foundation_url.cpp foundation_request.cpp foundation_web.cpp foundation_date.cpp foundation_string.cpp foundation_session.cpp stdlib_shim.cpp stdlib_collections.cpp stdlib_strings.cpp stdlib_runtime.cpp concurrency_shim.cpp localization.cpp key_path.cpp json.cpp codable_shim.cpp regex_shim.cpp coredata_shim.cpp combine_shim.cpp dispatch_shim.cpp)
target_compile_options(iosrunner PRIVATE -Wall -Wextra)
target_link_options(iosrunner PRIVATE -Wl,-z,max-page-size=16384)
target_link_libraries(iosrunner PRIVATE log)

# A debugging aid for heap corruption, turned on through the iosrunner.efence Gradle property.
if(IOSRUNNER_EFENCE)
    target_sources(iosrunner PRIVATE ../../../../tools/efence.cpp)
    target_link_options(iosrunner PRIVATE -Wl,--wrap=malloc,--wrap=calloc,--wrap=free,--wrap=realloc,--wrap=posix_memalign,--wrap=malloc_usable_size)
    if(IOSRUNNER_EFENCE_AFTER)
        target_compile_definitions(iosrunner PRIVATE "IOSRUNNER_EFENCE_AFTER=\"${IOSRUNNER_EFENCE_AFTER}\"")
    endif()
endif()
