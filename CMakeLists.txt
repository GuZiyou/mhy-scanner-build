cmake_minimum_required(VERSION 3.26)
project(MHY_Scanner
	VERSION 1.1.15
	LANGUAGES CXX
)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(UNIT_TESTS "Unit Tests" OFF)
option(DEV "DEV" OFF)
# 扫码自测工具：不依赖 Qt，直接对图片跑一遍新的扫码逻辑
option(SCAN_TEST "Build standalone QR scan test tool" OFF)

add_definitions(-DMHY_Scanner_VERSION="${PROJECT_VERSION}")

if(MSVC)
	set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
	add_compile_options(/Zi /sdl /arch:AVX2 /utf-8)
	#add_compile_options("/analyze:external-")
	if(CMAKE_BUILD_TYPE STREQUAL "Release")
		add_link_options(/DEBUG /OPT:REF,ICF,LBR)
	endif()
endif()

include(FetchContent)
include(ExternalProject)
include(cmake/install_ffmpeg.cmake)
include(cmake/build_QR-Code-generator.cmake)

FetchContent_Declare(
	nlohmann_json
	GIT_REPOSITORY https://github.com/nlohmann/json.git
	GIT_TAG v3.12.0
)
FetchContent_MakeAvailable(nlohmann_json)

find_package(unofficial-webview2 CONFIG REQUIRED)

find_package(cpr CONFIG REQUIRED)

find_package(OpenCV REQUIRED opencv_world)
include_directories(${OpenCV_INCLUDE_DIRS})

set(OPENSSL_USE_STATIC_LIBS TRUE)
find_package(OpenSSL REQUIRED)

find_package(Qt6 COMPONENTS Widgets REQUIRED)

#boost
include_directories(${CMAKE_SOURCE_DIR}/3rdparty/boost)

#include_directories(./3rdparty/abogus_cpp)
#link_directories(./3rdparty/abogus_cpp)

if(NOT DEV)
	add_definitions(-DTESTSPEED)
	add_definitions(-DSHOW)
endif()

include_directories(src/Core)

add_executable(${PROJECT_NAME}
	"${CMAKE_SOURCE_DIR}/src/Resources/MHY_Scanner.rc"
	"${CMAKE_SOURCE_DIR}/src/Resources/MHY_Scanner.qrc" 
	"${CMAKE_SOURCE_DIR}/src/UI/Main.cpp" 
	"${CMAKE_SOURCE_DIR}/src/UI/QRCodeForStream.cpp"
	"${CMAKE_SOURCE_DIR}/src/UI/QRCodeForStream.h"
	"${CMAKE_SOURCE_DIR}/src/UI/QRCodeForScreen.cpp" 
	"${CMAKE_SOURCE_DIR}/src/UI/QRCodeForScreen.h" 
	"${CMAKE_SOURCE_DIR}/src/UI/WindowAbout.cpp" 
	"${CMAKE_SOURCE_DIR}/src/UI/WindowAbout.h" 
	"${CMAKE_SOURCE_DIR}/src/UI/WindowMain.cpp" 
	"${CMAKE_SOURCE_DIR}/src/UI/WindowMain.h" 
	"${CMAKE_SOURCE_DIR}/src/UI/WindowLogin.h"
	"${CMAKE_SOURCE_DIR}/src/UI/WindowLogin.cpp" 
	"${CMAKE_SOURCE_DIR}/src/UI/WindowGeeTest.h" 
	"${CMAKE_SOURCE_DIR}/src/UI/WindowGeeTest.cpp"
	"${CMAKE_SOURCE_DIR}/src/Core/CryptoKit.cpp"
	"${CMAKE_SOURCE_DIR}/src/Core/CryptoKit.h"
	"${CMAKE_SOURCE_DIR}/src/Core/LiveStreamLink.cpp"
	"${CMAKE_SOURCE_DIR}/src/Core/LiveStreamLink.h"
	"${CMAKE_SOURCE_DIR}/src/Core/QRScanner.cpp"
	"${CMAKE_SOURCE_DIR}/src/Core/QRScanner.h"
	"${CMAKE_SOURCE_DIR}/src/Core/ScreenScan.cpp"
	"${CMAKE_SOURCE_DIR}/src/Core/ScreenScan.h"
	"${CMAKE_SOURCE_DIR}/src/Core/ConfigDate.cpp"
	"${CMAKE_SOURCE_DIR}/src/Core/ConfigDate.h"
	"${CMAKE_SOURCE_DIR}/src/Core/BSGameSDK.hpp"
	"${CMAKE_SOURCE_DIR}/src/Core/CookieParser.hpp"
	"${CMAKE_SOURCE_DIR}/src/Core/CreateUUID.hpp"
	"${CMAKE_SOURCE_DIR}/src/Core/ScannerBase.hpp"
	"${CMAKE_SOURCE_DIR}/src/Core/compile_string.hpp"
	"${CMAKE_SOURCE_DIR}/src/Core/ApiDefs.hpp"
)

