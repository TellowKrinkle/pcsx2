set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

add_executable(pcsx2-qt)

if (PACKAGE_MODE)
	install(TARGETS pcsx2-qt DESTINATION ${CMAKE_INSTALL_BINDIR})
else()
	install(TARGETS pcsx2-qt DESTINATION ${CMAKE_SOURCE_DIR}/bin)
endif()

target_sources(pcsx2-qt PRIVATE
	Settings/BIOSSettingsWidget.cpp
	Settings/BIOSSettingsWidget.h
	Settings/BIOSSettingsWidget.ui
	Settings/DebugSettingsDialog.cpp
	Settings/DebugSettingsDialog.h
	Settings/DebugSettingsDialog.ui
	Settings/DisplaySettingsWidget.cpp
	Settings/DisplaySettingsWidget.h
	Settings/DisplaySettingsWidget.ui
	Settings/EmulationSettingsWidget.cpp
	Settings/EmulationSettingsWidget.h
	Settings/EmulationSettingsWidget.ui
	Settings/GameListSettingsWidget.cpp
	Settings/GameListSettingsWidget.h
	Settings/GameListSettingsWidget.ui
	Settings/GraphicsSettingsWidget.cpp
	Settings/GraphicsSettingsWidget.h
	Settings/GraphicsSettingsWidget.ui
	Settings/GraphicsDebugSettingsWidget.cpp
	Settings/GraphicsDebugSettingsWidget.h
	Settings/GraphicsDebugSettingsWidget.ui
	Settings/InterfaceSettingsWidget.cpp
	Settings/InterfaceSettingsWidget.h
	Settings/InterfaceSettingsWidget.ui
	Settings/OSDSettingsWidget.cpp
	Settings/OSDSettingsWidget.h
	Settings/OSDSettingsWidget.ui
	Settings/SettingsDialog.cpp
	Settings/SettingsDialog.h
	Settings/SettingsDialog.ui
	Settings/SystemDebugSettingsWidget.cpp
	Settings/SystemDebugSettingsWidget.h
	Settings/SystemDebugSettingsWidget.ui
	Settings/SystemSettingsWidget.cpp
	Settings/SystemSettingsWidget.h
	Settings/SystemSettingsWidget.ui
	DisplayWidget.cpp
	DisplayWidget.h
	EmuThread.cpp
	EmuThread.h
	GameListModel.cpp
	GameListModel.h
	GameListRefreshThread.cpp
	GameListRefreshThread.h
	GameListWidget.cpp
	GameListWidget.h
	Main.cpp
	MainWindow.cpp
	MainWindow.h
	MainWindow.ui
	PrecompiledHeader.cpp
	PrecompiledHeader.h
	QtHost.cpp
	QtHost.h
	QtKeyCodes.cpp
	QtUtils.cpp
	QtUtils.h
	SettingWidgetBinder.h
	resources/resources.qrc
)

target_precompile_headers(pcsx2-qt PRIVATE PrecompiledHeader.h)

target_include_directories(pcsx2-qt PRIVATE
	${Qt5Gui_PRIVATE_INCLUDE_DIRS}
	"${CMAKE_BINARY_DIR}/common/include"
	"${CMAKE_SOURCE_DIR}/pcsx2"
	"${CMAKE_SOURCE_DIR}/pcsx2-qt"
)

target_link_libraries(pcsx2-qt PRIVATE
	PCSX2_FLAGS
	PCSX2
	common
	yaml-cpp
	simpleini
	wx30_base
	Qt5::Core
	Qt5::Gui
	Qt5::Widgets
	Qt5::Network
)
