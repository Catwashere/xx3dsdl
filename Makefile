SYS := ${shell uname -s}
ARC := ${shell uname -m}
VER := 1.0.14
TAR := libftd3xx-linux-arm-v6-hf-${VER}.tgz
ICONSRC=icon.png
ICONSET=AppIcon.iconset
ifeq (${SYS}, Darwin)
	USR := ${shell logname}
	GRP := admin
	EXT := dylib
	LIB := libftd3xx.${VER}.${EXT}
	UPD := true
	TAR := d3xx-osx.${VER}.tgz
	ZIP := tar -xzf temp/${TAR} --strip-components 1 -C temp
	USB := true
else ifeq (${SYS}, Linux)
	USR := root
	GRP := root
	EXT := so
	LIB := libftd3xx.${EXT}.$(VER)
	UPD := ldconfig /usr/local/lib
	ifeq (${ARC}, $(filter aarch% arm%, ${ARC}))
		ifeq (${ARC}, $(filter arm64% %64 armv8% %v8, ${ARC}))
			TAR := libftd3xx-linux-arm-v8-${VER}.tgz
		else ifeq (${ARC}, $(filter armv7% %v7, ${ARC}))
			TAR := libftd3xx-linux-arm-v7_32-${VER}.tgz
		endif
	else
		ifeq (${ARC}, $(filter %64, ${ARC}))
			TAR := libftd3xx-linux-x86_64-${VER}.tgz
		else ifeq (${ARC}, $(filter %32 %86, ${ARC}))
			TAR := libftd3xx-linux-x86_32-${VER}.tgz
		endif
	endif
	ZIP := tar -xzf temp/${TAR} --strip-components 1 -C temp
	USB := install -m 755 -o ${USR} -g ${GRP} -d /etc/udev/rules.d && install -m 644 -o ${USR} -g ${GRP} temp/51-ftd3xx.rules /etc/udev/rules.d && udevadm control --reload-rules && udevadm trigger
endif

xx3dsdl: lodepng.o execpath.o xx3dsdl.o
ifeq (${SYS}, Darwin)
	${CXX} lodepng.o execpath.o xx3dsdl.o -o xx3dsdl -pthread -lftd3xx `sdl2-config --libs` -framework OpenGL
else
	${CXX} lodepng.o execpath.o xx3dsdl.o -o xx3dsdl -pthread -lftd3xx `sdl2-config --libs` -lGL
endif

download_lodepng:
	@if [ ! -f lodepng.cpp ]; then \
		curl -L https://raw.githubusercontent.com/lvandeve/lodepng/master/lodepng.cpp -o lodepng.cpp; \
	fi
	@if [ ! -f lodepng.h ]; then \
		curl -L https://raw.githubusercontent.com/lvandeve/lodepng/master/lodepng.h -o lodepng.h; \
	fi

lodepng.o: download_lodepng lodepng.cpp 
	${CXX} -std=c++17 -c lodepng.cpp -o lodepng.o

xx3dsdl.o: xx3dsdl.cpp
	${CXX} -std=c++17 -c xx3dsdl.cpp -o xx3dsdl.o `sdl2-config --cflags`

execpath.o: execpath.cpp execpath.h
	${CXX} -std=c++17 -c execpath.cpp -o execpath.o

clean_deps:
	rm -rf lodepng.* execpath.o

clean: clean_deps
	rm -rf xx3dsdl *.o *.app