if(NOT DEV)
	#target_link_options(${PROJECT_NAME} PRIVATE "/SUBSYSTEM:WINDOWS")
	set_target_properties(${PROJECT_NAME} PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

set(PROJECT_LIBRARIES
	# windows
	Ws2_32
	wldap32
	Normaliz
	Dbghelp

	# Qt6
	Qt6::Core
	Qt6::Widgets

	cpr::cpr
	QR_Code_generator
	unofficial::webview2::webview2
	nlohmann_json::nlohmann_json
	${OpenCV_LIBS}
	${FFMPEG_LIBS}
	OpenSSL::Crypto
)

target_link_libraries(${PROJECT_NAME} PRIVATE ${PROJECT_LIBRARIES})

file(GLOB FFMPEG_DLL_FILES
	"${FFMPEG_BIN_DIR}/avcodec-*.dll"
	"${FFMPEG_BIN_DIR}/avformat-*.dll"
	"${FFMPEG_BIN_DIR}/avutil-*.dll"
	"${FFMPEG_BIN_DIR}/swscale-*.dll"
	"${FFMPEG_BIN_DIR}/swresample-*.dll"
)

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
	${FFMPEG_DLL_FILES}
	"$<TARGET_FILE_DIR:${PROJECT_NAME}>"
)

# ---------------------------------------------------------------------------
# ZBar 运行库：扫码增强模块在运行时动态加载，把 dll 放到 third_party/zbar/ 即可
# 自动随构建拷贝到输出目录；不需要新增任何 vcpkg/编译期依赖。
# ---------------------------------------------------------------------------
set(ZBAR_RUNTIME_DLLS
	"${CMAKE_SOURCE_DIR}/third_party/zbar/libzbar64-0.dll"
	"${CMAKE_SOURCE_DIR}/third_party/zbar/zbar-0.dll"
	"${CMAKE_SOURCE_DIR}/third_party/zbar/zlib1.dll"
	"${CMAKE_SOURCE_DIR}/third_party/zbar/libiconv.dll"
	"${CMAKE_SOURCE_DIR}/third_party/zbar/iconv-2.dll"
)

foreach(ZBAR_DLL ${ZBAR_RUNTIME_DLLS})
	if(EXISTS "${ZBAR_DLL}")
		add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E copy_if_different
			"${ZBAR_DLL}"
			"$<TARGET_FILE_DIR:${PROJECT_NAME}>"
		)
	endif()
endforeach()

# unit tests
if(UNIT_TESTS)
	enable_testing()
	add_subdirectory(tests)
	target_link_libraries(test_core PRIVATE ${PROJECT_LIBRARIES})
	target_link_libraries(test_gui PRIVATE ${PROJECT_LIBRARIES})
	target_link_libraries(test_video PRIVATE ${PROJECT_LIBRARIES})
	target_link_libraries(test_video_hw PRIVATE ${PROJECT_LIBRARIES})
	target_link_libraries(test_streamlink PRIVATE ${PROJECT_LIBRARIES})
endif()

# 独立的扫码自测工具（不依赖 Qt）
if(SCAN_TEST)
	add_executable(scan_test
		"${CMAKE_SOURCE_DIR}/tools/scan_test.cpp"
		"${CMAKE_SOURCE_DIR}/src/Core/QRScanner.cpp"
	)
	target_include_directories(scan_test PRIVATE "${CMAKE_SOURCE_DIR}/src/Core")
	target_link_libraries(scan_test PRIVATE ${OpenCV_LIBS})
	foreach(ZBAR_DLL ${ZBAR_RUNTIME_DLLS})
		if(EXISTS "${ZBAR_DLL}")
			add_custom_command(TARGET scan_test POST_BUILD
				COMMAND ${CMAKE_COMMAND} -E copy_if_different
				"${ZBAR_DLL}"
				"$<TARGET_FILE_DIR:scan_test>"
			)
		endif()
	endforeach()
endif()

# install
set(Install_Directory "${CMAKE_INSTALL_PREFIX}/${PROJECT_NAME}_${PROJECT_VERSION}")

install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION ${Install_Directory})

install(FILES $<TARGET_PDB_FILE:${PROJECT_NAME}> DESTINATION ${Install_Directory})

set(QT_DLL_FILES
	${QT6_INSTALL_PREFIX}/bin/Qt6Core.dll
	${QT6_INSTALL_PREFIX}/bin/Qt6Widgets.dll
	${QT6_INSTALL_PREFIX}/bin/Qt6Gui.dll
)

install(
	FILES 
	${QT_DLL_FILES}
	${FFMPEG_DLL_FILES}
	DESTINATION 
	${Install_Directory}
)

install(
	FILES
	${QT6_INSTALL_PREFIX}/plugins/platforms/qdirect2d.dll
	${QT6_INSTALL_PREFIX}/plugins/platforms/qminimal.dll
	${QT6_INSTALL_PREFIX}/plugins/platforms/qoffscreen.dll
	${QT6_INSTALL_PREFIX}/plugins/platforms/qwindows.dll
	DESTINATION
	${Install_Directory}/plugins/platforms
)

install(
	FILES
	${QT6_INSTALL_PREFIX}/plugins/styles/qmodernwindowsstyle.dll
	DESTINATION
	${Install_Directory}/plugins/styles
)

# 若准备了 ZBar 运行库则一并安装
set(ZBAR_INSTALL_FILES)
foreach(ZBAR_DLL ${ZBAR_RUNTIME_DLLS})
	if(EXISTS "${ZBAR_DLL}")
		list(APPEND ZBAR_INSTALL_FILES "${ZBAR_DLL}")
	endif()
endforeach()
if(ZBAR_INSTALL_FILES)
	install(FILES ${ZBAR_INSTALL_FILES} DESTINATION ${Install_Directory})
endif()

install(
	DIRECTORY
	ScanModel
	DESTINATION
	${Install_Directory}
)
