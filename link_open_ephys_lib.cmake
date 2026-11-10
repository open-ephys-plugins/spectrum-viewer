function(link_open_ephys_lib target libname)
	if(NOT TARGET ${libname})
		add_library(${libname} SHARED IMPORTED)
	endif()

	if(MSVC)
		set(LIBLOC IMPORTED_IMPLIB)
	else()
		set(LIBLOC IMPORTED_LOCATION)
	endif()

	# Release and Debug are always looked for. Multi-config generators add
	# their configuration types, and single-config generators (Linux, macOS
	# Makefiles/Ninja) contribute CMAKE_BUILD_TYPE, since they do not populate
	# CMAKE_CONFIGURATION_TYPES.
	set(LIB_CONFIGS Release Debug ${CMAKE_CONFIGURATION_TYPES} ${CMAKE_BUILD_TYPE})
	list(REMOVE_DUPLICATES LIB_CONFIGS)

	set(FOUND_CONFIGS "")
	set(MISSING_CONFIGS "")

	foreach(config ${LIB_CONFIGS})
		string(TOUPPER ${config} CONFIG)

		# find_library caches its result, so a library installed after the first
		# configure would otherwise never be picked up.
		unset(LIB_PATH_${CONFIG} CACHE)
		find_library(LIB_PATH_${CONFIG} NAMES ${libname}
			PATHS ${GUI_COMMONLIB_DIR}/${config}/lib/${CMAKE_LIBRARY_ARCHITECTURE})

		if(LIB_PATH_${CONFIG})
			set_target_properties(${libname} PROPERTIES ${LIBLOC}_${CONFIG} ${LIB_PATH_${CONFIG}})
			set_property(TARGET ${libname} APPEND PROPERTY IMPORTED_CONFIGURATIONS ${CONFIG})
			list(APPEND FOUND_CONFIGS ${config})
		else()
			list(APPEND MISSING_CONFIGS ${config})
		endif()
	endforeach()

	if(NOT FOUND_CONFIGS)
		message(WARNING "${libname} not found in ${GUI_COMMONLIB_DIR} - linking will fail.")
	else()
		# A configuration without its own library links to the closest one that
		# exists: Release, then Debug, then whatever was found.
		list(GET FOUND_CONFIGS 0 FALLBACK_CONFIG)
		if("Release" IN_LIST FOUND_CONFIGS)
			set(FALLBACK_CONFIG Release)
		elseif("Debug" IN_LIST FOUND_CONFIGS)
			set(FALLBACK_CONFIG Debug)
		endif()
		string(TOUPPER ${FALLBACK_CONFIG} FALLBACK_CONFIG_UPPER)

		# Also used for any configuration not listed above, such as a custom one.
		set_target_properties(${libname} PROPERTIES
			${LIBLOC} ${LIB_PATH_${FALLBACK_CONFIG_UPPER}})

		foreach(config ${MISSING_CONFIGS})
			string(TOUPPER ${config} CONFIG)
			message(WARNING "${libname} ${config} library not found - will link to ${FALLBACK_CONFIG} version instead.")
			set_target_properties(${libname} PROPERTIES
				MAP_IMPORTED_CONFIG_${CONFIG} ${FALLBACK_CONFIG})
		endforeach()
	endif()

	target_link_libraries(${target} ${libname})
endfunction(link_open_ephys_lib)