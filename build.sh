#!/bin/bash

# Setup build environment
BUILDHOME=/android/src/subZero
BUILDLOG=$BUILDHOME/release/build/build.log
export ANDROID_BUILD_TOP=/android/src/aokp
export ARCH=arm
export CROSS_COMPILE=$ANDROID_BUILD_TOP/prebuilt/linux-x86/toolchain/arm-eabi-4.4.3/bin/arm-eabi-
#export CCACHE=1
export CPUS=`grep 'processor' /proc/cpuinfo | wc -l`

# Set the kernel version
VERSION="1.3"

# Set the phone model.  Default is "vibrant"
if [ $# -lt 1 ]
then
	MODEL=vibrant
else
	MODEL=$1
fi

# Check whether I/O Scheduler is BFS or CFS
SCHED=`grep CONFIG_SCHED_BFS= .config | awk -F= '{print $2}'`
if [[ ${SCHED} = "y" ]]
then
	EXTRA=BFS
else
	EXTRA=CFS
fi

# Check if Voodoo Color is enabled
VOODOO=`grep CONFIG_FB_VOODOO= .config | awk -F= '{print $2}'`
if [[ ${VOODOO} = "y" ]]
then
	# Now check if this is a MIUI build
	if [ `grep MIUI .config` ]
	then
		EXTRA=${EXTRA}_MIUI
	else
		EXTRA=${EXTRA}_VOODOO
	fi
fi

# Check whether BLNv9 is in use
BLN=`grep BLN .config | awk -F= '{print $2}'`
if [[ ${BLN} = "y" ]]
then
	EXTRA=${EXTRA}_BLN
else
	EXTRA=${EXTRA}_LED
fi

# Check whether we are using FATRAM
FATRAM=`grep FATRAM= .config | awk -F= '{print $2}'`
if [[ ${FATRAM} = "y" ]]
then
	EXTRA=${EXTRA}_FAT
fi

# The Beginning
clear
cd $BUILDHOME
if [ -f $BUILDLOG ]; then mv $BUILDLOG $BUILDLOG.old; fi

# Setup the kernel's 'system' directory
cp -rp release/build/system.kernel release/build/system

# Ready, Set, GO!
STARTTIME=`date`
START=`date +%s`
echo "Makin' Bacon. Please be patient..." | tee -a $BUILDLOG
make -j$CPUS -s >> $BUILDLOG 2>&1

if [ $? -ne 0 ]
then
  echo "Make failed." | tee -a $BUILDLOG
  END=`date +%s`
  ELAPSED=`expr $END - $START`
  echo "Total Elapsed Time = $ELAPSED seconds" | tee -a $BUILDLOG
  echo "End Time: `date`" | tee -a $BUILDLOG
  exit
fi
  
# Create the appropriate directory for the final kernel
mkdir -p release/${MODEL}

# Set the build version
BUILDVER=`cat .version`

# Include the appropriate liblights library
if [[ ${BLN} = "y" ]]
then
  cp release/build/liblights/lights.aries.so.bln release/build/system/lib/hw/lights.aries.so
else
  cp release/build/liblights/lights.aries.so.cmled release/build/system/lib/hw/lights.aries.so
fi

# Generate the release string for the CWM flashable zip file
RELEASE=subZero-${MODEL}-${VERSION}_build${BUILDVER}-${EXTRA}

# Generate the "boot.img" kernel
/android/src/aries-common/mkshbootimg.py release/build/boot.img arch/arm/boot/zImage release/ramdisks/ramdisk.img release/ramdisks/ramdisk-recovery.img >> $BUILDLOG 2>&1

# Copy the modules
find . -name "*.ko" -exec cp {} release/build/system/lib/modules/ \; >> $BUILDLOG 2>&1

cd release/build

# Get the appropriate updater-script
UPDATER=updater-script.template
cp scripts/$UPDATER META-INF/com/google/android/updater-script

# Fix-up script details
perl -p -i -e "s/VERSION/${VERSION}/" META-INF/com/google/android/updater-script
perl -p -i -e "s/EXTRA/${EXTRA}/" META-INF/com/google/android/updater-script

# Make the CWM flashable zip
echo "Creating subZero kernel package..." | tee -a $BUILDLOG
7za a -r cwm-${RELEASE}.zip system cleanup boot.img META-INF bml_over_mtd bml_over_mtd.sh >> $BUILDLOG 2>&1
rm -r system

# Make the AriesParts package
echo "Creating AriesParts add-on package..." | tee -a $BUILDLOG
cp scripts/updater-script.ariesparts META-INF/com/google/android/updater-script
cp -rp system.ariesparts system
7za a -r cwm-ariesparts.zip system META-INF >> $BUILDLOG 2>&1
rm -r system

# Make the Heimdall 1.3 package
echo "Creating Heimdall kernel package..." | tee -a $BUILDLOG
cd heimdall
cp -p ../boot.img zImage
tar -czf ../heimdall-${RELEASE}.tar.gz Vibrant.pit firmware.xml zImage
rm zImage
cd ..

# Generate file checksums for file intregrity checks
echo "Generating file checksums..." | tee -a $BUILDLOG
for package in *${RELEASE}*
do
  SIZE=`du -h $package | awk '{print $1}'`
  MD5=`md5sum $package | awk '{print $1}'`
  SHA256=`sha256sum $package | awk '{print $1}'`
  echo "FILE:      $package ($SIZE)" > ../${MODEL}/$package.hash | tee -a $BUILDLOG
  echo "MD5SUM:    $MD5" >> ../${MODEL}/$package.hash | tee -a $BUILDLOG
  echo "SHA256SUM: $SHA256" >> ../${MODEL}/$package.hash | tee -a $BUILDLOG
  echo >> ../${MODEL}/$package.hash | tee -a $BUILDLOG
done

for package in cwm-ariesparts.zip
do
  SIZE=`du -h $package | awk '{print $1}'`
  MD5=`md5sum $package | awk '{print $1}'`
  SHA256=`sha256sum $package | awk '{print $1}'`
  echo "FILE:      $package ($SIZE)" > ../${MODEL}/$package.hash | tee -a $BUILDLOG
  echo "MD5SUM:    $MD5" >> ../${MODEL}/$package.hash | tee -a $BUILDLOG
  echo "SHA256SUM: $SHA256" >> ../${MODEL}/$package.hash | tee -a $BUILDLOG
  echo >> ../${MODEL}/$package.hash | tee -a $BUILDLOG
done

# Move the finished packages to the appropriate pickup directory
echo "Moving packages to final destination..." | tee -a $BUILDLOG
mv *${RELEASE}* ../${MODEL} >> $BUILDLOG 2>&1
mv cwm-ariesparts.zip ../${MODEL} >> $BUILDLOG 2>&1
echo "Bacon has been cooked." | tee -a $BUILDLOG

# Cleanup
echo "Cleaning up..." | tee -a $BUILDLOG
rm META-INF/com/google/android/updater-script system/lib/modules/* system/lib/hw/lights.aries.so >> $BUILDLOG 2>&1

# The End
END=`date +%s`
ENDTIME=`date`
ELAPSED=`expr $END - $START`
echo "Build complete!" | tee -a $BUILDLOG
echo | tee -a $BUILDLOG
echo "Elapsed Build Time = $ELAPSED seconds" | tee -a $BUILDLOG
echo "Start Time: $STARTTIME   End Time: $ENDTIME" | tee -a $BUILDLOG
echo | tee -a $BUILDLOG
echo "Look in release/${MODEL} for kernel" | tee -a $BUILDLOG
echo | tee -a $BUILDLOG
cat ../${MODEL}/*.hash | tee -a $BUILDLOG