ftd3xx:
	curl --create-dirs https://ftdichip.com/wp-content/uploads/2023/06/${TAR} -o temp/${TAR}
	${ZIP}
	${USB}
	install -m 755 -o ${USR} -g ${GRP} -d /usr/local/include/ftd3xx && install -m 644 -o ${USR} -g ${GRP} temp/*.h /usr/local/include/ftd3xx
	install -m 755 -o ${USR} -g ${GRP} -d /usr/local/lib && install -m 755 -o ${USR} -g ${GRP} temp/${LIB} /usr/local/lib && ln -sf /usr/local/lib/${LIB} /usr/local/lib/libftd3xx.${EXT} && ${UPD}
	rm -rf temp

install: ftd3xx xx3dsdl
	install -m 755 -o ${USR} -g ${GRP} -d /usr/local/bin && install -m 755 -o ${USR} -g ${GRP} xx3dsdl /usr/local/bin

uninstall:
	rm -rf /etc/udev/rules.d/51-ftd3xx.rules /usr/local/bin/xx3dsdl /usr/local/include/ftd3xx /usr/local/lib/libftd3xx.*

update:
	curl --create-dirs https://raw.githubusercontent.com/Catwashere/xx3dsdl/main/{LICENSE,Makefile,README.md,xx3dsdl.cpp,blank.png,execpath.cpp,execpath.h,icon.png} -o "#1"

app: xx3dsdl
ifeq (${SYS}, Darwin)
	# copy these dynamic libs and inter-deps over to be bundled, change perms
	cp /usr/local/lib/libftd3xx.dylib .
	cp /usr/local/lib/libSDL2.dylib .
	chmod 777 *.dylib
	chmod +x xx3dsdl

	# edit binary to have ftd3xx and sdl relatively linked (can check with otool -L xx3dsdl to see current dylib paths)
	install_name_tool -change libftd3xx.dylib  @executable_path/libftd3xx.dylib  xx3dsdl
	install_name_tool -change @rpath/libSDL2.dylib @executable_path/libsdl-audio.2.6.dylib xx3dsdl

	# make Info.plist with version from configure.ac
	VERSION=v0.0.1

	# Build xx3dsdl bundle
	echo '<?xml version="1.0" encoding="UTF-8"?>' > Info.plist
	echo '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">' >> Info.plist
	echo '<plist version="1.0">' >> Info.plist
	echo '	<dict>' >> Info.plist
	echo '		<key>CFBundleExecutable</key>' >> Info.plist
	echo '		<string>xx3dsdl</string>' >> Info.plist
	echo '		<key>CFBundleIconFile</key>' >> Info.plist
	echo '		<string>AppIcon</string>' >> Info.plist
	echo '		<key>CFBundleIconName</key>' >> Info.plist
	echo '		<string>AppIcon</string>' >> Info.plist
	echo '		<key>CFBundleIdentifier</key>' >> Info.plist
	echo '		<string>com.catwashere.xx3dsdl</string>' >> Info.plist
	echo '		<key>CFBundleVersion</key>' >> Info.plist
	echo '		<string>v0.0.1</string>' >> Info.plist
	echo '		<key>CFBundleDisplayName</key>' >> Info.plist
	echo '		<string>xx3dsdl</string>' >> Info.plist
	echo '		<key>LSRequiresIPhoneOS</key>' >> Info.plist
	echo '		<string>false</string>' >> Info.plist
	echo '		<key>NSHighResolutionCapable</key>' >> Info.plist
	echo '		<string>true</string>' >> Info.plist
	echo '	</dict>' >> Info.plist
	echo '</plist>' >> Info.plist

	# Create App Icon
	mkdir -p ${ICONSET}
	sips -z 16 16     "${ICONSRC}" --out "${ICONSET}/icon_16x16.png"
	sips -z 32 32     "${ICONSRC}" --out "${ICONSET}/icon_16x16@2x.png"
	sips -z 32 32     "${ICONSRC}" --out "${ICONSET}/icon_32x32.png"
	sips -z 64 64     "${ICONSRC}" --out "${ICONSET}/icon_32x32@2x.png"
	sips -z 128 128   "${ICONSRC}" --out "${ICONSET}/icon_128x128.png"
	sips -z 256 256   "${ICONSRC}" --out "${ICONSET}/icon_128x128@2x.png"
	sips -z 256 256   "${ICONSRC}" --out "${ICONSET}/icon_256x256.png"
	sips -z 512 512   "${ICONSRC}" --out "${ICONSET}/icon_256x256@2x.png"
	sips -z 512 512   "${ICONSRC}" --out "${ICONSET}/icon_512x512.png"
	cp "${ICONSRC}" "${ICONSET}/icon_512x512@2x.png"  # Original is 1024x1024
	iconutil -c icns ${ICONSET} -o AppIcon.icns
	rm -rf ${ICONSET}

	# Create App Bundle
	mkdir -p xx3dsdl.app/Contents/MacOS xx3dsdl.app/Contents/Resources xx3dsdl.app/Contents/Resources
	mv AppIcon.icns xx3dsdl.app/Contents/Resources
	mv xx3dsdl *.dylib xx3dsdl.app/Contents/MacOS
	mv Info.plist xx3dsdl.app/Contents
	cp blank.png xx3dsdl.app/Contents/MacOS
	make clean_deps
else
	@echo "Not supported on this platform"
endif